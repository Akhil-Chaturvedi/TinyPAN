# TinyPAN

TinyPAN is a lightweight C library that implements a Bluetooth PAN (Personal Area Network) client. It connects an embedded device to a phone's Bluetooth tethering, bridging BNEP frames into the lwIP TCP/IP stack so the device can obtain an IP address via DHCP and communicate over IP.

## Architecture

The library targets constrained embedded platforms (ESP32, nRF52, etc.) and avoids heap allocation. The TX path checks whether the radio is ready, the queue is empty, and the outgoing pbuf is a single contiguous segment. If all three conditions hold, it manipulates the original `pbuf` in-place: strips the Ethernet header via `pbuf_remove_header`, claims BNEP headroom via `pbuf_add_header`, writes the BNEP header, sends to the HAL, and reverts the pbuf before returning to lwIP. This fast path allocates zero memory and copies zero bytes.

If any condition fails (radio busy, queue non-empty, or chained pbuf), the pbuf is cloned into a contiguous `PBUF_RAM` block, encapsulated with the BNEP header, and placed into an internal TX queue (ring buffer, default 8 slots). Queued frames are drained automatically when the radio signals readiness via `HAL_L2CAP_EVENT_CAN_SEND_NOW`.

The core loop is a single-threaded polling pump driven by `tinypan_process()`. For power-sensitive applications, `tinypan_get_next_timeout_ms()` returns the exact number of milliseconds until the next scheduled event (state machine timeout or lwIP timer), allowing the MCU to enter WFI instead of polling.

Compiled size metrics (GCC, x86_64):
* **RAM (bss + data):** ~192 bytes
* **Flash (text):** ~14.5 KB
* **Heap allocation:** None

## Repository Layout

```text
TinyPAN/
├── CMakeLists.txt         # Build configuration (fetches lwIP via FetchContent)
├── include/
│   ├── tinypan.h          # Public API
│   ├── tinypan_config.h   # Timing and retry parameters
│   ├── tinypan_hal.h      # Hardware Abstraction Layer interface
│   ├── lwipopts.h         # lwIP configuration overrides
│   └── arch/              # lwIP architecture port (cc.h)
├── src/
│   ├── tinypan.c          # Initialization, event routing, timeout bridge
│   ├── tinypan_bnep.c     # BNEP protocol: frame building, parsing, state machine
│   ├── tinypan_supervisor.c # Connection supervisor (IDLE -> CONNECTING -> DHCP -> ONLINE)
│   ├── tinypan_lwip_netif.c # lwIP network interface driver (TX queue, pbuf handling)
│   └── tinypan_internal.h # Internal cross-module prototypes
├── tests/
│   ├── test_bnep.c        # BNEP parser/builder unit tests
│   ├── test_supervisor.c  # State machine and timeout tests
│   ├── test_integration.c # Full DHCP DORA flow over mock HAL
│   └── dhcp_sim.c/.h      # DHCP packet builder/parser for test simulation
└── hal/
    └── mock/              # Mock HAL for simulation-based testing
```

## Porting to Hardware

To run TinyPAN on real hardware, implement the functions declared in `tinypan_hal.h`:

1. **`hal_get_tick_ms()`** — Return a monotonic millisecond counter (e.g., `xTaskGetTickCount() * portTICK_PERIOD_MS` on FreeRTOS).
2. **`hal_bt_l2cap_send(data, len)`** — Transmit a single contiguous buffer over the L2CAP channel assigned to BNEP (PSM 0x000F). The buffer contains both the BNEP header and IP payload.
3. **`hal_bt_l2cap_connect(addr, psm, local_mtu)`** — Initiate an L2CAP connection. `local_mtu` should be at least 1691 for BNEP.
4. **`hal_bt_l2cap_can_send()`** — Return whether the L2CAP channel can accept a new frame.
5. **`hal_bt_l2cap_request_can_send_now()`** — Request a `HAL_L2CAP_EVENT_CAN_SEND_NOW` callback when the channel becomes writable.
6. **Receive path** — When the Bluetooth stack receives L2CAP data on the BNEP PSM, call the registered receive callback (see `hal_bt_l2cap_register_recv_callback`).

See `hal/mock/` for a reference implementation.

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

- **Single-threaded.** All calls to TinyPAN functions and the HAL receive callback must run in the same execution context. There is no internal synchronization. If your Bluetooth stack delivers L2CAP callbacks from a hardware ISR or high-priority RTOS task (e.g., ESP-IDF Bluedroid, Zephyr, NimBLE), you must bounce these events through an OS message queue and process them in the same context as `tinypan_process()`. Calling TinyPAN or lwIP functions directly from an ISR will corrupt internal queues and state.

- **TX path uses in-place header swap on the fast path.** When the radio is ready, the queue is empty, and the pbuf is a single contiguous segment (`p->next == NULL`), `tinypan_netif_linkoutput` manipulates lwIP's original `pbuf` in-place (strip Ethernet header, add BNEP header, send, revert) with zero allocations or copies. Chained pbufs (e.g., from TCP scatter-gather) and busy-radio conditions fall through to the slow path, which clones the pbuf via `pbuf_alloc(PBUF_LINK, ...) + pbuf_copy()` to flatten and detach it before queuing.

- **TX queue is bounded.** The internal TX queue holds up to `TINYPAN_TX_QUEUE_LEN` (default 8) packets. If the queue is full, the packet is dropped and `ERR_MEM` is returned to lwIP.

- **BNEP filter requests are declined.** When the NAP sends BNEP filter set requests, TinyPAN responds with `0x0001` (Unsupported Request). This is spec-compliant and means the NAP must handle its own filtering.

- **Heartbeat/keepalive is not implemented.** The `TINYPAN_ENABLE_HEARTBEAT` config flag and associated fields in `tinypan_config_t` exist in the API but the supervisor does not act on them. Link health monitoring is a planned feature.

## License

MIT License.
