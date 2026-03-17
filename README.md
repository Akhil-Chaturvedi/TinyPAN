# TinyPAN

TinyPAN is a C library that implements a Bluetooth PAN (Personal Area Network) client for embedded systems. It provides two operating modes: native Bluetooth Classic tethering via BNEP, and BLE-based tethering via SLIP over a UART-style characteristic. Both modes bridge into the lwIP TCP/IP stack so the device can obtain an IP address via DHCP and communicate over IP.

## Architecture: Dual-Mode Connectivity

TinyPAN operates in two distinct modes depending on your hardware capabilities, configurable via `TINYPAN_USE_BLE_SLIP` in `tinypan_config.h`:

### Mode A: Native Bluetooth Classic (BNEP)
For microcontrollers with a Bluetooth Classic or Dual-Mode radio (e.g., ESP32, Raspberry Pi Pico W).
*   **Protocol:** lwIP -> BNEP encapsulation -> BT Classic L2CAP.
*   **Phone side:** Uses the built-in iOS/Android Personal Hotspot. No companion app required.

### Mode B: BLE Companion App (SLIP)
For BLE-only microcontrollers (e.g., nRF52, ESP32-C3/S3, STM32WB) that lack a Classic radio.
*   **Protocol:** lwIP -> SLIP framing -> BLE UART characteristic (e.g., Nordic UART Service).
*   **Phone side:** Requires a companion app that reads SLIP frames from the BLE pipe and injects the contained IPv4 packets into the OS networking stack via `VpnService` (Android) or `NetworkExtension` (iOS).

### Transport Abstraction

Mode selection at compile time determines which transport backend is compiled in, but the core modules (`tinypan_supervisor.c`, `tinypan_lwip_netif.c`) are transport-agnostic at the source level. Each backend implements the `tinypan_transport_t` interface defined in `src/tinypan_transport.h`. This interface consists of function pointers for initialization, connection events, incoming data dispatch, TX queue management, and lwIP output. `tinypan_transport_get()` returns the active backend.

## Memory Management (BNEP Native Mode)

The library targets constrained embedded platforms and strictly avoids dynamic heap allocation. 

### Secure TX Path
To prevent data corruption in lwIP retransmission queues and ensure DMA stability, the BNEP transport (`tinypan_bnep_transport.c`) uses a "Safe Path" for all outgoing data. Every frame is copied into a new, contiguous `pbuf` allocated from the static RAM pool. The BNEP header is prepended to this copy before transmission. This ensures that the original `pbuf` remains untouched by TinyPAN, preserving its integrity for TCP retransmissions or multi-homed routing scenarios.

### Queueing
If the radio is busy (L2CAP queue full), the cloned frame is placed into an internal 3-slot ring buffer. Queued frames are drained automatically when the radio signals readiness via `HAL_L2CAP_EVENT_CAN_SEND_NOW` or during the `tinypan_process()` polling cycle.

In SLIP mode, the SLIP transport backend parses incoming BLE bytes through a streaming SLIP FSM, building lwIP pbufs incrementally as bytes arrive. No intermediate ring buffer is used. When the SLIP `0xC0` END byte is received, the completed pbuf chain is dispatched directly to lwIP.

The core loop is a single-threaded polling pump driven by `tinypan_process()`. For power-sensitive applications, `tinypan_get_next_timeout_ms()` returns the exact number of milliseconds until the next scheduled event (state machine timeout or lwIP timer), allowing the MCU to enter WFI instead of polling.

Compiled size metrics (GCC, x86_64):
* **RAM (bss + data):** ~192 bytes
* **Flash (text):** ~14.5 KB
* **Heap allocation:** None

## Repository Layout

```text
TinyPAN/
├── CMakeLists.txt           # Build configuration (fetches lwIP via FetchContent)
├── include/
│   ├── tinypan.h            # Public API
│   ├── tinypan_config.h     # Compile-time configuration (timeouts, queue sizes)
│   ├── tinypan_hal.h        # Hardware Abstraction Layer interface
│   ├── lwipopts.h           # lwIP configuration overrides
│   └── arch/                # lwIP architecture port (cc.h)
├── src/
│   ├── tinypan.c            # Initialization, event routing, transport dispatch
│   ├── tinypan_bnep.c       # BNEP protocol: frame building, parsing, state machine
│   ├── tinypan_supervisor.c # Connection supervisor (IDLE -> CONNECTING -> BNEP_SETUP -> FILTER_WAIT -> DHCP -> ONLINE)
│   ├── tinypan_lwip_netif.c # lwIP netif driver (delegates TX/RX to active transport)
│   ├── tinypan_transport.h  # Transport interface definition (tinypan_transport_t)
│   ├── tinypan_transport.c  # Transport factory (returns active backend via tinypan_transport_get)
│   ├── tinypan_bnep_transport.c   # BNEP transport backend
│   ├── tinypan_slip_transport.c   # SLIP transport backend
│   └── tinypan_internal.h   # Internal cross-module prototypes
├── ports/
│   ├── esp32_classic/       # Reference HAL for ESP-IDF (Bluedroid, BT Classic)
│   └── zephyr_ble/          # Reference HAL for Zephyr RTOS (NUS, BLE SLIP)
├── tools/
│   ├── tinypan_diag.c       # Optional diagnostic module (link state, IP info)
│   ├── slip_simulator.py    # Python script simulating a SLIP MCU over TCP
│   ├── slip_client.py       # Python script decoding SLIP stream (companion app mock)
│   └── flutter_slip_blueprint.dart # Dart SLIP decoder for Flutter companion app
├── tests/
│   ├── test_bnep.c          # BNEP parser/builder unit tests
│   ├── test_supervisor.c    # State machine and timeout tests
│   ├── test_integration.c   # Full DHCP DORA flow over mock HAL
│   └── dhcp_sim.c/.h        # DHCP packet builder/parser for test simulation
└── hal/
    └── mock/                # Mock HAL for simulation-based testing
```

## Porting to Hardware

To run TinyPAN on real hardware, implement the functions declared in `tinypan_hal.h`:

1.  **`hal_get_tick_ms()`** -- Return a monotonic millisecond counter (e.g., `xTaskGetTickCount() * portTICK_PERIOD_MS` on FreeRTOS, `k_uptime_get()` on Zephyr).
2.  **`hal_bt_l2cap_send(data, len)`** -- Transmit a contiguous buffer over the Bluetooth channel. In BNEP mode this carries a BNEP header + payload over Classic L2CAP (PSM 0x000F). In SLIP mode this carries raw SLIP-escaped bytes over a BLE characteristic.
3.  **`hal_bt_l2cap_connect(addr, psm, local_mtu)`** -- Initiate a connection. For BNEP, `local_mtu` should be at least 1691. For BLE SLIP, this may be a no-op if the MCU acts as a peripheral waiting for the phone to connect.
4.  **`hal_bt_l2cap_can_send()`** -- Return whether the channel can accept a new frame.
5.  **`hal_bt_l2cap_request_can_send_now()`** -- Request a `HAL_L2CAP_EVENT_CAN_SEND_NOW` callback when the channel becomes writable.
6.  **Receive path** -- When the Bluetooth stack receives data, call the registered receive callback (see `hal_bt_l2cap_register_recv_callback`). In BNEP mode this delivers raw L2CAP frames. In SLIP mode this delivers raw BLE UART bytes.

Reference implementations are provided in `ports/`:
*   `ports/esp32_classic/` -- ESP-IDF (Bluedroid) for Bluetooth Classic / BNEP mode.
*   `ports/zephyr_ble/` -- Zephyr RTOS (Nordic UART Service) for BLE / SLIP mode.

### RTOS Threading

TinyPAN is single-threaded and non-reentrant. All calls to TinyPAN API functions and all HAL callbacks must execute in the same thread context as `tinypan_process()`.

If your platform delivers Bluetooth callbacks from a separate task or ISR (this is the case on ESP-IDF, Zephyr, and NimBLE), you must bounce events through an OS message queue and process them in the `tinypan_process()` thread. Calling TinyPAN or lwIP functions from an ISR will corrupt internal queues and state.

```c
/* BT stack callback context (ISR or high-priority task) */
void on_l2cap_rx_from_bt_stack(uint8_t* data, uint16_t len) {
    bt_msg_t msg = { .data = clone_buffer(data, len), .len = len };
    xQueueSend(xTinyPanQueue, &msg, 0);
}

/* Application task */
void app_main_task(void* arg) {
    while (1) {
        tinypan_process();
        uint32_t wait_ms = tinypan_get_next_timeout_ms();
        bt_msg_t msg;
        if (xQueueReceive(xTinyPanQueue, &msg, pdMS_TO_TICKS(wait_ms))) {
            my_registered_tinypan_rx_callback(msg.data, msg.len, user_data);
            free_buffer(msg.data);
        }
    }
}
```

See `hal/mock/` for the simulation HAL and `ports/` for production reference implementations.

## Building

Requires CMake (>= 3.12), a C99 compiler (GCC, Clang, or MSVC), and Ninja or Make.

The CMake configuration uses `FetchContent` to download lwIP (STABLE-2_1_3_RELEASE) and compiles it alongside TinyPAN. The lwIP source list is loaded from lwIP's own `Filelists.cmake` to avoid maintaining a hardcoded file list.

```bash
cmake -S . -B build
cmake --build build
```

## Testing

The test suite runs entirely in simulation using the mock HAL. No Bluetooth hardware is needed.

```bash
ctest --test-dir build -V
```

The suite includes:
- **BNEPTests** — Validates BNEP header construction, parsing, and edge cases.
- **SupervisorTests** — Verifies state machine transitions, timeout handling, and reconnection logic.
- **IntegrationFlowTests** — Runs a complete DHCP DORA handshake: lwIP generates a real DHCP DISCOVER, the test harness responds with a simulated OFFER, lwIP sends a REQUEST, and the harness confirms with an ACK. The test passes when lwIP reports an assigned IP address.

## Memory & Performance
The library targets constrained embedded platforms (32KB-64KB RAM) and minimizes heap usage. 

### BNEP Native Mode (Mode A)
- **Safe Path**: TinyPAN uses a contiguous-copy strategy for BNEP encapsulation. It extracts addresses from the incoming Ethernet frame, calculates the required BNEP header (typically 15 bytes), and copies the IP payload into a pre-allocated contiguous `pbuf`. This ensures the HAL always receives a contiguous buffer and the original stack memory is never mutated.
- **Alignment**: TinyPAN sets `ETH_PAD_SIZE = 1` in `lwipopts.h`. This ensures that after the 1-byte padding, the IP payload remains 4-byte aligned for stack performance. However, due to the 15-byte BNEP header, the start of the final Bluetooth frame is typically at an unaligned offset (`addr % 4 == 1`). HAL implementations must handle this via internal DMA bounce buffers.

### BLE SLIP Mode (Mode B)
- **Complexity**: O(N) streaming parser.
- **Mechanism**: The SLIP transport backend builds `pbuf` chains incrementally as bytes arrive from the BLE UART characteristic. When the `0xC0` END byte is detected, the frame is dispatched to lwIP.

## Design Constraints
- **Concurrency**: Single-threaded and non-reentrant. All API calls and HAL callbacks must execute in the same task context as `tinypan_process()`.
- **Queueing**: TX and Control queues are bounded to prevent heap exhaustion.
- **Protocol Compliance**: Implements BNEP v1.0, DHCP, and ARP. Dynamic multicast filtering (Broadcast, IPv4 Multicast) is supported. IPv6/mDNS filtering is conditionally compiled based on `LWIP_IPV6`.
- **HAL Requirements**: HAL implementations for DMA-driven hardware must provide an internal bounce buffer for unaligned L2CAP payloads.
- **BNEP Robustness**: Control packets (Setup, Filter) are managed via a static internal queue to prevent message loss during rapid state transitions.
- **Header Compression**: Set `TINYPAN_FORCE_UNCOMPRESSED_TX` to 1 if the tethering host has a non-compliant BNEP parser. This forces General Ethernet headers for all outgoing data.
- **Heartbeat**: Reserved for future use. The supervisor currently ignores `heartbeat_interval_ms` and `heartbeat_retries`.


## License

MIT License.
