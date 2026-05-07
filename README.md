# TinyPAN

TinyPAN is a C implementation of the Bluetooth Personal Area Network (PAN) client protocol. It provides IP-over-Bluetooth connectivity for embedded systems by integrating the BNEP (Bluetooth Network Encapsulation Protocol) and SLIP (Serial Line IP) transports with the lwIP TCP/IP stack.

## Architecture and Operating Modes

TinyPAN supports two primary modes of operation, configured via `TINYPAN_USE_BLE_SLIP` in `tinypan_config.h`:

### Mode A: Bluetooth Classic (BNEP)
Targeted at dual-mode Bluetooth controllers (e.g., original ESP32).

*   **Stack:** lwIP -> BNEP -> L2CAP (Classic)
*   **BNEP Transport:** Implements a state-safe BNEP client with control-packet handling and multicast filtering.
*   **Architectural Zero-Copy Ready:** Designed for zero-copy TX via scatter-gather mapping (requires HAL DMA support; reference HALs may use fallback copying).
*   **Compatibility:** Standard iOS/Android Personal Hotspot. No custom host-side software required.

### Mode B: BLE SLIP Bridge
Targeted at BLE-only or dual-mode controllers (e.g., nRF52, ESP32-C3, ESP32-S3).

*   **Stack:** lwIP -> SLIP -> BLE UART Service (NUS)
*   **SLIP Streaming:** The SLIP transport encodes outgoing `pbuf` chains into a configurable staging buffer using a single-pass C loop.
*   **Compatibility:** Requires a companion application on the host to bridge BLE traffic into the OS networking stack. This is not a standard hotspot connection.

## Memory Design

### BNEP Transmission
The BNEP transport (`tinypan_bnep_transport.c`) implements zero-copy for IP payloads via scatter-gather mapping. Outgoing `pbuf` chains are mapped to a `tinypan_iovec_t` array where `iov[0]` is a locally synthesized BNEP header (Type 0x00 or Type 0x02). Subsequent `iovec` entries reference the original `pbuf->payload` segments directly, skipping the Ethernet header, without mutating shared lwIP memory.

The `pbuf` and its associated `tinypan_iovec_t` descriptor array are held in the static transport job queue until the HAL fires `HAL_L2CAP_EVENT_TX_COMPLETE`. This ensures that even on true DMA implementations where the hardware reads descriptors asynchronously, the memory remains valid and immutable until the radio signals completion. Only one frame is in-flight at a time. 

**Link Protection (Hardening):** TinyPAN implements link-loss protection strategies. If an asynchronous transmission times out at the BNEP layer (e.g., hardware stall), the library forcibly tears down the L2CAP link to request hardware state cancellation before reclaiming pbufs. This helps mitigate potential DMA use-after-free conditions in multi-threaded RTOS stacks.

### SLIP Encoder
The SLIP transport encodes outgoing `pbuf` chains into a configurable staging buffer (`TINYPAN_SLIP_CHUNK_SIZE`) using a structural single-pass C loop. At runtime, the transport queries `hal_bt_l2cap_get_mtu()` and enforces a minimum safety boundary to prevent integer underflows. The original pbuf is held by reference (`pbuf_ref`) and released only after the final chunk is transmitted.

### SLIP Decoder
Incoming SLIP bytes are accumulated directly into a static 1.6 KB accumulator buffer (`s_slip_rx_buf`). Once a `SLIP_END` delimiter is detected, exactly one `pbuf` is allocated from the pool and the frame is passed to lwIP. This deterministic approach eliminates pool fragmentation risks and prevents memory exhaustion during serial stream error recovery.

### Resource Metrics (Typical 32-bit MCU)
- **Library BSS/Data:** < 400 bytes (core logic and transport state)
- **Flash (Text):** ~12-18 KB (mode dependent)
- **Memory Model:** The reference HALs (ESP32, Zephyr) use a thread-safe RX path via `xMessageBuffer` or `ring_buf`. This design allows the Bluetooth task to copy incoming data without touching the lwIP heap, avoiding the spinlock-induced heap corruption risks inherent in standard multicore stack integrations.

## Hardware Abstraction Layer (HAL)

Integration with a specific Bluetooth stack requires implementing the `tinypan_hal.h` interface:

1.  **`hal_bt_l2cap_send_iovec()`**: Transmit a scatter-gather array. The primary TX interface for BNEP mode.
2.  **`hal_bt_l2cap_send()`**: Transmit a contiguous buffer. Used for SLIP streaming and BNEP control packets.
3.  **`hal_bt_l2cap_connect()`**: Initiate an L2CAP channel to a remote BD_ADDR.
4.  **`hal_get_tick_ms()`**: Provide a monotonic millisecond counter. Wrap-around is handled correctly.
5.  **`hal_mutex_x`**: (Mandatory) Mutex primitives for thread-safe cross-task communication in RTOS environments.
6.  **TX Lifecycle**: After a successful `send_iovec` call returns `0` (used by BNEP zero-copy DMA), the HAL must fire `HAL_L2CAP_EVENT_TX_COMPLETE` (via the event callback) once the radio is done with the submitted buffer. **Stack Safety:** To prevent deep recursion panics, the HAL must NOT fire this synchronously within the send context; instead, it must defer the callback to the next `hal_bt_poll()` cycle. **Note:** Contiguous `send` calls (used in SLIP mode) rely purely on synchronous return backpressure and must NOT fire this event to avoid event queue DoS.
7.  **RX Integration**: The HAL must invoke the registered `hal_l2cap_recv_callback_t` from the polling context or bridge incoming data through a thread-safe queue/ring buffer.

### ESP32-C3 / ESP32-S3 (BLE-only/NimBLE)
> The provided `ports/esp32_classic/tinypan_hal_esp32.c` reference HAL targets the **Bluetooth Classic (BR/EDR)** L2CAP stack.
> 
> **ESP-IDF Integration:** Ensure `TINYPAN_ENABLE_LWIP=1` is set in your build configuration. The library will detect the ESP-IDF environment and link against the system lwIP headers. Do NOT set `TINYPAN_FETCH_LWIP_TEST_HARNESS` in production builds.
>
> **Prerequisite: GAP Security.** TinyPAN handles L2CAP and BNEP only. Your application must configure GAP security (SSP mode, IO capabilities, bonding) and register a GAP callback before calling `tinypan_init()`. Without this, Android/iOS will reject the L2CAP connection with an authentication failure (HCI reason 0x05). Ensure `nvs_flash_init()` is called before `esp_bluedroid_init()` so bonding keys persist across reboots. See `examples/esp32_app_main.c` for a reference integration.

### Threading and Reentrancy
TinyPAN is non-reentrant. All library interactions -- including API calls and HAL callbacks -- must be synchronized to the same thread context as `tinypan_process()`. The provided reference ports (ESP32, Zephyr) bridge interrupt/callback-context events to the application thread using thread-safe RTOS primitives (Mutexes and MessageBuffers).

## Optimizations

The following optimizations were implemented to ensure TinyPAN functions within the resource constraints of low-power microcontrollers, specifically focusing on memory layout and cycle efficiency.

### 1. Zero-Copy Architecture via Scatter-Gather `iovec`
*   **Context:** Conventional network implementations often require a contiguous buffer to prepend protocol headers to a payload, necessitating a memory copy of the entire packet.
*   **Implementation:** TinyPAN utilizes a scatter-gather approach within the BNEP transport layer (`src/tinypan_bnep_transport.c`). By passing lwIP `pbuf` references directly and synthesizing the 15-byte BNEP header in a small static buffer, the system avoids payload duplication.
*   **Impact:** This eliminates the need for an MTU-sized (up to 1500 bytes) intermediate buffer during transmission. The core library operates with a static memory footprint of **< 400 bytes (BSS/Data)** and avoids heap-based allocations during the TX path, preventing fragmentation-related failures in long-running deployments.

### 2. Header Compression and Protocol Efficiency
*   **BNEP Optimization:** The implementation includes dynamic header compression via `bnep_get_ethernet_header_len`. When the system detects standard PANU-to-NAP traffic flows, it strips redundant source and destination MAC addresses as permitted by the BNEP specification.
*   **Results:** This reduces per-packet overhead by **12 bytes**, increasing effective throughput on bandwidth-constrained links and reducing radio-active duty cycles.

### 3. Deterministic Single-Pass SLIP Encoding
*   **Implementation:** The SLIP (Serial Line IP) encoder/decoder is implemented as a single-pass state machine using pointer offsets. This avoids secondary buffering and minimizes CPU branching during byte-stuffing operations.
*   **Impact:** The encoder maintains line-rate throughput relative to the hardware UART/USART baud rate. By processing bytes directly between the transport layer and the peripheral registers, CPU utilization remains linear relative to throughput, ensuring stability even at high serial clock speeds.

## Protocol Implementation Notes

- **BNEP Version:** v1.0, supporting General and Compressed Ethernet formats.
- **Header Compression:** Dynamically enabled for PANU-to-NAP flows to minimize radio-on time and latency.
- **BNEP Control Packets:** Extension headers are parsed with strict bounds checking before control type dispatch.
- **Multicast Filtering:** Automatically sent after BNEP setup before DHCP.
- **DHCP Lifecycle:** Managed by lwIP's DHCP client. If DHCP discovery fails after maximum retries (`TINYPAN_DHCP_MAX_RETRIES`), TinyPAN forcibly tears down the L2CAP link. This ensures the mobile OS (iOS/Android) interface is reset, which is the most reliable way to recover from stalled routing daemons on the hotspot host.
- **State Transition Safety:** Prevents invalid transitions and guarantees state machine consistency.
- **MCU Design:** Parsing logic and static queue sizes are designed for high-availability, low-RAM environments.

## Acknowledgments

- [@dipinraj](https://github.com/dipinraj) -- Reported the GAP security documentation gap that led to the addition of `examples/esp32_app_main.c` and the ESP-IDF integration prerequisites ([#3](https://github.com/Akhil-Chaturvedi/TinyPAN/issues/3)).

## License

MIT License.
