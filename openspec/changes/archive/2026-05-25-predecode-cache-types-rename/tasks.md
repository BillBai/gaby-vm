> Prerequisite: `predecode-cache-core` is implemented (the `gaby_vm`
> `PredecodeCache` public API exists with `RegisterStatus`, `ErrorDetail`,
> `PredecodedEntry`, `CodeRange` at the `gaby_vm::` namespace top). This
> change renames those types and archives *after* `predecode-cache-core`.

## 1. Public header rename and nesting

- [x] 1.1 In `include/gaby_vm/predecode_cache.h`, move `enum class RegisterStatus : int` inside `class PredecodeCache`'s `public:` section as `enum class RegistrationStatus : int`. Preserve the underlying type (`int`), the variant names (`Ok`, `InvalidArgument`, `OverlappingRange`, `UnsupportedFeature`, `OutOfMemory`), and the variant values verbatim. Preserve the doc comment, rewording only the references to the type's old name.
- [x] 1.2 Move `struct ErrorDetail` inside `class PredecodeCache`'s `public:` section as `struct RegistrationError`. Preserve the field set (`status`, `pc`, `reason`, `missing_features`) and the doc comment, rewording only references to the type's old name. The `status` field's type becomes the nested `RegistrationStatus`.
- [x] 1.3 Move `struct PredecodedEntry` inside `class PredecodeCache`'s `public:` section as `PredecodedEntry` (same identifier, new scope). Preserve the field set (`form_hash`, `reserved`, `leaf`) and the 16-byte layout exactly. Preserve the doc comment.
- [x] 1.4 Move the `static_assert(sizeof(PredecodedEntry) == 16, "PredecodedEntry must stay 16 bytes for flat-array indexing")` into the class body, placed immediately after the nested `struct PredecodedEntry` and before `struct CodeRange`. The assert references `PredecodedEntry` unqualified (in-scope lookup resolves it).
- [x] 1.5 Move `struct CodeRange` inside `class PredecodeCache`'s `public:` section as `CodeRange` (same identifier, new scope). Preserve the field set (`start`, `size_bytes`, `entries`) and the doc comment; the `entries` pointer's pointee type becomes the nested `PredecodedEntry`.
- [x] 1.6 Change `RegisterStatus RegisterCodeRange(const void* start, size_t size_bytes)` to return the nested `RegistrationStatus`. Signature otherwise unchanged.
- [x] 1.7 Rename `const ErrorDetail* GetLastErrorDetail() const` to `const RegistrationError* GetLastRegistrationError() const`. Preserve the "never null" contract and the lifetime/threading doc comment verbatim (substituting only the type and method names).
- [x] 1.8 Change `const CodeRange* FindRange(uintptr_t pc) const` to return `const CodeRange*` resolved as the nested type. Signature otherwise unchanged.
- [x] 1.9 Delete the original namespace-top declarations of `RegisterStatus`, `ErrorDetail`, `PredecodedEntry`, `CodeRange`, and the namespace-top `static_assert`. The only namespace-top public type left in this header is `class PredecodeCache`.
- [x] 1.10 Header still builds vixl-free (`#include <cstddef>`, `<cstdint>`, `<memory>` only) and a translation unit that includes only this header still compiles.

## 2. Implementation follow-through

- [x] 2.1 In `src/gaby_vm/predecode_cache.cc`, substitute every reference: `RegisterStatus` → `PredecodeCache::RegistrationStatus`, `ErrorDetail` → `PredecodeCache::RegistrationError`. Variant names (`Ok`, `InvalidArgument`, …) are referenced through the now-nested enum (`PredecodeCache::RegistrationStatus::Ok`, …) wherever the unqualified `RegisterStatus::Ok` appears today.
- [x] 2.2 Update the `Impl::SetError(RegisterStatus status, …)` signature to `SetError(RegistrationStatus status, …)` (or the fully qualified form, depending on where `Impl` lives) and update its `last_error_` field initializer from `ErrorDetail last_error_{RegisterStatus::Ok, 0, "", ""}` to use the renamed types.
- [x] 2.3 Rename `PredecodeCache::GetLastErrorDetail` to `PredecodeCache::GetLastRegistrationError` at its definition; return type follows.
- [x] 2.4 No behavioral edits: every return-value site, every error-setting site, and every storage-allocation site carries through the existing logic byte-for-byte. The cache's all-or-nothing, append-only, shared-mutex semantics are not touched.

## 3. Marker-region pointer-type updates

- [x] 3.1 In `src/aarch64/simulator-aarch64.h`, locate the `cur_range_` field (currently `const gaby_vm::CodeRange* cur_range_ = nullptr;`) inside the `// gaby-vm BEGIN` / `END` block and change its type to `const gaby_vm::PredecodeCache::CodeRange*`.
- [x] 3.2 In `ExecuteInstructionCached`, update the two local-variable types: `const gaby_vm::CodeRange* range = cur_range_;` → `const gaby_vm::PredecodeCache::CodeRange*`, and `const gaby_vm::PredecodedEntry* entry = &range->entries[…];` → `const gaby_vm::PredecodeCache::PredecodedEntry*`.
- [x] 3.3 In `struct GabyInterpreterCursor`, change the `cur_range` field type from `const gaby_vm::CodeRange*` to `const gaby_vm::PredecodeCache::CodeRange*`. `GabySaveCursor` / `GabyRestoreCursor` need no further edits — they just pass the field through.
- [x] 3.4 Verify the marker reason text — the prose lines starting with `// design.md`, `// 详见 …`, and the `// cache 命中执行` block — is **unchanged**. The diff inside the marker block must contain only the three pointer-type-spelling edits.
- [x] 3.5 Any single-line `if`/`for`/`while` statement touched in passing takes `{}` braces and a new line, per the project-wide convention from the archived `marker-block-style-and-brace-convention` change.

## 4. Test call-site updates

- [x] 4.1 In `test/typed_register_io_test.cc`, substitute the two `gaby_vm::RegisterStatus::Ok` references with `gaby_vm::PredecodeCache::RegistrationStatus::Ok`. Add a file-scope or local `using` alias if either occurrence becomes hard to read.
- [x] 4.2 In `test/simulator_correctness.cc`, substitute the `gaby_vm::RegisterStatus status` declaration and the `status != gaby_vm::RegisterStatus::Ok` comparison to use the nested type.
- [x] 4.3 In `test/reentrancy_test.cc`, substitute the four `gaby_vm::RegisterStatus::Ok` references to use the nested type.
- [x] 4.4 In `test/shadow_runner_test.cc`, substitute the two `gaby_vm::RegisterStatus::Ok` references to use the nested type.
- [x] 4.5 In `test/workload_shadow_test.cc`, substitute the `gaby_vm::RegisterStatus` declaration to use the nested type. Also substitute `gaby_vm::ErrorDetail` → `gaby_vm::PredecodeCache::RegistrationError` and `GetLastErrorDetail` → `GetLastRegistrationError` (this file also names those two symbols, not just `RegisterStatus`).
- [x] 4.6 Confirm no other file under `test/`, `bench/`, `src/`, or `include/gaby_vm/` references `gaby_vm::RegisterStatus`, `gaby_vm::ErrorDetail`, `gaby_vm::PredecodedEntry`, or `gaby_vm::CodeRange` as symbols. `src/gaby_vm/simulator.cc` mentions `PredecodeCache::RegisterCodeRange` in a doc comment — that string is the method name and stays.

## 5. Build and acceptance

- [x] 5.1 Build under the project's standard CMake configuration; the public header, `src/gaby_vm/`, the imported `src/aarch64/` tree, all five test files, and the `bench/` binaries compile clean.
- [x] 5.2 Run `ctest` against the test build directory; every existing test case still passes — the rename touches no semantics, so no expected-output edits should be needed.
- [x] 5.3 The cache-track inline ABI is preserved: `sizeof(gaby_vm::PredecodeCache::PredecodedEntry) == 16` holds and the relocated `static_assert` fires at compile time.
- [x] 5.4 The public header advertises exactly one type at the `gaby_vm::` namespace top (`class PredecodeCache`); a `grep -nE '^(struct|enum class|class) ' include/gaby_vm/predecode_cache.h` outside the class body returns only the `PredecodeCache` line plus the private `class Impl;` inside it.
- [x] 5.5 Run `openspec validate predecode-cache-types-rename --strict` (expect `valid`).
- [ ] 5.6 Once `predecode-cache-core` is archived, run `openspec archive predecode-cache-types-rename` to apply the `predecode-cache` MODIFIED-requirement delta to the live capability spec.
