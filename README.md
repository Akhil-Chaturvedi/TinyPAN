# TinyPAN

TinyPAN is an ultra-lightweight, zero-allocation C library providing a Bluetooth Personal Area Network (PAN) client. It bridges Bluetooth L2CAP/BNEP data into the lwIP network stack, enabling background IP connectivity for embedded microcontrollers.

## Architecture and Footprint

The library is designed specifically for constrained embedded environments (e.g., ESP32, nRF52) and avoids dynamic memory allocation entirely. Packets are zero-copied from the hardware abstraction layer (HAL) directly into the lwIP stack.

Performance and size metrics (compiled via GCC for x86_64, size budgets enforced in CI):
* **RAM (bss + data):** 192 bytes
* **Flash (text):** ~14.5 KB
* **Heap Allocation:** 0 bytes (No malloc/free used)

The core architecture is an RTOS-friendly polling pump. The background idle state consumes near-zero CPU cycles, driven by a periodic `tinypan_process()` tick to handle lwIP timeouts, and waking instantly on incoming L2CAP payload interrupts.

## Repository Layout

```text
TinyPAN/
├── CMakeLists.txt         # Primary build configuration (Fetches lwIP automatically)
├── include/
│   ├── tinypan.h          # Main library API
│   ├── tinypan_config.h   # Timing and retry configurations
│   ├── tinypan_hal.h      # Hardware Abstraction Layer interface
│   ├── lwipopts.h         # Project-specific lwIP configuration
│   └── arch/              # lwIP architecture shims
├── src/
│   ├── tinypan.c          # Core initialization and event routing
│   ├── tinypan_bnep.c     # BNEP protocol encapsulation/decapsulation
│   ├── tinypan_supervisor.c # Connection state machine (IDLE -> CONNECTING -> DHCP -> ONLINE)
│   └── tinypan_lwip_netif.c # lwIP Network Interface (netif) bridge driver
├── tests/
│   ├── test_bnep.c        # BNEP parser/builder unit tests
│   ├── test_supervisor.c  # State machine and timeout simulation tests
│   ├── test_integration.c # Full lwIP DHCP DORA integration test over Mock HAL
│   └── dhcp_sim.c         # DHCP packet simulation helpers
└── hal/
    └── mock/              # Pure C simulation HAL used for validation
```

## Integration and Usage

To use TinyPAN on target hardware, the host application must provide implementations for the three functions defined in `tinypan_hal.h`:

1. `hal_get_tick_ms()`: Provide a monotonically increasing millisecond tick (e.g., FreeRTOS `xTaskGetTickCount() * portTICK_PERIOD_MS`).
2. `hal_bt_l2cap_send(data, len)`: Route the byte array to the native Bluetooth stack for transmission over the L2CAP PSM assigned to BNEP.
3. `tinypan_input(data, len)`: Call this from your Bluetooth stack's RX interrupt when an L2CAP packet is received.

## Build and Validation

The project is built and validated purely in simulation using a Mock HAL. This mathematically verifies the BNEP framing, state machine transitions, and lwIP DHCP bridge without requiring physical Bluetooth hardware.

### Prerequisites
* CMake (>= 3.12)
* GCC / MinGW / Clang (C99 compliant)
* Ninja or Make

### Building

The CMake configuration automatically uses `FetchContent` to download the lwIP network stack (STABLE-2_1_3_RELEASE) and binds it to the TinyPAN library.

```bash
cmake -S . -B build
cmake --build build
```

### Running the Test Suite

The CTest suite validates the BNEP headers, the supervisor state machine reconnect logic, the complete IP acquisition flow via lwIP, and guarantees the static size constraints remain unbroken.

```bash
ctest --test-dir build -V
```

If successful, the `IntegrationFlowTests` will demonstrate lwIP successfully booting up, issuing an automatically generated DHCP DISCOVER packet, handling a simulated mock DHCP OFFER, and verifying the leased IP address via ARP ping.

## Design Constraints

TinyPAN makes deliberate trade-offs to stay small. Users should be aware of these before integrating:

* **Single-threaded only.** All calls to `tinypan_process()`, `tinypan_start()`, `tinypan_stop()`, and the HAL receive callback must originate from the same execution context (main loop or a single RTOS task). There is no internal locking. Calling TinyPAN functions from multiple threads or ISRs will corrupt internal state.

* **TX path performs two copies.** Outgoing IP packets are copied once when lwIP flattens chained pbufs, and again when the BNEP layer prepends its header. This costs ~3 KB of static BSS and extra CPU cycles per packet. A future optimization could use lwIP's `PBUF_LINK_ENCAPSULATION_HLEN` to reserve BNEP header space inside the pbuf itself, eliminating one copy entirely.

* **No transmit queue.** If the Bluetooth L2CAP channel is busy when lwIP tries to send a packet, the packet is dropped with `ERR_WOULDBLOCK`. lwIP's UDP and RAW APIs do not automatically retry. For bursty traffic patterns, the integrator should implement a small ring buffer in the HAL layer or throttle transmission rate at the application level.

* **BNEP filter requests are declined.** When the NAP (phone) sends BNEP Network Protocol Type or Multicast Address filter requests, TinyPAN responds with "Unsupported Request" (`0x0001`). This is spec-compliant and forces the NAP to handle filtering on its own end. Actual filter processing is not implemented.

## License

MIT License.
