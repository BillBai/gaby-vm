## ADDED Requirements

### Requirement: Simulator constructor rejects undersized stack buffers

The `gaby_vm::Simulator(PredecodeCache*, void* stack_buffer, size_t stack_size)` constructor SHALL reject `stack_size` values strictly less than a documented minimum, exposed as the public `static constexpr` member `gaby_vm::Simulator::kMinStackSize`. On rejection the constructor SHALL abort via `VIXL_ABORT_WITH_MSG` (or equivalent) with a diagnostic that names both the rejected `stack_size` and the `kMinStackSize` value. The minimum SHALL be greater than or equal to the sum of the imported `vixl::aarch64::SimStack` default `limit_guard_size_` plus `usable_size_` (currently `4 * 1024 + 8 * 1024 = 12288` bytes in `src/aarch64/simulator-aarch64.h`). `stack_size` values at or above the minimum SHALL construct a usable `Simulator` whose initial guest SP is set to the 16-byte-aligned top of `stack_buffer`, exactly as today.

#### Scenario: Below-minimum stack size aborts with a diagnostic

- **WHEN** a `gaby_vm::Simulator` is constructed with `stack_size = 0`, `stack_size = 16`, or any value strictly less than `gaby_vm::Simulator::kMinStackSize`
- **THEN** the constructor aborts before returning, and the abort diagnostic includes both the literal text "kMinStackSize" and a decimal representation of the rejected `stack_size`

#### Scenario: At-minimum stack size constructs normally

- **WHEN** a `gaby_vm::Simulator` is constructed with `stack_size == gaby_vm::Simulator::kMinStackSize`
- **THEN** the constructor returns a usable `Simulator` whose initial guest SP, observed via `Read(GpRegister::SP)`, equals the 16-byte-aligned top of `stack_buffer`

#### Scenario: kMinStackSize matches or exceeds the VIXL SimStack default total

- **WHEN** the value of `gaby_vm::Simulator::kMinStackSize` is compared with the default value of `vixl::aarch64::SimStack::limit_guard_size_` plus `vixl::aarch64::SimStack::usable_size_` in `src/aarch64/simulator-aarch64.h`
- **THEN** `kMinStackSize` is greater than or equal to that sum, enforced at compile time by a `static_assert` in `src/gaby_vm/simulator.cc`
