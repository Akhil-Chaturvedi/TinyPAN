# TinyPAN Simulation-First Roadmap

This roadmap is optimized for the current constraint (no access to target Bluetooth hardware) and the product requirement (TinyPAN must remain light and fast for always-on MCU usage).

## North-star outcome

A fully simulation-validated TinyPAN runtime that:

- reliably transitions `IDLE -> CONNECTING -> BNEP_SETUP -> DHCP -> ONLINE`,
- has deterministic reconnect/recovery behavior for failures,
- can validate lwIP-on runtime data flow under simulation,
- enforces explicit footprint/performance budgets in CI.

## Milestone M1 - Budget contract + measurement gates

Scope:

1. Define explicit default budgets for core TinyPAN objects.
2. Add an automated budget check script and wire it into CTest.
3. Keep limits configurable via environment variables so they can be tightened over time.

Initial default budgets (host build baseline, non-optimized):

- Total `.text` for core objects (`tinypan.c`, `tinypan_bnep.c`, `tinypan_supervisor.c`) <= **20000** bytes.
- Total `.bss` for core objects <= **3000** bytes.

Acceptance:

- `ctest --test-dir build -V` includes and passes `SizeBudgetCheck`.

## Milestone M2 - Simulation truth-model expansion

Scope:

1. Expand state-machine tests for timeout/retry/reconnect edges.
2. Add deterministic assertions for reconnect backoff behavior and max-attempt handling.
3. Keep all new tests HAL-mock driven (no hardware dependency).

Acceptance:

- CTest includes explicit pass/fail cases for connect timeout, BNEP setup timeout exhaustion, and reconnect attempt policy.

## Milestone M3 - Complete lwIP ON-path in simulation

Scope:

1. Promote `TINYPAN_ENABLE_LWIP=ON` from partial/stub acceptance to end-to-end simulation acceptance.
2. Validate frame bridge in both directions:
   - BNEP ingress -> lwIP netif input,
   - lwIP netif output -> BNEP transmit path.
3. Keep deterministic DHCP/IP acquisition validation in tests.

Acceptance:

- Dedicated lwIP-on simulation test target passes in CTest and verifies ONLINE transition through netif status path.

## Milestone M4 - Low-footprint + low-latency optimization

Scope:

1. Reduce copies/temporary buffers in hot paths.
2. Minimize persistent state and static buffer pressure.
3. Add process-loop timing micro-benchmarks under simulation.

Acceptance:

- Budget gates remain green after optimizations.
- Timing benchmark establishes and enforces a baseline for `tinypan_process()` idle and active cycles.

## Milestone M5 - Long-run soak and robustness

Scope:

1. Add long-run simulation soak tests with repeated connect/disconnect/reconnect cycles.
2. Validate no state deadlock and stable memory profile over long loops.

Acceptance:

- Soak target passes for a fixed large cycle count with no assertion failures.

## Operating principles for all milestones

- Simulation-first and deterministic: every feature must be testable via existing mock HAL or deterministic simulator extensions.
- Resource-aware by default: no change merges without budget visibility.
- Incremental tightening: start with practical budgets, then ratchet downward as code is optimized.
