## 1. Public header — `include/gaby_vm/registers.h`

- [x] 1.1 Create the new file `include/gaby_vm/registers.h` with a project license header and include guards.
- [x] 1.2 Declare `enum class GpRegister : uint8_t` covering `X0`..`X30` (0..30), `LR = X30`, `SP = 31`, `PC = 32` in the `gaby_vm` namespace.
- [x] 1.3 Declare `enum class VRegister : uint8_t` covering `V0`..`V31` (0..31).
- [x] 1.4 Declare `enum class SysRegister : uint8_t` covering `NZCV`, `FPCR`, `FPSR`, `BType`.
- [x] 1.5 Move `struct VRegisterValue { uint64_t lo; uint64_t hi; }` from `simulator.h` into `registers.h`; leave the type visible via `simulator.h` through include rather than redeclaration.
- [x] 1.6 Declare `struct RegisterFile` with the exact field order from the spec: `uint64_t x[31]; uint64_t sp; uint64_t pc; VRegisterValue v[32]; uint32_t nzcv; uint32_t fpcr; uint32_t fpsr; uint32_t btype;`.
- [x] 1.7 Add a `static_assert(sizeof(RegisterFile) == 31*8 + 8 + 8 + 32*16 + 4*4, ...)` next to the declaration to freeze the layout.
- [x] 1.8 Add `static_assert(std::is_standard_layout_v<RegisterFile> && std::is_trivially_copyable_v<RegisterFile>, ...)`.
- [x] 1.9 Declare the typed write structs `GpWrite { GpRegister reg; uint64_t value; }`, `VWrite { VRegister reg; VRegisterValue value; }`, `SysWrite { SysRegister reg; uint32_t value; }`.
- [x] 1.10 Declare `using RegisterWrite = std::variant<GpWrite, VWrite, SysWrite>;` (include `<variant>`).
- [x] 1.11 Confirm the header is self-contained: it `#include`s only `<cstdint>`, `<type_traits>`, `<variant>` (no gaby-vm dependencies, no VIXL headers, no `vixl::*` references).

## 2. Public header — `include/gaby_vm/simulator.h`

- [x] 2.1 Add `#include <gaby_vm/registers.h>` and `#include <span>` near the existing `#include <memory>` block.
- [x] 2.2 Delete `void WriteXRegister(unsigned, uint64_t);` and `uint64_t ReadXRegister(unsigned) const;`.
- [x] 2.3 Delete `void WriteSp(uint64_t);` and `uint64_t ReadSp() const;`.
- [x] 2.4 Delete `void WritePc(uintptr_t);` and `uintptr_t ReadPc() const;`.
- [x] 2.5 Delete `VRegisterValue ReadVRegister(unsigned) const;`.
- [x] 2.6 Delete the old `uint32_t ReadNzcv/Fpcr/Fpsr/BType() const;` accessors (now reachable via typed `Read(SysRegister, …)`).
- [x] 2.7 Add `void Write(GpRegister reg, uint64_t value);` and `uint64_t Read(GpRegister reg) const;`.
- [x] 2.8 Add `void Write(VRegister reg, VRegisterValue value);` and `VRegisterValue Read(VRegister reg) const;`.
- [x] 2.9 Add `void Write(SysRegister reg, uint32_t value);` and `uint32_t Read(SysRegister reg) const;`.
- [x] 2.10 Add `RegisterFile ReadAll() const;` and `void WriteAll(const RegisterFile& file);`.
- [x] 2.11 Add `void Write(std::span<const RegisterWrite> writes);`.
- [x] 2.12 Move the existing `WritePc` re-entrancy docstring verbatim onto `Write(GpRegister, uint64_t)`, scoped to the `GpRegister::PC` case; name `RunFrom`, `StepOnce(entry_pc)`, and `DebugStepOnce(entry_pc)` as the re-entrant alternatives.
- [x] 2.13 Add a docstring on `WriteAll` stating it is for top-level use only (MUST NOT be called from inside a leaf executed by an enclosing run).

## 3. Pimpl — `src/gaby_vm/simulator.cc`

- [x] 3.1 In `Simulator::Impl`, implement `Write(GpRegister, uint64_t)` dispatching on the enum: `X0..X30` → vixl `Simulator::WriteXRegister(code, value)`; `SP` → vixl `Simulator::WriteSp(value)`; `PC` → vixl `Simulator::WritePc(value)`. `LR` shares `X30`'s case.
- [x] 3.2 Implement `Read(GpRegister)` const-mirror with the same fan-out.
- [x] 3.3 Implement `Write(VRegister, VRegisterValue)` and `Read(VRegister)` against vixl's V-register accessors (reuse the path the existing `ReadVRegister(unsigned)` used).
- [x] 3.4 **Audit before implementing 3.5**: audit complete — VIXL exposes everything needed via public surface (`SimSystemRegister::SetRawValue` for NZCV/FPCR; `WriteNextBType + UpdateBType` for BType; FPSR is unmodeled in VIXL so the Pimpl gains a `uint32_t fpsr_storage` slot to satisfy the round-trip contract). **No imported-code edits required.**
- [x] 3.5 Implement `Write(SysRegister, uint32_t)` and `Read(SysRegister)` for NZCV/FPCR/FPSR/BType against the routes confirmed in 3.4.
- [x] 3.6 Implement `ReadAll()` — populate every field of `RegisterFile` by calling the typed `Read` paths above; assert the V-array stride matches `sizeof(VRegisterValue)` (or copy element-by-element to sidestep alignment assumptions).
- [x] 3.7 Implement `WriteAll(const RegisterFile&)` — invoke each typed `Write` in field order.
- [x] 3.8 Implement `Write(std::span<const RegisterWrite>)` using `std::visit` with an `overloaded` lambda set (`template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };`) to dispatch each element to the matching typed `Write`.
- [x] 3.9 Add range-check abort inside each typed `Write` (e.g. `assert`-style guard plus `VIXL_ABORT` or equivalent diagnostic) that fires when an enum value is constructed via `static_cast` outside its declared range; the diagnostic SHALL identify the offending element's position when called from the span-write path.

## 4. Internal call-site migration

- [x] 4.1 Migrate `src/gaby_vm/shadow_runner.cc` to the typed surface (every `ReadXRegister(i)` → `Read(GpRegister{i})` etc.; every `ReadNzcv()` → `Read(SysRegister::NZCV)`).
- [x] 4.2 Migrate `test/reentrancy_test.cc` — pay special attention to PC-write sites and confirm the re-entrancy hazard docstring's new wording is consistent with the test scenarios.
- [x] 4.3 Migrate `test/shadow_runner_test.cc` to the typed surface. (No changes needed — file only uses the `gaby_vm::testing::ShadowRunner` public API, which is intentionally out of scope for this change.)
- [x] 4.4 Migrate `test/workload_shadow_test.cc` to the typed surface. (No changes needed — same reason as 4.3.)
- [x] 4.4b **Added during implementation.** Migrate `test/simulator_correctness.cc` — the surveyed migration list missed this file. It drives `gaby_vm::Simulator` directly and had ~25 call sites that needed conversion to the typed `Write(GpRegister::Xn, …)` / `Read(SysRegister::NZCV)` forms.
- [x] 4.4c **Added during implementation.** Bump C++ standard to 20 in `CMakeLists.txt` (`CMAKE_CXX_STANDARD 20`) and `src/CMakeLists.txt` (`target_compile_features(gaby_vm PUBLIC cxx_std_20)`). Required to use `std::span`. Add `-Wno-deprecated-enum-enum-conversion` to `gaby_vm_apply_imported_compile_flags` in `cmake/CompileFlags.cmake` to silence the imported VIXL warnings that became active under C++20.
- [x] 4.5 `git grep -nE '\b(WriteXRegister|ReadXRegister|WriteSp|ReadSp|WritePc|ReadPc|ReadVRegister|ReadNzcv|ReadFpcr|ReadFpsr|ReadBType)\b' src/gaby_vm/ test/` returns no matches against the public `gaby_vm::Simulator` (matches inside `src/aarch64/` or against `vixl::aarch64::Simulator` are expected and unaffected). Verified: only `test/simulator_smoke.cc:45 sim.WritePc(pc)` remains and that's a call on `vixl::aarch64::Simulator` via the privileged build pattern. `ShadowRunner`'s public API (its own `WriteXRegister`/`ReadXRegister`/`WriteSp`/`ReadSp` methods) is intentionally retained.
- [x] 4.6 If any benchmark or other in-tree target references the removed accessors, migrate it in the same change. (Verified clean — `bench/` has no references.)

## 5. Tests covering the new surface

- [x] 5.1 Add a typed-accessor round-trip test (single-register write + read for each of GP/V/Sys). Lives in `test/typed_register_io_test.cc` as `test_gp_register_round_trip`, `test_v_register_round_trip`, and the GP/V parts of `test_sysreg_round_trips`.
- [x] 5.2 Add an SP-vs-XZR disambiguation test: write `GpRegister::SP`, then verify both that `Read(GpRegister::SP)` reflects the write and that nothing has clobbered the X-register slot 31 codepath that the simulator uses for XZR semantics (exercise via an instruction sequence if needed). Covered by `test_sp_vs_xzr_disambiguation` plus the existing `simulator_correctness` LDR/STR-via-SP suite.
- [x] 5.3 Add a PC-write seating test: `Write(GpRegister::PC, entry)` followed by `StepOnce()` executes the instruction at `entry`. Covered by `test_pc_seating_executes_at_entry` (seats X0=41, PCs to `ADD x0,x0,#1`, steps, expects X0=42).
- [x] 5.4 Add a system-register write+read round-trip for each of NZCV/FPCR/FPSR/BType. Covered by `test_sysreg_round_trips`.
- [x] 5.5 Add a `ReadAll → instructions → WriteAll → ReadAll` round-trip test demonstrating field-by-field equality with the original snapshot. Covered by `test_read_all_write_all_round_trip` (uses `std::memcmp` for byte-equal).
- [x] 5.6 Add a `WriteAll` coverage test: construct a `RegisterFile` with distinct sentinel values in every slot, call `WriteAll`, then read each slot back via typed `Read` and assert equality. Covered by `test_write_all_covers_every_slot`. Note: PC sentinel uses `0x0000000000400000` because VIXL's `WritePc` strips PAC tag bits from the top of the address — any value with the top byte set would read back as zero.
- [x] 5.7 Add a mixed `std::span<RegisterWrite>` batch-write test (one `GpWrite`, one `VWrite`, one `SysWrite`), verifying order-equivalent results to three individual `Write` calls. Covered by `test_span_batch_write_mixed`.
- [x] 5.8 Add a fail-fast batch test: a span containing a `static_cast<GpRegister>(99)` element causes the call to abort. Covered by a separate binary `test/typed_register_io_abort_test.cc` that forks a child to run the bad Write and asserts in the parent that the child died from a signal. `WILL_FAIL`-on-`SIGABRT` was tried first and turned out not to flip "Subprocess aborted" outcomes; the fork+waitpid pattern is portable across POSIX targets and self-validates with a normal 0/1 exit code.
- [x] 5.9 Confirm the compile-time properties hold by adding a small TU under `test/` that consumes `std::is_standard_layout_v<RegisterFile>` and `std::is_trivially_copyable_v<RegisterFile>` plus the `sizeof` assertion. Implemented as namespace-scope `static_assert`s at the top of `test/typed_register_io_test.cc` (the property is a build-time check, not a runtime test; a separate TU would just shift where it lives).

## 6. Verification

- [x] 6.1 `cmake --preset dev-debug && cmake --build --preset dev-debug && ctest --preset dev-debug` — all 8 tests pass (the 6 pre-existing tests plus `typed_register_io` and `typed_register_io_abort`).
- [x] 6.2 `cmake --preset dev-release && cmake --build --preset dev-release && ctest --preset dev-release` — all 8 tests pass.
- [x] 6.3 `git grep -nE '\b(WriteXRegister|ReadXRegister|WriteSp|ReadSp|WritePc|ReadPc|ReadVRegister|ReadNzcv|ReadFpcr|ReadFpsr|ReadBType)\b' include/gaby_vm/simulator.h` returns empty. **Verified.**
- [x] 6.4 `git grep -nE 'vixl|aarch64/' include/gaby_vm/registers.h` returns empty. **Verified.**
- [x] 6.5 Task 3.4 confirmed no imported-code edits are needed (VIXL exposes everything via public surface). `git diff --stat HEAD -- src/aarch64/ src/utils-vixl* src/cpu-features* src/compiler-intrinsics-vixl* src/platform-vixl.h src/globals-vixl.h` shows no entries. **Verified.**
- [x] 6.6 `openspec validate simulator-register-io-api` — `Change 'simulator-register-io-api' is valid`. **Verified.**
