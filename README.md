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
The BNEP transport (`tinypan_bnep_transport.c`) implements zero-copy for IP payloads via scatter-gather mapping. Outgoing `pbuf` chains are mapped to a `tinypan_iovec_t` array where `iov[0]` is a locally synthesized BNEP header (Type 0x00 or Type 0x02). Subsequent `iovec` entries reference the original `pbuf->payload` segments directly, skipping the Ethernet header, without mutating shared lwIP memory.

The `pbuf` is held in-queue until the HAL fires `HAL_L2CAP_EVENT_TX_COMPLETE`, at which point `pbuf_free` is called. Only one frame is in-flight at a time. HAL implementations that copy data internally (e.g., Bluedroid's `esp_bt_l2cap_data_write`) may fire `TX_COMPLETE` immediately; HAL implementations that perform true DMA must fire it only after the hardware signals completion.

### SLIP Encoder
The SLIP transport encodes outgoing `pbuf` chains on the fly into a configurable staging buffer (`TINYPAN_SLIP_CHUNK_SIZE`). At runtime, the transport queries `hal_bt_l2cap_get_mtu()` to dynamically align chunks with the link-layer MTU (e.g., 185 bytes for iOS, 247 bytes for Android), preventing fragmentation at the BLE stack level. Contiguous runs of non-escape bytes are copied in bulk; only bytes requiring escaping (`0xC0`, `0xDB`) are handled individually. The original pbuf is held by reference (`pbuf_ref`) and released only after the final chunk is acknowledged by the HAL.

### SLIP Decoder
Incoming SLIP bytes are accumulated directly into pool-allocated (`PBUF_POOL`) segments via a streaming FSM. A single incoming frame is bounded to 2 pool segments (~3 KB). If a frame exceeds this limit, the FSM enters a `seeking_end` state and pauses pool allocations until a `SLIP_END` delimiter is reached, preventing memory-pool thrashing during error recovery. When a valid frame is completed, the last segment is trimmed with `pbuf_realloc` to the exact data length.

### Resource Metrics (Typical 32-bit MCU)
- **Library BSS/Data:** < 400 bytes (core logic and transport state)
- **Flash (Text):** ~12-18 KB, mode dependent
- **lwIP Heap/Pool:** Configurable; defaults to 4 byte-aligned segments (6.8KB pool) plus 4KB heap.
- **ESP32 HAL RAM:** ~8.6 KB static RAM (6.8KB RX ring buffer + 1.7KB TX bounce buffer). Configurable via `TINYPAN_ESP_RX_RING_SLOTS`.
- **Zephyr HAL RAM:** ~1.9 KB static RAM (RX ring buffer only). The SLIP transport chunks to BLE MTU; the HAL does not maintain a separate TX buffer.

## Hardware Abstraction Layer (HAL)

Integration with a specific Bluetooth stack requires implementing the `tinypan_hal.h` interface:

1. **`hal_bt_l2cap_send_iovec()`**: Transmit a scatter-gather array. The primary TX interface for BNEP mode.
2. **`hal_bt_l2cap_send()`**: Transmit a contiguous buffer. Used for SLIP streaming and BNEP control packets.
3. **`hal_bt_l2cap_connect()`**: Initiate an L2CAP channel to a remote BD_ADDR.
4. **`hal_get_tick_ms()`**: Provide a monotonic millisecond counter. Wrap-around is handled correctly.
5. **TX Lifecycle**: After a successful `send_iovec` call returns `0` (used by BNEP zero-copy DMA), the HAL must fire `HAL_L2CAP_EVENT_TX_COMPLETE` (via the event callback) once the radio is done with the submitted buffer. **Stack Safety:** To prevent deep recursion panics, the HAL must NOT fire this synchronously within the send context; instead, it must defer the callback to the next `hal_bt_poll()` cycle. **Note:** Contiguous `send` calls (used in SLIP mode) rely purely on synchronous return backpressure and must NOT fire this event to avoid event queue DoS.
6. **RX Integration**: The HAL must invoke the registered `hal_l2cap_recv_callback_t` from the polling context or bridge incoming data through a thread-safe queue/ring buffer.

### Threading and Reentrancy
TinyPAN is non-reentrant. All library interactions -- including API calls and HAL callbacks -- must be synchronized to the same thread context as `tinypan_process()`. The provided reference ports (ESP32, Zephyr) bridge interrupt/callback-context events to the application thread using static ring buffers.

`tinypan_get_next_timeout_ms()` returns the maximum safe sleep duration based on active protocol timers and HAL-internal polling requirements (e.g. congestion backoffs). 

**Latency Elimination:** To avoid the "500ms ping" floor caused by lwIP/BNEP timer resolutions, the application should register a wakeup hook via `tinypan_set_wakeup_callback()`. The HAL must invoke this callback from Bluetooth ISRs or event tasks to immediately abort the application's sleep state when new data arrives.

## Protocol Implementation Notes

- **BNEP Version:** v1.0, supporting General and Compressed Ethernet formats.
- **Header Compression:** Dynamically enabled for PANU-to-NAP flows to minimize radio-on time and latency.
- **BNEP Control Packets:** Extension headers are parsed and skipped before control type dispatch.
- **Multicast Filtering:** Automatically sent after BNEP setup before DHCP.
- **DHCP Lifecycle:** Managed by lwIP's DHCP client. The netif stack is reset on disconnect; TinyPAN monitors the IP address state in the netif callback to handle lease expiration or renewal failures, ensuring the library state machine accurately reflects the network reachability.

## License

MIT License.
