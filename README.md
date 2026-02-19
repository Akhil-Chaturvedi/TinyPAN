# TinyPAN

A minimal, portable Bluetooth PAN (Personal Area Network) library that enables microcontrollers to access the internet via a phone's standard Bluetooth tethering feature.

## 🎯 What is TinyPAN?

TinyPAN allows your MCU to connect to the internet by simply having the user turn on **Bluetooth Tethering** on their Android phone. No custom app required!

```
┌─────────────┐         Bluetooth Classic          ┌─────────────┐
│   Your MCU  │◄─────────────────────────────────►│   Phone     │
│  (TinyPAN)  │            PAN/BNEP                │   (NAP)     │
└─────────────┘                                    └──────┬──────┘
                                                          │
                                                          │ WiFi/4G/5G
                                                          ▼
                                                    ┌───────────┐
                                                    │  Internet │
                                                    └───────────┘
```

## ✨ Features

- **No phone app required** - Uses standard Bluetooth tethering
- **Minimal footprint** - Designed for resource-constrained MCUs
- **Portable** - Hardware abstraction layer for easy porting
- **Reliable** - Automatic reconnection with exponential backoff
- **Battle-tested networking** - Uses lwIP for TCP/IP stack

## 🚀 Quick Start

```c
#include "tinypan.h"

void my_event_handler(tinypan_event_t event, void* user_data) {
    switch (event) {
        case TINYPAN_EVENT_IP_ACQUIRED:
            printf("We're online!\n");
            break;
        case TINYPAN_EVENT_DISCONNECTED:
            printf("Connection lost\n");
            break;
    }
}

int main(void) {
    // Configure
    tinypan_config_t config;
    tinypan_config_init(&config);
    
    // Set phone's Bluetooth address
    uint8_t phone_addr[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    memcpy(config.remote_addr, phone_addr, 6);
    
    // Initialize
    tinypan_init(&config);
    tinypan_set_event_callback(my_event_handler, NULL);
    
    // Start connecting
    tinypan_start();
    
    // Main loop
    while (1) {
        tinypan_process();  // Must be called periodically
        
        if (tinypan_is_online()) {
            // Use lwIP sockets here!
        }
    }
}
```

## 📁 Project Structure

```
TinyPAN/
├── include/
│   ├── tinypan.h           # Main public API
│   ├── tinypan_config.h    # Configuration options
│   └── tinypan_hal.h       # Hardware abstraction layer
├── src/
│   ├── tinypan.c           # Main implementation
│   ├── tinypan_bnep.c      # BNEP protocol
│   └── tinypan_supervisor.c # Connection state machine
├── hal/
│   └── mock/               # Mock HAL for testing
└── tests/
    └── test_bnep.c         # Unit tests
```

## 🔧 Building

### Requirements
- CMake 3.12+
- C99 compiler

### Build with CMake

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

### Run Tests

```bash
cd build
ctest -V
```

## 🔌 Porting to Your Hardware

To port TinyPAN to a new platform, implement the functions in `tinypan_hal.h`:

| Function | Description |
|----------|-------------|
| `hal_bt_init()` | Initialize Bluetooth stack |
| `hal_bt_l2cap_connect()` | Connect to remote L2CAP channel |
| `hal_bt_l2cap_send()` | Send data over L2CAP |
| `hal_get_tick_ms()` | Get millisecond timestamp |
| ... | See `tinypan_hal.h` for full API |

## 📋 Status

- [x] Project structure
- [x] Public API design
- [x] BNEP packet building/parsing
- [x] Supervisor state machine
- [x] Mock HAL for testing
- [ ] lwIP integration
- [ ] Linux/BlueZ HAL
- [ ] ESP32 HAL

## 📄 License

MIT License - see LICENSE file.

## 🤝 Contributing

Contributions welcome! Please read the architecture documentation in `docs/` first.
