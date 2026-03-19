# TinyPAN

TinyPAN is a C implementation of the Bluetooth Personal Area Network (PAN) client protocol. It provides IP-over-Bluetooth connectivity for embedded systems by integrating the BNEP (Bluetooth Network Encapsulation Protocol) and SLIP (Serial Line IP) transports with the lwIP TCP/IP stack.

## Architecture and Operating Modes

TinyPAN supports two primary modes of operation, configured via `TINYPAN_USE_BLE_SLIP` in `tinypan_config.h`:

### Mode A: Bluetooth Classic (BNEP)
Targeted at dual-mode or Bluetooth Classic controllers (e.g., ESP32, Raspberry Pi Pico W).
- **Stack:** lwIP → BNEP → L2CAP (Classic).
- **Compatibility:** Connects directly to standard iOS/Android Personal Hotspot menus. No custom host-side software is required.

### Mode B: BLE SLIP Bridge
Targeted at BLE-only controllers (e.g., nRF52, ESP32-C3).
- **Stack:** lwIP → SLIP → BLE UART Service (e.g., NUS).
- **Compatibility:** Requires a companion application on the host device to bridge BLE traffic into the OS networking stack.

## Memory Design and Zero-Copy Path

TinyPAN is optimized for highly constrained environments and avoids dynamic heap allocation in its core logic.

### BNEP Zero-Copy Transmission
The BNEP transport (`tinypan_bnep_transport.c`) implements a true zero-copy path for outgoing frames. lwIP pre-allocates `PBUF_LINK_ENCAPSULATION_HLEN` (15) bytes of headroom in every outgoing packet. At transmission time, `pbuf_header()` expands backwards into that reserved space to write the BNEP header in-place, then restores the pointer after the send completes. No additional pbuf allocations, copies, or chains are created.

### Streaming SLIP Encoder
The SLIP transport uses an on-the-fly streaming encoder. Instead of buffering an entire escaped frame, it processes incoming `pbuf` chains in small chunks (128 bytes), minimizing peak RAM usage during transmission.

### Resource Metrics (Typical 32-bit MCU)
- **Library BSS/Data:** < 400 bytes (Core + Transport State).
- **Flash (Text):** ~12–18 KB (Mode dependent).
- **Heap Usage:** Static allocation only.
- **Buffer Requirements:** Hardware-specific HALs typically require a 1.6–2.0 KB alignment/MTU buffer depending on the radio controller DMA requirements.

## Hardware Abstraction Layer (HAL)

Integration with a specific Bluetooth stack requires implementing the `tinypan_hal.h` interface:

1. **`hal_bt_l2cap_send_iovec()`**: Transmit a scatter-gather array. This is the primary interface for zero-copy BNEP operation.
2. **`hal_bt_l2cap_send()`**: Transmit a contiguous buffer. Primarily used for SLIP streaming.
3. **`hal_bt_l2cap_connect()`**: Initiate an L2CAP channel to a remote BD_ADDR.
4. **`hal_get_tick_ms()`**: Provide a monotonic millisecond counter for protocol timeouts.
5. **RX Integration**: The HAL must invoke the registered `hal_l2cap_recv_callback_t` when data is received from the L2CAP channel.

### Threading and Reentrancy
TinyPAN is non-reentrant. All library interactions -- including API calls and HAL callbacks -- must be synchronized to the same thread context as `tinypan_process()`. The provided reference ports (ESP32, Zephyr) demonstrate safe event bridging using RTOS queues and static ring buffers.

`tinypan_get_next_timeout_ms()` returns the maximum safe sleep duration based on protocol timers. On RTOS platforms, the HAL should additionally use native signaling primitives (semaphores, event groups) to wake the processing thread immediately when a packet arrives from the radio, rather than relying solely on this timeout.

## Protocol and Compliance

- **BNEP**: Implements BNEP v1.0 (General Ethernet Format).
- **Encapsulation**: Uses uncompressed BNEP headers (Header Type 0x00) for maximum compatibility with mobile OS networking stacks.
- **Multicast**: Supports dynamic multicast filtering for efficient power management.
- **lwIP Integration**: Automatically manages `netif` state, link status, and DHCP negotiation.

## License

MIT License.
