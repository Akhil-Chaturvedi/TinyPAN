# ESP32 Compile Test Stubs

This directory contains minimal stub headers and function implementations for the ESP-IDF and FreeRTOS APIs used by `ports/esp32_classic/tinypan_hal_esp32.c`. They allow the HAL to be compiled without the full ESP-IDF toolchain installed.

## What These Are

- **Not unit tests.** These files do not test HAL behavior.
- **API surface verification.** If the HAL compiles against these stubs, it will compile against the real ESP-IDF v5.5+ headers. Every type, enum value, struct field, and function signature here matches the real ESP-IDF API.
- **Documentation.** The stubs serve as a precise record of which ESP-IDF APIs the HAL depends on.

## Key Files

| File | Purpose |
|---|---|
| `esp_l2cap_bt_api.h` | L2CAP event types, callback params, API function signatures. This is the most critical file -- it defines the exact API contract. |
| `esp32_stubs.c` | Minimal stub implementations for all ESP-IDF and FreeRTOS functions. Allows the linker to succeed. |
| `esp_*.h` | ESP-IDF system header stubs (`esp_bt.h`, `esp_err.h`, `esp_log.h`, etc.) |
| `freertos/*.h` | FreeRTOS primitive stubs (queues, message buffers, semaphores, timers, event groups) |
| `unistd.h` | POSIX `read()`/`write()`/`close()` stubs used by the VFS-based I/O model |

## How to Use

Compile `tinypan_hal_esp32.c` with this directory on the include path, linking against `esp32_stubs.c`:

```bash
gcc -I tests/esp32_compile_test -I include \
    ports/esp32_classic/tinypan_hal_esp32.c \
    tests/esp32_compile_test/esp32_stubs.c \
    -c -o /dev/null
```

If this succeeds, the HAL's API usage is correct for ESP-IDF v5.5+.

## Updating for New ESP-IDF Versions

When updating the HAL for a new ESP-IDF version, update these stubs to match the new API surface. Any compile failure against the stubs indicates an API incompatibility that needs to be fixed in the HAL.

The `esp_l2cap_bt_api.h` header is verified against the ESP-IDF master source tree. The comment at the top of that file references the specific source lines it was traced from.
