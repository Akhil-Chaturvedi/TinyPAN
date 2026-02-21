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

## Memory Footprint (BNEP Native Mode)

The library targets constrained embedded platforms and avoids heap allocation. The TX path checks whether the radio is ready, the queue is empty, and the outgoing pbuf is a single contiguous segment. If all three conditions hold, it manipulates the original `pbuf` in-place: strips the Ethernet header via `pbuf_remove_header`, claims BNEP headroom via `pbuf_add_header`, writes the BNEP header, sends to the HAL, and reverts the pbuf before returning to lwIP. This fast path allocates zero memory and copies zero bytes.

If any condition fails (radio busy, queue non-empty, or chained pbuf), the pbuf is cloned into a contiguous `PBUF_RAM` block, encapsulated with the BNEP header, and placed into an internal TX queue (ring buffer, default 16 slots). Queued frames are drained automatically when the radio signals readiness via `HAL_L2CAP_EVENT_CAN_SEND_NOW`. In BLE SLIP mode, the queue holds raw escaped SLIP packets to survive radio latency, and an internal 1700-byte `s_rx_queue` buffer absorbs the byte stream.

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
│   ├── tinypan_config.h     # Compile-time configuration (timeouts, queue sizes, mode select)
│   ├── tinypan_hal.h        # Hardware Abstraction Layer interface
│   ├── lwipopts.h           # lwIP configuration overrides
│   └── arch/                # lwIP architecture port (cc.h)
├── src/
│   ├── tinypan.c            # Initialization, event routing, timeout bridge
│   ├── tinypan_bnep.c       # BNEP protocol: frame building, parsing, state machine
│   ├── tinypan_supervisor.c # Connection supervisor (IDLE -> CONNECTING -> DHCP -> ONLINE)
│   ├── tinypan_lwip_netif.c # lwIP netif driver (TX queue, SLIP/BNEP routing, pbuf handling)
│   └── tinypan_internal.h   # Internal cross-module prototypes
├── ports/
│   ├── esp32_classic/       # Reference HAL for ESP-IDF (Bluedroid, BT Classic)
│   └── zephyr_ble/          # Reference HAL for Zephyr RTOS (NUS, BLE SLIP)
├── tools/
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

1. **`hal_get_tick_ms()`** -- Return a monotonic millisecond counter (e.g., `xTaskGetTickCount() * portTICK_PERIOD_MS` on FreeRTOS, `k_uptime_get()` on Zephyr).
2. **`hal_bt_l2cap_send(data, len)`** -- Transmit a contiguous buffer over the Bluetooth channel. In BNEP mode this carries a BNEP header + payload over Classic L2CAP (PSM 0x000F). In SLIP mode this carries raw SLIP-escaped bytes over a BLE characteristic.
3. **`hal_bt_l2cap_connect(addr, psm, local_mtu)`** -- Initiate a connection. For BNEP, `local_mtu` should be at least 1691. For BLE SLIP, this may be a no-op if the MCU acts as a peripheral waiting for the phone to connect.
4. **`hal_bt_l2cap_can_send()`** -- Return whether the channel can accept a new frame.
5. **`hal_bt_l2cap_request_can_send_now()`** -- Request a `HAL_L2CAP_EVENT_CAN_SEND_NOW` callback when the channel becomes writable.
6. **Receive path** -- When the Bluetooth stack receives data, call the registered receive callback (see `hal_bt_l2cap_register_recv_callback`). In BNEP mode this delivers raw L2CAP frames. In SLIP mode this delivers raw BLE UART bytes.

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

## Design Constraints

- **Single-threaded.** There is no internal synchronization. All calls to TinyPAN and all HAL callbacks must run in the same execution context. See the RTOS Threading section above.

- **BNEP TX fast path (Mode A only).** When the radio is ready, the queue is empty, and the pbuf is contiguous (`p->next == NULL`), the netif manipulates the original `pbuf` in-place (strips 14-byte Ethernet header, adds 15-byte BNEP header, sends, reverts) without allocating or copying. Chained pbufs and busy-radio conditions fall through to the slow path which clones into `PBUF_RAM` before queuing. In SLIP mode (Mode B), `slipif_output` handles framing and the queue holds escaped SLIP bytes directly.

  > **Note (BNEP mode only):** The in-place header swap shifts the buffer pointer by -1 byte (14 removed, 15 added). If the HAL's DMA requires 4-byte aligned source pointers, `hal_bt_l2cap_send` must bounce the buffer. Check alignment with `((uintptr_t)data & 3)`. The ESP-IDF reference port in `ports/esp32_classic/` demonstrates this.

- **TX queue is bounded.** The queue holds up to `TINYPAN_TX_QUEUE_LEN` (default 16) packets. If the queue is full, the packet is dropped and `ERR_MEM` is returned to lwIP.

- **BNEP filter requests are declined.** The library responds to BNEP filter set requests with `0x0001` (Unsupported Request). This is permitted by the BNEP specification.

- **Heartbeat is not implemented.** The `TINYPAN_ENABLE_HEARTBEAT` flag and the `heartbeat_interval_ms` / `heartbeat_retries` fields in `tinypan_config_t` are reserved for future use. The supervisor does not act on them.

## License

MIT License.
