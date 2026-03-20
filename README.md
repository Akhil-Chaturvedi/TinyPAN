# TinyPAN

TinyPAN is a C implementation of the Bluetooth Personal Area Network (PAN) client protocol. It provides IP-over-Bluetooth connectivity for embedded systems by integrating the BNEP (Bluetooth Network Encapsulation Protocol) and SLIP (Serial Line IP) transports with the lwIP TCP/IP stack.

## Architecture and Operating Modes

TinyPAN supports two primary modes of operation, configured via `TINYPAN_USE_BLE_SLIP` in `tinypan_config.h`:

### Mode A: Bluetooth Classic (BNEP)
Targeted at dual-mode or Bluetooth Classic controllers (e.g., ESP32).

- **Stack:** lwIP -> BNEP -> L2CAP (Classic)
- **Compatibility:** Connects to the standard iOS/Android Personal Hotspot. No custom host-side software is required.

### Mode B: BLE SLIP Bridge
Targeted at BLE-only controllers (e.g., nRF52, ESP32-C3).

- **Stack:** lwIP -> SLIP -> BLE UART Service (NUS)
- **Compatibility:** Requires a companion application on the host to bridge BLE traffic into the OS networking stack. This is not a standard hotspot connection.

## Memory Design

### BNEP Transmission
The BNEP transport (`tinypan_bnep_transport.c`) implements true zero-copy via scatter-gather DMA. Outgoing `pbuf` chains are mapped to a `tinypan_iovec_t` array where the first element is a locally synthesized BNEP header (supporting both Type 0x00 General and Type 0x02 Compressed formats). The subsequent `iovec` entries point directly to the original `pbuf->payload` segments, bypassing the Ethernet header. This architecture prevents mutation of shared lwIP memory, making the library safe for TCP-enabled builds.

### SLIP Encoder
The SLIP transport encodes outgoing `pbuf` chains on the fly into a 255-byte staging buffer—a size optimized for modern BLE 4.2+ Data Length Extension (DLE) MTUs. Contiguous runs of non-escape bytes are copied in bulk; only bytes requiring escaping (`0xC0`, `0xDB`) are handled individually. The original pbuf is held by reference (`pbuf_ref`) and released only after the final chunk is acknowledged by the HAL.

### SLIP Decoder
Incoming SLIP bytes are accumulated directly into pool-allocated (`PBUF_POOL`) segments via a streaming FSM. A single incoming frame is bounded to 2 pool segments (~3 KB). If a frame exceeds this limit, the FSM enters a `seeking_end` state and pauses pool allocations until a `SLIP_END` delimiter is reached, preventing memory-pool thrashing during error recovery. When a valid frame is completed, the last segment is trimmed with `pbuf_realloc` to the exact data length.

### Resource Metrics (Typical 32-bit MCU)
- **Library BSS/Data:** < 400 bytes (core and transport state, excluding lwIP pool)
- **Flash (Text):** ~12-18 KB, mode dependent
- **lwIP Heap/Pool:** Configurable; defaults to 4 byte-aligned segments (6.8KB pool) plus 4KB heap.
- **ESP32 HAL BSS:** ~7.0 KB static RAM (4 x 1.7KB RX slots plus TX bounce buffer). Configurable via `TINYPAN_ESP_RX_RING_SLOTS`.
- **Zephyr HAL BSS:** ~2.5 KB static RAM (1.9KB RX ring buffer plus events).

## Hardware Abstraction Layer (HAL)

Integration with a specific Bluetooth stack requires implementing the `tinypan_hal.h` interface:

1. **`hal_bt_l2cap_send_iovec()`**: Transmit a scatter-gather array. The primary TX interface.
2. **`hal_bt_l2cap_send()`**: Transmit a contiguous buffer. Primarily used for SLIP streaming.
3. **`hal_bt_l2cap_connect()`**: Initiate an L2CAP channel to a remote BD_ADDR.
4. **`hal_get_tick_ms()`**: Provide a monotonic millisecond counter. Wrap-around is handled correctly.
5. **RX Integration**: The HAL must invoke the registered `hal_l2cap_recv_callback_t` from the polling context or bridge incoming data through a thread-safe queue/ring buffer.

### Threading and Reentrancy
TinyPAN is non-reentrant. All library interactions -- including API calls and HAL callbacks -- must be synchronized to the same thread context as `tinypan_process()`. The provided reference ports (ESP32, Zephyr) bridge interrupt/callback-context events to the application thread using static ring buffers.

`tinypan_get_next_timeout_ms()` returns the maximum safe sleep duration based on active protocol timers. On RTOS platforms, the HAL should use native signaling primitives (semaphores, event groups) to wake the processing thread when new data arrives from the radio.

## Protocol Implementation Notes

- **BNEP Version:** v1.0, supporting General and Compressed Ethernet formats.
- **Header Compression:** Dynamically enabled for PANU-to-NAP flows to minimize radio-on time and latency.
- **BNEP Control Packets:** Extension headers are parsed and skipped before control type dispatch.
- **Multicast Filtering:** Automatically sent after BNEP setup before DHCP.
- **DHCP:** Managed by lwIP's DHCP client. The netif stack is reset on disconnect; `dhcp_stop` is called before `dhcp_start` on reconnection to force a fresh lease discovery.

## License

MIT License.
