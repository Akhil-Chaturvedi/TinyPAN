# TinyPAN

TinyPAN is a C implementation of the Bluetooth Personal Area Network (PAN) client protocol. It provides IP-over-Bluetooth connectivity for embedded systems by integrating the BNEP (Bluetooth Network Encapsulation Protocol) and SLIP (Serial Line IP) transports with the lwIP TCP/IP stack.

## Architecture and Operating Modes

TinyPAN supports two primary modes of operation, configured via `TINYPAN_USE_BLE_SLIP` in `tinypan_config.h`:

### Mode A: Bluetooth Classic (BNEP)
Targeted at dual-mode Bluetooth controllers (e.g., original ESP32).

- **Stack:** lwIP -> BNEP -> L2CAP (Classic)
- **Compatibility:** Standard iOS/Android Personal Hotspot. No custom host-side software required.
- **Support:** ESP32-C3, ESP32-S3, and other BLE-only SoCs are not supported in this mode.

### Mode B: BLE SLIP Bridge
Targeted at BLE-only or dual-mode controllers (e.g., nRF52, ESP32-C3, ESP32-S3).

- **Stack:** lwIP -> SLIP -> BLE UART Service (NUS)
- **Compatibility:** Requires a companion application on the host to bridge BLE traffic into the OS networking stack. This is not a standard hotspot connection.

## Memory Design

### BNEP Transmission
The BNEP transport (`tinypan_bnep_transport.c`) implements zero-copy for IP payloads via scatter-gather mapping. Outgoing `pbuf` chains are mapped to a `tinypan_iovec_t` array where `iov[0]` is a locally synthesized BNEP header (Type 0x00 or Type 0x02). Subsequent `iovec` entries reference the original `pbuf->payload` segments directly, skipping the Ethernet header, without mutating shared lwIP memory.

The `pbuf` and its associated `tinypan_iovec_t` descriptor array are held in the static transport job queue until the HAL fires `HAL_L2CAP_EVENT_TX_COMPLETE`. This ensures that even on true DMA implementations where the hardware reads descriptors asynchronously, the memory remains valid and immutable until the radio signals completion. Only one frame is in-flight at a time. 

**Dead-Link Protection (Hardening):** In the event of an abrupt L2CAP disconnect where the peer fails to acknowledge in-flight packets, the BNEP transport forcibly reclaims and frees all queued pbufs during the cleanup phase. This prevents pool exhaustion and ensures immediate memory recovery for subsequent connection attempts.

### SLIP Encoder
The SLIP transport encodes outgoing `pbuf` chains into a configurable staging buffer (`TINYPAN_SLIP_CHUNK_SIZE`) using a multi-pass scanner based on `memchr()`. This approach leverages hardware-optimized SIMD instructions for frame boundary detection, significantly outperforming manual byte-by-byte loops on bulk IP data. At runtime, the transport queries `hal_bt_l2cap_get_mtu()` and enforces a minimum safety boundary to prevent integer underflows on legacy or fluctuating links. The original pbuf is held by reference (`pbuf_ref`) and released only after the final chunk is acknowledged by the HAL.

### SLIP Decoder
Incoming SLIP bytes are accumulated directly into pool-allocated (`PBUF_POOL`) segments via a streaming FSM. A single incoming frame is bounded to 2 pool segments (~3 KB). If a frame exceeds this limit, the FSM enters a `seeking_end` state and pauses pool allocations until a `SLIP_END` delimiter is reached, preventing memory-pool thrashing during error recovery. When a valid frame is completed, the last segment is trimmed with `pbuf_realloc` to the exact data length.

### Resource Metrics (Typical 32-bit MCU)
- **Library BSS/Data:** < 400 bytes (core logic and transport state)
- **Flash (Text)::** ~12-18 KB, mode dependent
- **lwIP Heap/Pool:** Configurable; defaults to 4 byte-aligned segments (6.8KB pool) plusStatic RAM cost: Approximately 1.8 KB of BSS for the TX aligned bounce buffer. The RX path is dynamic; the HAL allocates lwIP `pbuf` pool segments directly from the Bluetooth callback task and queues the resulting pointers for the application thread. This eliminates multi-kilobyte static RX buffers and leverages lwIP's native memory management for improved heap efficiency.
The SLIP transport chunks to BLE MTU; the HAL does not maintain a separate TX buffer.

## Hardware Abstraction Layer (HAL)

Integration with a specific Bluetooth stack requires implementing the `tinypan_hal.h` interface:

1. **`hal_bt_l2cap_send_iovec()`**: Transmit a scatter-gather array. The primary TX interface for BNEP mode.
2. **`hal_bt_l2cap_send()`**: Transmit a contiguous buffer. Used for SLIP streaming and BNEP control packets.
3. **`hal_bt_l2cap_connect()`**: Initiate an L2CAP channel to a remote BD_ADDR.
4. **`hal_get_tick_ms()`**: Provide a monotonic millisecond counter. Wrap-around is handled correctly.
5. **TX Lifecycle**: After a successful `send_iovec` call returns `0` (used by BNEP zero-copy DMA), the HAL must fire `HAL_L2CAP_EVENT_TX_COMPLETE` (via the event callback) once the radio is done with the submitted buffer. **Stack Safety:** To prevent deep recursion panics, the HAL must NOT fire this synchronously within the send context; instead, it must defer the callback to the next `hal_bt_poll()` cycle. **Note:** Contiguous `send` calls (used in SLIP mode) rely purely on synchronous return backpressure and must NOT fire this event to avoid event queue DoS.
6. **RX Integration**: The HAL must invoke the registered `hal_l2cap_recv_callback_t` from the polling context or bridge incoming data through a thread-safe queue/ring buffer.

### ESP32-C3 / ESP32-S3 (BLE-only/NimBLE)
> [!IMPORTANT]
> The provided `ports/esp32_classic/tinypan_hal_esp32.c` reference HAL targets the **Bluetooth Classic (BR/EDR)** L2CAP stack. 
> To use TinyPAN in **Mode B (SLIP Bridge)** on BLE-only chips like the ESP32-C3, you must implement a wrapper for your chosen BLE stack (e.g., NimBLE or Bluedroid BLE GATT Server) that satisfies the `tinypan_hal.h` interface. 

### Threading and Reentrancy
TinyPAN is non-reentrant. All library interactions -- including API calls and HAL callbacks -- must be synchronized to the same thread context as `tinypan_process()`. The provided reference ports (ESP32, Zephyr) bridge interrupt/callback-context events to the application thread using static ring buffers.

`tinypan_get_next_timeout_ms()` returns the maximum safe sleep duration based on active protocol timers and HAL-internal polling requirements (e.g. congestion backoffs). 

**Wakeup Optimization:** To minimize latency caused by protocol timer resolutions, the application should register a wakeup hook via `tinypan_set_wakeup_callback()`. The HAL must invoke this callback from Bluetooth ISRs or event tasks to immediately abort the application's sleep state when new data or event transitions occur.

## Protocol Implementation Notes

- **BNEP Version:** v1.0, supporting General and Compressed Ethernet formats.
- **Header Compression:** Dynamically enabled for PANU-to-NAP flows to minimize radio-on time and latency.
- **BNEP Control Packets:** Extension headers are parsed and skipped before control type dispatch.
- **Multicast Filtering:** Automatically sent after BNEP setup before DHCP.
- **DHCP Lifecycle:** Managed by lwIP's DHCP client. TinyPAN implements a soft retry mechanism with exponential backoff (default: 3 attempts) for DHCP discovery timeouts before declaring a link failure. The netif stack is reset on disconnect; TinyPAN monitors the IP address state in the netif callback to handle lease expiration or renewal failures, ensuring the library state machine accurately reflects the network reachability.
- **State Transition Safety:** Prevents invalid transitions and guarantees state machine consistency.
- **ESP32 Concurrency:** The reference HAL maintains thread safety on dual-core processors via hardware spinlocks (`portENTER_CRITICAL`) and atomic FreeRTOS pointer queues, ensuring consistent cross-task memory visibility between the Bluetooth stack and application threads.
- **Portability:** Networking header extraction uses safe byte-indexing and respects `ETH_PAD_SIZE` to support unaligned access and Hard Fault prevention on ARM Cortex-M0/M3 targets.

## License

MIT License.
