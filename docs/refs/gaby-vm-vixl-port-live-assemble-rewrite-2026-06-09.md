# Rewriting `vixl_port`: From Frozen Fixtures to Live Assembly, 2026-06-09

[Chinese version](gaby-vm-vixl-port-live-assemble-rewrite-2026-06-09.zh-cn.md)

This design records the decision to rebuild the `vixl_port` test suite. The old
model extracted instruction bytes during authorship, committed frozen fixtures,
then replayed those bytes at test time. The new model assembles upstream VIXL
`TEST()` bodies at test time and runs each body on both gaby-vm execution modes.

It replaces the earlier frozen-fixture design
`gaby-vm-vixl-sim-test-port-design-2026-06-08.md`. That route could run broad
coverage, but it had an unavoidable blind spot: memory-access semantics.

This is a design note, not an implementation schedule or OpenSpec change.
Source references use repository-relative paths. `../vixl` means the reference
VIXL checkout.

## Summary

- Frozen fixtures cannot represent memory-access tests. Upstream load/store
  tests bake host addresses into instructions. Once those bytes are replayed in
  a different process, the baked addresses point at invalid memory. The integer
  suite dropped about 174 of 264 cases for this reason, including
  `LDR`, `STR`, `LDP`, `STP`, atomics, exclusive accesses, and CAS.
- Live assembly fixes the root cause. The test binary links a test-only copy of
  the VIXL assembler and assembles the test body in the current process. A
  `Mov(reg, reinterpret_cast<uintptr_t>(scratch))` bakes a real address from the
  current process, so memory tests run against valid in-process memory.
- The no-JIT boundary stays intact. Assembled bytes live in ordinary `malloc`
  memory through `VIXL_CODE_BUFFER_MALLOC`. gaby-vm feeds those bytes to its
  decoder and never executes them natively.
- The assembler belongs only to tests. It lives under
  `test/test_support/vixl_asm/`, is isolated by CMake, and is never linked into
  the shipping `gaby_vm` library.

## 1. Why the Fixture Model Was Replaced

The frozen-fixture model used real VIXL during authorship to assemble each
`TEST()` body and capture expected state. Runtime tests replayed committed
bytes. That made CTest self-contained, but memory tests looked like this:

```cpp
uint64_t src[2] = {...};
__ Mov(x17, reinterpret_cast<uintptr_t>(src));
__ Ldr(w0, MemOperand(x17));
ASSERT_EQUAL_64(0x76543210, x0);
```

The `Mov` instruction encodes the address of `src` from the extraction process.
After the bytes are committed and replayed later, that address is stale. The
extractor therefore had to drop tests containing load/store, ADR/ADRP, and
indirect register branches. That meant the guard rail did not cover memory
execution paths, which are exactly where dispatch and operand-predecode work can
regress.

Patching the frozen model would need a heavy runtime binding system: allocate
buffers, seed registers, remove baked address materialization, add a memory
oracle, and reset both tracks. Live assembly is simpler. Assemble against the
real runtime buffer address and the problem disappears.

## 2. Verified Ground Facts

1. VIXL simulator memory uses the same address space. Guest virtual addresses
   are host pointers. `Memory::Read/Write` in `simulator-aarch64.h` directly
   casts `AddressUntag(addr)` and copies bytes. The stack follows the same
   model. If the test assembles `Mov(reg, real_address)`, the VIXL reference
   simulator, gaby cache mode, and gaby decoder mode all see the same address.
2. gaby-vm imported the simulator and decoder chain, but not the assembler
   chain. The missing files are well bounded. The low-level shared headers used
   by `MacroAssembler` are already compiled into `gaby_vm`.
3. The shipping `gaby_vm` library has no link dependencies in
   `Sources/gaby_vm/src/CMakeLists.txt`. A test-only library cannot flow back
   into `gaby_vm.a` through a link edge.
4. The import SHA is `160c445`, `../vixl` HEAD from 2026-05-14. Before copying
   Tier 0 files, check out that SHA and record it in
   `vixl-extraction-map.md`. The SHA pin prevents silent ODR drift between
   copied assembler `.cc` files and the modified imported headers.

## 3. Test-Only Assembler Island

### 3.1 Location and Marker

Directory: `test/test_support/vixl_asm/`.

Every copied VIXL file keeps its original copyright header. Add this marker
below it:

```cpp
// TEST-ONLY - NOT part of the gaby_vm library. Do not include or link this from
// gaby_vm or anything that embeds it.
```

The island root also has a `.clang-format` with `DisableFormat: true` so
format-on-save does not rewrite upstream files and break diffability.

### 3.2 Files to Copy at the Pinned SHA

Assembler core, not present in gaby-vm:

- `assembler-aarch64.{h,cc}`
- `macro-assembler-aarch64.{h,cc}`
- `assembler-sve-aarch64.cc`
- `macro-assembler-sve-aarch64.cc`
- `assembler-base-vixl.h`
- `code-buffer-vixl.{h,cc}`
- `code-generation-scopes-vixl.h`
- `macro-assembler-interface.h`
- `invalset-vixl.h`

VIXL test infrastructure:

- `test-utils-aarch64.{h,cc}`
- `test-simulator-inputs-aarch64.h`
- `test-utils.h`
- `test-runner.{h,cc}`

Upstream test bodies:

- `test-assembler-aarch64.cc`
- `test-assembler-fp-aarch64.cc`
- `test-assembler-neon-aarch64.cc`

gaby-vm-owned stub:

- `test-utils-stub.cc`, used to avoid VIXL's executable-memory helper.

### 3.3 Isolation From `gaby_vm`

- The test library is named `gaby_vm_vixl_asm_testonly`.
- It has no `::` alias.
- It builds only when `GABY_VM_BUILD_TESTS` is enabled.
- `Sources/gaby_vm/src/CMakeLists.txt` does not list any assembler island
  source.
- Test executables link it `PRIVATE`.

### 3.4 ODR and Duplicate Symbol Avoidance

`MacroAssembler` and `Simulator` share imported low-level headers. Those symbols
already come from `gaby_vm`. The island must not copy shared headers or sources
again. Its include directories put `Sources/gaby_vm/src` first, so includes such
as `operands-aarch64.h` and `simulator-aarch64.h` resolve to the imported gaby
copy. The island private-links `gaby_vm::gaby_vm`, so shared leaf symbols are
defined once.

## 4. Test Flow and Oracles

The harness redefines VIXL test macros, then includes upstream test `.cc` files.
This is the same macro-capture approach proven by the old extraction tool, but
`RUN()` now executes both gaby modes instead of recording frozen bytes.

Each test case owns:

- a real `MacroAssembler` writing into a `VIXL_CODE_BUFFER_MALLOC` buffer;
- a VIXL reference `Simulator`, used only to compute absolute expected state;
- two gaby `Simulator` instances, one with `PredecodeCache` for cache mode and
  one without for decoder mode;
- a shared stack and scratch buffer for memory tests.

`START()` and `END()` mirror the old extractor. `END()` forces literal pools
inline with `EmitLiteralPool(kBranchRequired)`, dumps reference state with
`core.Dump(&masm)`, and terminates with `Br(xzr)` instead of relying on LR.

Oracles:

- Absolute oracle: every `ASSERT_EQUAL_*` checks both gaby modes against the
  VIXL reference simulator's dumped state.
- Differential oracle: cache-mode `RegisterFile` equals decoder-mode
  `RegisterFile`.
- Memory works naturally because all three simulators use the same runtime
  scratch addresses.

The harness explicitly seeds entry state for both gaby modes to match VIXL
`Simulator::ResetState()`. One startup assertion checks field-by-field
equivalence.

Remaining skips cover real capability gaps, such as unsupported features
through a deny-list and named quarantines for runtime calls, `dc_zva`, system
instructions, MOPS, and similar cases. The old structural
`IsNonPortableInstr` filter disappears because load/store and PC-relative cases
can now run.

## 5. no-RWX Boundary

VIXL's `test-utils.cc` contains `<sys/mman.h>` and `ExecuteMemory()`, which maps
executable memory and calls assembled bytes natively. That violates the no-JIT,
no-RWX, iOS-friendly boundary.

The island therefore does not copy `test-utils.cc`. It provides
`test-utils-stub.cc` for the symbols actually needed by simulator tests.
`ExecuteMemory()` is implemented as no-op or abort-on-call.

`code-buffer-vixl.cc` has mmap/mprotect paths behind `VIXL_CODE_BUFFER_MMAP`.
The island builds with `-DVIXL_CODE_BUFFER_MALLOC`, so code buffers are ordinary
malloc/free and `SetExecutable()` / `SetWritable()` are no-ops.

The test island contains no `sys/mman` use and never executes assembled bytes
natively.

## 6. Migration Order

1. Pin the SHA: `git -C ../vixl checkout 160c445`, then record the SHA in
   `vixl-extraction-map.md`.
2. Land the island without connecting the new harness. Copy the file groups,
   add the stub and marker, strip the self-include, add `.clang-format`, and
   create `gaby_vm_vixl_asm_testonly`. Keep the frozen suite green.
3. Add an ODR/link smoke gate before harness work. Link the island with
   `gaby_vm.a`, assemble `Mov(x0, 1) + FinalizeCode`, run one body through the
   VIXL reference simulator, and use `nm` to check for duplicate `vixl::`
   symbols, undefined SVE symbols, and `sys/mman` linkage.
4. Add `gaby_two_track_macros.h` and `vixl_port_oracle.{h,cc}`.
5. Bring up integer first as temporary `vixl_port_integer_live`, side by side
   with the old suite. Verify that included case count grows beyond the old
   memory-blind coverage.
6. Switch integer to the live path. Repeat for FP and NEON, keeping
   `ctest -R vixl_port` green after each step.
7. Remove frozen fixture headers, runner files, generated fixtures, and
   manifests.
8. Remove the extraction tool last: `tools/vixl_test_extract/`, the top-level
   `GABY_VM_BUILD_VIXL_EXTRACT` option, and its `add_subdirectory`.
9. Update boundary docs: `vixl-extraction-map.md`, `architecture.md`,
   `testing.md`, and `AGENTS.md`.

## 7. Risks

- SVE symbol completeness: non-SVE assembler headers declare out-of-line members
  implemented in SVE `.cc` files. Missing files show up at link time.
- Stub symbol set: `test-utils-stub.cc` must provide only what the simulator
  path needs. Link errors expose gaps.
- Header version drift: copied `.cc` files compile against imported headers that
  gaby-vm has modified. The pinned SHA and extraction-map record keep the island
  auditable.
- Entry-state drift: construct both gaby modes from the same explicit state and
  assert equivalence to VIXL reset state.
- Runtime memory or branch faults: keep named quarantines, crash/hang guards,
  and per-case instruction limits.
- Deleting the old tool too early: keep it until all three live families are
  green and the new harness has absorbed the useful crash guard pieces.
- Coverage is no longer frozen. New failures may expose real missing leaf
  support instead of disappearing into an extractor skip list.

## 8. Decisions and Open Details

Decided on 2026-06-09:

- Directory: `test/test_support/vixl_asm/`.
- Marker: short, plain "test-only" wording.
- Copy files instead of linking `../vixl`, so tests remain self-contained.
- Skip SVE execution. SVE assembler `.cc` files exist only to satisfy linker
  symbols.
- Keep the extraction tool until the end of the migration.

Implementation-time details:

- Exact `test-utils-stub.cc` symbol set is determined by linker errors.
- If one family is too slow, split it further in CTest.

## 9. Index

- Superseded frozen-fixture design:
  `gaby-vm-vixl-sim-test-port-design-2026-06-08.md` and its plan.
- Triggering issue: the old ported suite did not cover memory-access semantics,
  while Apple Silicon also made broad SVE execution irrelevant for the target
  workload shape.
- Affected boundary docs:
  [`vixl-extraction-map.md`](./vixl-extraction-map.md),
  [`../architecture.md`](../architecture.md),
  [`../testing.md`](../testing.md), and
  [`../../AGENTS.md`](../../AGENTS.md).
