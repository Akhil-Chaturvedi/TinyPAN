# TinyPAN

TinyPAN is an ultra-lightweight, zero-allocation C library providing a Bluetooth Personal Area Network (PAN) client. It bridges Bluetooth L2CAP/BNEP data into the lwIP network stack, enabling background IP connectivity for embedded microcontrollers.

## Architecture and Footprint

The library is designed specifically for constrained embedded environments (e.g., ESP32, nRF52) and avoids dynamic memory allocation entirely. Packets are zero-copied from the hardware abstraction layer (HAL) directly into the lwIP stack.

Performance and size metrics (compiled via GCC for x86_64, size budgets enforced in CI):
* **RAM (bss + data):** 192 bytes
* **Flash (text):** ~14.5 KB
* **Heap Allocation:** 0 bytes (No malloc/free used)

The core architecture is strictly event-driven. The background idle state consumes near-zero CPU cycles, waking only on incoming L2CAP payload interrupts or internal lwIP timer events.

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

## License

MIT License.
