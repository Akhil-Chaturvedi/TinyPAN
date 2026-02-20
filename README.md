# TinyPAN

TinyPAN is a C library for Bluetooth PAN (BNEP over L2CAP) client behavior with a portable HAL interface.

It is structured for embedded-style integration and includes:
- core state machine and BNEP packet handling,
- a mock HAL used by tests,
- a Linux BlueZ HAL prototype,
- a bundled lwIP source tree (not fully wired into the default CMake flow).

## Overview

Intended data path:

```
MCU/Application <-> TinyPAN <-> Bluetooth Classic (PAN/BNEP) <-> Phone (NAP) <-> Internet
```

Current repository state is mixed: some components are implemented and tested, while others are partial or environment-dependent. See [Implementation status](#implementation-status).

## Repository layout

```
TinyPAN/
├── include/
│   ├── tinypan.h
│   ├── tinypan_config.h
│   └── tinypan_hal.h
├── src/
│   ├── tinypan.c
│   ├── tinypan_bnep.c
│   ├── tinypan_supervisor.c
│   └── tinypan_lwip_netif.c
├── tests/
│   ├── test_bnep.c
│   ├── test_supervisor.c
│   ├── test_integration.c
│   ├── dhcp_sim.c
│   └── dhcp_sim.h
├── hal/
│   ├── mock/
│   └── linux/
├── lib/
│   └── lwip/
└── examples/
```

## Build (CMake)

Requirements:
- CMake >= 3.12
- C99 compiler

Build from repository root:

```bash
cmake -S . -B build
cmake --build build
```

Optional toggle:
- `-DTINYPAN_ENABLE_LWIP=ON` enables runtime lwIP hook calls in `tinypan.c`/supervisor.
- In this repository state, that mode links to a stub backend (`src/tinypan_lwip_stub.c`) so the library remains linkable while full lwIP backend wiring is still in progress.
- This does **not** yet provide a full working lwIP data path by itself.

## Tests

### Tests wired into CTest

```bash
ctest --test-dir build -V
```

At the time of writing, CTest runs:
- `BNEPTests` (`tests/test_bnep.c`)
- `SupervisorTests` (`tests/test_supervisor.c`)
- `IntegrationFlowTests` (`tests/test_integration.c` + `tests/dhcp_sim.c`)

## Linux HAL prototype

There is a Linux-specific Makefile under `hal/linux`:

```bash
make -C hal/linux test
```

`demo_linux` in that Makefile requires BlueZ development headers/libraries (for example `libbluetooth-dev` on Debian/Ubuntu-like systems).

## Implementation status

- Implemented and covered by default tests:
  - BNEP frame build/parse primitives
  - Connection supervisor state transitions
  - Mock HAL-based end-to-end state progression to ONLINE via simulated IP-acquired hook, including DHCP packet framing checks
- Implemented but not fully integrated in runtime path:
  - lwIP netif glue (`src/tinypan_lwip_netif.c`) is present and basic runtime hook points are available via `TINYPAN_ENABLE_LWIP`
  - current CMake path uses stubbed lwIP hook implementations (`src/tinypan_lwip_stub.c`) to keep linkability while backend integration remains incomplete
- Environment-dependent:
  - Linux BlueZ demo (`hal/linux/demo_linux`)

## Porting notes

To use TinyPAN on target hardware, implement the functions declared in `include/tinypan_hal.h` for your Bluetooth stack, timers, and data-path callbacks.

## Documentation consistency checklist

When changing behavior, update this README in the same change if any of the following are touched:
- build targets (`CMakeLists.txt`, `hal/linux/Makefile`),
- test targets or test wiring (`tests/` and CTest registration),
- public API (`include/tinypan.h`, `include/tinypan_hal.h`, `include/tinypan_config.h`),
- repository layout sections in this document.

Keep statements factual: only document behavior that is currently implemented and reproducible.

## License

MIT License.
