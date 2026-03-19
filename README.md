# TinyPAN

TinyPAN is a C library implementing a Bluetooth PAN (Personal Area Network) client for embedded systems. It supports native Bluetooth Classic tethering via BNEP and BLE-based tethering via SLIP over a UART-style characteristic. Both modes integrate with the lwIP TCP/IP stack to provide standard IP connectivity and DHCP support.

## Architecture: Dual-Mode Connectivity

TinyPAN operates in two modes, selectable via `TINYPAN_USE_BLE_SLIP` in `tinypan_config.h`:

### Mode A: Native Bluetooth Classic (BNEP)
Designed for microcontrollers with Bluetooth Classic or Dual-Mode radios (e.g., ESP32, Raspberry Pi Pico W).
*   **Protocol:** lwIP -> BNEP encapsulation -> BT Classic L2CAP.
*   **Host Compatibility:** Compatible with standard iOS and Android Personal Hotspot. No companion app required.

### Mode B: BLE Companion App (SLIP)
Designed for BLE-only microcontrollers (e.g., nRF52, ESP32-C3/S3, STM32WB).
*   **Protocol:** lwIP -> SLIP framing -> BLE UART characteristic (e.g., Nordic UART Service).
*   **Host Compatibility:** Requires a companion app to bridge BLE SLIP frames into the host OS networking stack (via `VpnService` on Android or `NetworkExtension` on iOS).

### Transport Abstraction

The core modules (`tinypan_supervisor.c`, `tinypan_lwip_netif.c`) are transport-agnostic. Each backend implements the `tinypan_transport_t` interface. Compile-time selection determines the active backend, which is accessed via `tinypan_transport_get()`.

## Memory Management

The library is designed for constrained environments and strictly avoids dynamic heap allocation in its core logic.

### Zero-Copy BNEP Path
The BNEP transport (`tinypan_bnep_transport.c`) utilizes a zero-copy transmission path via lwIP `pbuf_chain`. For each outgoing IP frame, a small pbuf for the BNEP header is allocated and chained to the original IP payload. This avoids redundant 1500-byte protocol-layer copies while preserving the original pbuf for potential TCP retransmissions.

**Engineering Note**: Hardware-specific HALs (e.g., ESP32) may perform a single internal copy into an aligned bounce buffer if the underlying DMA controller requires 4-byte alignment or contiguous memory.

### Queueing
If the radio is busy (L2CAP queue full), the `pbuf` chain is stored in a 3-slot ring buffer. Queued frames are drained automatically when the radio signals readiness via `HAL_L2CAP_EVENT_CAN_SEND_NOW` or during the `tinypan_process()` polling cycle.

In SLIP mode, an $O(N)$ encoder serializes full SLIP frames into a secondary buffer before transmission to maximize BLE throughput. The RX path uses a streaming FSM to build lwIP pbufs incrementally.

## Performance and Scheduling

TinyPAN uses a single-threaded polling model driven by `tinypan_process()`. For power-constrained applications, `tinypan_get_next_timeout_ms()` provides the millisecond duration until the next scheduled internal event or lwIP timer, allowing the MCU to enter low-power sleep states.

### Resource Metrics (Typical GCC, x86_64)
* **RAM (bss + data):** ~256 bytes
* **Flash (text):** ~15 KB
* **Heap usage:** None (core)

## Repository Layout

```text
TinyPAN/
├── CMakeLists.txt           # Build configuration
├── include/
│   ├── tinypan.h            # Public API
│   ├── tinypan_config.h     # Compile-time configuration
│   ├── tinypan_hal.h        # Hardware Abstraction Layer interface
│   ├── lwipopts.h           # lwIP configuration
│   └── arch/                # lwIP architecture port
├── src/
│   ├── tinypan.c            # Initialization and event routing
│   ├── tinypan_bnep.c       # BNEP protocol implementation
│   ├── tinypan_supervisor.c # Connection state machine
│   ├── tinypan_lwip_netif.c # lwIP netif driver
│   ├── tinypan_transport.h  # Transport interface
│   ├── tinypan_bnep_transport.c   # BNEP backend
│   ├── tinypan_slip_transport.c   # SLIP backend
│   └── tinypan_internal.h   # Internal prototypes
├── ports/
│   ├── esp32_classic/       # Reference HAL for ESP-IDF
│   └── zephyr_ble/          # Reference HAL for Zephyr RTOS
├── tests/
│   ├── test_bnep.c          # Unit tests
│   ├── test_supervisor.c    # State machine tests
│   ├── test_integration.c   # DHCP flow over mock HAL
│   └── dhcp_sim.c/.h        # DHCP simulation
└── hal/
    └── mock/                # Mock HAL for simulation
```

## Porting to Hardware

Implement the functions declared in `tinypan_hal.h`:

1.  **`hal_get_tick_ms()`**: Return a monotonic millisecond counter.
2.  **`hal_bt_l2cap_send()`**: Transmit a buffer. In SLIP mode, this handles raw SLIP-escaped bytes.
3.  **`hal_bt_l2cap_connect()`**: Initiate L2CAP connection.
4.  **`hal_bt_l2cap_can_send()`**: Return internal controller readiness.
5.  **`hal_bt_l2cap_request_can_send_now()`**: Request a callback when the controller is ready.
6.  **RX Path**: Invoke the registered receive callback when data arrives from the radio.

### RTOS and Thread Safety

TinyPAN is non-reentrant. All API calls and HAL callbacks must execute in the same thread context as `tinypan_process()`.

Reference HALs (ESP32, Zephyr) demonstrate how to safely bridge events from Bluetooth stack tasks (or ISRs) into the application thread using OS primitives (Queues/Ring Buffers). This is critical to prevent corruption of lwIP internal state.

## Design Constraints and Constants

* **BNEP Protocol**: Implements BNEP v1.0, DHCP, and ARP.
* **Multicast**: Dynamic multicast filtering is supported. IPv6/mDNS filtering depends on `LWIP_IPV6`.
* **Linker Compatibility**: `sys_now()` in `tinypan_lwip_netif.c` is guarded by `#if NO_SYS` to avoid conflicts on platforms (ESP-IDF, Zephyr) that provide their own timing providers.
* **Header Compression**: For stability and compatibility with diverse networking stacks, TinyPAN uses General Ethernet headers (15 bytes) for all BNEP traffic. Compressed headers are not used in the TX path.
* **Alignment**: TinyPAN sets `ETH_PAD_SIZE = 1` for optimal stack alignment. HAL implementations must handle potential unalignment in the 15-byte BNEP header.

## License

MIT License.
