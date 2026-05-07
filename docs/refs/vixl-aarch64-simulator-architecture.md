# VIXL AArch64 Simulator Architecture

> Annotated map of the VIXL AArch64 simulator that Gaby-VM imports.
> Citations are paths inside `../vixl/` (relative to this repo). Inline
> path/line references use `path:line` to anchor specific declarations.

The simulator is a single C++ class plus a small handful of cooperating
helpers. It is single-threaded, owns no shared global state, and reads
guest memory as ordinary host pointers. It depends on a Decoder for
instruction classification, a CPUFeaturesAuditor for feature
gating, and (optionally) a PrintDisassembler and Debugger for
diagnostics.

## Simulator class entry points

The Simulator is declared at `src/aarch64/simulator-aarch64.h:1288`:

```cpp
class Simulator : public DecoderVisitor {
 public:
  explicit Simulator(Decoder* decoder,
                     FILE* stream = stdout,
                     SimStack::Allocated stack = SimStack().Allocate());
```

Construction takes a `Decoder*` (externally owned), an output `FILE*`
for trace, and a `SimStack::Allocated` for the guest stack
(self-allocated by default, see "Stack" below). The constructor body at
`simulator-aarch64.cc:645-706` does five notable things:

1. Initializes `memory_` with the supplied stack
   (`simulator-aarch64.cc:646`).
2. Creates the `CPUFeaturesAuditor` with the full feature set
   (`cc:648`) and registers the Simulator itself onto the decoder's
   visitor list (`cc:662`).
3. Constructs a `PrintDisassembler` on the supplied stream (`cc:666`)
   and registers the auditor on it (`cc:677`).
4. Resets architectural state via `ResetState()` (`cc:686`); see
   "Register state" below.
5. Seeds the deterministic PRNG with a fixed constant
   (`cc:696-697`) and creates the (disabled) Debugger (`cc:704-705`).

Public execution entry points:

- `void Run()` — the main loop (`simulator-aarch64.h:1298`,
  body at `simulator-aarch64.cc:821-836`). Calls `ExecuteInstruction()`
  until `IsSimulationFinished()` returns true.
- `void RunFrom(const Instruction* first)` — sets PC and calls `Run()`
  (`simulator-aarch64.h:1299`, `cc:840-843`).
- `template<typename R, typename... P> R RunFrom(...)` — ABI-aware
  variant that marshals C++ arguments into guest registers and reads
  the return value back; declared at `simulator-aarch64.h:1313-1355`,
  guarded on `VIXL_HAS_ABI_SUPPORT`.
- `bool IsSimulationFinished() const` — returns `pc_ == kEndOfSimAddress`
  (`simulator-aarch64.h:1362`). `kEndOfSimAddress` is `NULL`
  (`simulator-aarch64.cc:58`); `ResetRegisters()` writes it to LR
  (`cc:720`) so a `ret` from the top-level call sequence terminates the
  loop.
- `void ExecuteInstruction()` — the inline per-instruction step
  (`simulator-aarch64.h:1401-1442`). Covered in detail in the companion
  doc [`vixl-decode-dispatch-pattern.md`](./vixl-decode-dispatch-pattern.md).

## Register state

VIXL stores guest registers in three fixed-size arrays plus a few
specialized types:

```cpp
SimRegister  registers_[kNumberOfRegisters];   // simulator-aarch64.h:5288
SimVRegister vregisters_[kNumberOfVRegisters]; //                    :5291
SimPRegister pregisters_[kNumberOfPRegisters]; //                    :5294
```

`SimRegisterBase<kMaxSizeInBits>` (`simulator-aarch64.h:494`) is the
template that backs all three:

- `SimRegister = SimRegisterBase<kXRegSize>` (64-bit GPR slot).
- `SimPRegister = SimRegisterBase<kPRegMaxSize>` (SVE predicate slot,
  up to 256 bits).
- `SimVRegister` extends the template (`simulator-aarch64.h:625`) and
  carries the SVE-vs-NEON-access bookkeeping.

Storage is a raw `uint8_t data_[kMaxSizeInBytes]` array; reads and
writes use typed templates (`Read<T>()`, `WriteLane<T>()`, etc.). The
per-instance arrays mean each Simulator owns its own register file —
instances do not alias each other.

Register encoding constants live in `src/aarch64/instructions-aarch64.h:103-108`:

```cpp
const unsigned kFpRegCode = 29;
const unsigned kLinkRegCode = 30;
const unsigned kSpRegCode = 31;
const unsigned kZeroRegCode = 31;
const unsigned kSPRegInternalCode = 63;  // distinguishes SP from XZR
```

So `r31` is overloaded as SP or XZR depending on the instruction
context; the simulator disambiguates via `kSPRegInternalCode` when
selecting from the array (`simulator-aarch64.cc:1591-1592`).

System registers — only NZCV and FPCR are tracked in the V1 simulator:

```cpp
SimSystemRegister nzcv_;  // simulator-aarch64.h:5305
SimSystemRegister fpcr_;  //                    :5308
```

`SimSystemRegister` itself (`simulator-aarch64.h:1158-1214`) is a
`uint32_t` value plus a `write_ignore_mask_` so unimplemented bits stay
zero. Default values are obtained via
`SimSystemRegister::DefaultValueFor(SystemRegister)` (h:1191).
Accessors for NZCV/FPCR are at `h:2186, 2204` (`ReadNzcv`, `ReadFpcr`).

PC and BType (Branch Target Identification state):

```cpp
const Instruction* pc_;          // simulator-aarch64.h:5344
bool pc_modified_;               //                    :5343
BType btype_, next_btype_;       // see WriteNextBType / UpdateBType
```

`pc_` is the read pointer for the next instruction; `pc_modified_` is
reset before each `decoder_->Decode(pc_)` call and set by
`WritePc()` so the post-decode `IncrementPc()` (h:1379-1383) skips
auto-advance for branches.

SVE first-fault and predicate-all-true:

```cpp
SimFFRRegister ffr_register_;          // simulator-aarch64.h:5297
SimPRegister   pregister_all_true_;    //                    :5300
```

## Memory model

The `Memory` class lives entirely inside the simulator header
(`simulator-aarch64.h:371-490`). It treats every guest address as a host
pointer — the simulator does not own a translated address space and
does not maintain a TLB. The verbatim implementation of `Memory::Read`
(`simulator-aarch64.h:393-411`) makes this explicit:

```cpp
template <typename T, typename A>
std::optional<T> Read(A address, Instruction const* pc = nullptr) const {
  T value;
  // ... static_assert on size 1/2/4/8/16 ...
  auto base = reinterpret_cast<const char*>(AddressUntag(address));
  if (stack_.IsAccessInGuardRegion(base, sizeof(value))) {
    VIXL_ABORT_WITH_MSG("Attempt to read from stack guard region");
  }
  if (!IsMTETagsMatched(address, pc)) {
    VIXL_ABORT_WITH_MSG("Tag mismatch.");
  }
  if (TryMemoryAccess(reinterpret_cast<uintptr_t>(base), sizeof(value)) ==
      MemoryAccessResult::Failure) {
    return std::nullopt;
  }
  memcpy(&value, base, sizeof(value));
  return value;
}
```

Three things to note:

1. `AddressUntag<T>` (`simulator-aarch64.h:198-203`) strips the top-byte
   address tag (8 bits at offset 56) before any access — guest pointers
   tagged with MTE/PAC bits are dereferenced through the underlying
   address.
2. Stack accesses are bounded by `SimStack::Allocated::IsAccessInGuardRegion`
   (the stack class lives at `simulator-aarch64.h:95-158`, with its
   nested `Allocated` type holding the actual buffer + guard pages).
3. Reads/writes are plain host `memcpy`. There are no barriers; LSE
   atomics and exclusive accesses are *not* implemented through this
   path (they have dedicated visitors that the host simulator handles
   probabilistically — see "Exclusive monitor and atomics" below).

Convenience templates on the Simulator wrap the Memory accessors:

- `MemRead<T>(address)` and `MemWrite<T>(address, value)` —
  `simulator-aarch64.h:2076-2085`.
- `MemReadUint(size_in_bytes, address)`, `MemReadInt(...)`, sized
  variants — `simulator-aarch64.h:2088-2100`.

The Memory class also holds a `MetaDataDepot*` (h:489) for MTE tag
maps and similar metadata; that depot is provided by the Simulator via
`Memory::AppendMetaData` (h:481).

## Branch interception (host callbacks)

Tests and embedders can install host C/C++ functions that fire when
guest code branches to a chosen address. Two declarations matter:

- `using InterceptionCallback = std::function<void(uint64_t)>`
  (`simulator-aarch64.h:209`).
- `template <typename R, typename... P> struct BranchInterception { ... }`
  (`simulator-aarch64.h:310`).

Registration goes through the templated
`RegisterBranchInterception<R, P...>(R (*function)(P...), InterceptionCallback callback = nullptr)`
(`simulator-aarch64.h:3209`), which adds an entry to
`meta_data_.branch_interceptions` (h:365-366), keyed on the host
function address.

When guest code branches to that address, the simulator routes to
`DoRuntimeCall(instr)` (`simulator-aarch64.h:5275`,
`simulator-aarch64.cc:7347`), which uses VIXL's ABI machinery to copy
guest register state into a C++ argument tuple, call the host function,
and copy the return value back. The templated
`DoRuntimeCall<R, P...>` (h:3023-3050) provides the wrappers.

A `RuntimeCallVoid` / `RuntimeCallNonVoid` distinction determines
whether the result is read back (h:3031-3050).

The exit condition for `Run()` is also wired through this mechanism:
`ResetRegisters()` writes `kEndOfSimAddress` (`NULL`) into LR
(`simulator-aarch64.cc:720`), so a `ret` from the top-level guest call
falls into `IsSimulationFinished()` and breaks the loop.

## Exclusive monitor and atomics (upstream behavior)

Two helpers model exclusive accesses:

- `SimExclusiveLocalMonitor` (`simulator-aarch64.h:1217-1259`) — the
  per-core monitor for LDXR/STXR. It records `(address, size)` on
  `MarkExclusive` (h:1240), checks pedantic equality on `IsExclusive`
  (h:1247-1251), and probabilistically clears via an LCG seed in
  `MaybeClear()` (h:1230-1237). The skip probability constant is `8`
  (h:1219).
- `SimExclusiveGlobalMonitor` (`simulator-aarch64.h:1265-1281`) — does
  not actually track addresses; `IsExclusive()` returns true on most
  calls and randomly false (1-in-8) to model contention.

Both are members of `Simulator` (`simulator-aarch64.h:5280-5281`).

The verbatim header for the global monitor explains the design intent
(h:1262-1264):

```cpp
// We can't accurate simulate the global monitor since it depends on external
// influences. Instead, this implementation occasionally causes accesses to
// fail, according to kPassProbability.
```

LSE atomics (CAS / LDADD / LDSET / LDCLR / LDSMAX / LDSMIN / LDUMAX /
LDUMIN / SWP, scalar and pair) are handled by their own visitor
methods inside `simulator-aarch64.cc`, which for the upstream
single-threaded simulator just perform plain loads and stores — there
is no host atomic primitive in the upstream path. Barriers (DMB, DSB,
ISB) are functionally no-ops.

This behavior is correct *only* when the simulator runs single-threaded
on a private memory region. Gaby-VM relaxes that assumption (multiple
instances on shared memory), and the modification sketch
([`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md))
flags both monitor classes and the LSE visitors as the only simulator
code expected to be modified post-import.

## CPUFeaturesAuditor

`src/aarch64/cpu-features-auditor-aarch64.h:51` defines the auditor.
It is itself a `DecoderVisitor` and is created by the Simulator
constructor (`simulator-aarch64.cc:648`):

```cpp
cpu_features_auditor_(decoder, CPUFeatures::All())
```

The constructor wires it into the decoder's visitor list. Per
`Visit(Metadata*, Instruction*)` (`cpu-features-auditor-aarch64.h:107-118`)
it records:

- The set of features the decoded instruction requires.
- Whether the instruction is *available* on the configured CPU profile
  (the second argument to construction).

The Simulator then asserts at the bottom of each
`ExecuteInstruction()` (`simulator-aarch64.h:1441`):

```cpp
VIXL_CHECK(cpu_features_auditor_.InstructionIsAvailable());
```

Failing this assertion aborts execution. Two implications:

- The auditor is *effectively required*. Excluding it from a Gaby-VM
  build either requires removing this assertion or stubbing the
  auditor. The extraction map keeps it.
- The auditor records an accumulating "seen features" set across the
  run, useful for trace and feature reports.

`PrintDisassembler` calls `RegisterCPUFeaturesAuditor` on it
(`simulator-aarch64.cc:677`) so disassembly can annotate instructions
with the features they require.

## Disassembler

`src/aarch64/disasm-aarch64.h:46` defines `Disassembler`; the
streaming subclass `PrintDisassembler` (h:211) is the one the Simulator
uses. The Simulator owns a `PrintDisassembler*` (h:5285) constructed
on its output stream (`simulator-aarch64.cc:666`):

```cpp
print_disasm_ = new PrintDisassembler(stream_);
```

It runs only when trace flags include `LOG_DISASM` (or another
disassembly-driving bit). Outside of trace, `PrintDisassembler` is
attached to the decoder visitor list but its `Visit()` short-circuits.

For Gaby-VM the disassembler is in tier-2: not strictly required for
execution, but easier to import than to remove because the constructor
unconditionally creates one.

## Debugger

`src/aarch64/debugger-aarch64.h:50` defines `Debugger`. The Simulator
holds it via `std::unique_ptr<Debugger> debugger_` (h:5496) and starts
it disabled (`simulator-aarch64.cc:704-705`):

```cpp
SetDebuggerEnabled(false);
debugger_ = std::make_unique<Debugger>(this);
```

The debugger has no platform-specific dependencies beyond the standard
library (no `readline`, no `mmap`); it's an interactive REPL with
breakpoints, single-stepping, and register dumps. Gaby-VM defers it to
tier-3.

## Tracing and runtime-call mechanism

Trace flags live in `simulator-aarch64.cc:15112-15114` and are bitwise:
`LOG_NONE`, `LOG_WRITE`, `LOG_REGS`, `LOG_VREGS`, `LOG_SYSREGS`,
`LOG_BRANCH`, `LOG_DISASM`. They are read and written through
`Get/SetTraceParameters` (`simulator-aarch64.h:2816-2840`). Default
after construction is `LOG_NONE` (`cc:680`).

When trace is on:

- `LogAllWrittenRegisters()` runs after each instruction
  (`simulator-aarch64.h:1438`).
- `PrintDisassembler::Visit()` prints the instruction in mnemonic form
  before the leaf executes (visitor list ordering puts it before the
  Simulator).
- `LogTakenBranch()` fires inside `WritePc()` when `log_mode == LogBranches`
  (`simulator-aarch64.h:1369-1373`).

Runtime-call mechanism — used both by tests' branch interceptions and
by HLT-based syscall-style handlers:

- `DoRuntimeCall(const Instruction* instr)` declares the basic call site
  (`simulator-aarch64.h:5275`). Triggered from the HLT visitor at
  `simulator-aarch64.cc:7347`.
- The templated `DoRuntimeCall<R, P...>` (h:3023-3050) marshals
  arguments via VIXL's `ABI` machinery, calls the host function, and
  writes the return value back into the guest registers.

HLT, BRK, SVC, HVC: HLT routes to `DoRuntimeCall`; BRK invokes the
default breakpoint handler (or the debugger if enabled); SVC and HVC
abort — Gaby-VM does not model an EL1/2 transition in V1.

## Threading and determinism

Each Simulator instance is single-threaded. Per-instance state
includes:

- All register arrays (`registers_`, `vregisters_`, `pregisters_`).
- PC, BType, NZCV, FPCR, exclusive-monitor state.
- The output stream pointer `stream_` (h:5284).
- The deterministic PRNG `rand_gen_`
  (`std::linear_congruential_engine`, h:5476-5480), seeded with a
  fixed constant `(11 + (22 << 16) + (33 << 32))`
  (`simulator-aarch64.cc:696-697`).
- The implicit-checks pipe FDs `placeholder_pipe_fd_[2]` opened on
  POSIX in the constructor (`simulator-aarch64.cc:657`).

There is no static (program-wide) state that would prevent multiple
Simulator instances from coexisting in one process — the only static
tables are the read-only `xreg_names`, `wreg_names`, etc. (h:5361-5369).
However, see [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md)
for the audit Gaby-VM still owes around shared `FILE*` outputs and
`placeholder_pipe_fd_`.

The deterministic PRNG seed means upstream tests reproduce byte-for-byte
across runs. Gaby-VM should preserve this (per-instance seed, no host
entropy).

## Implicit-checks scaffolding (host-arch detail)

`simulator-aarch64.cc:640-643` contains an inline x86_64 `__asm__`
block guarded by `VIXL_ENABLE_IMPLICIT_CHECKS`. It implements
`CanReadMemory` via SIGSEGV-trapping `pipe(2)` writes. iOS implication:
leave `VIXL_ENABLE_IMPLICIT_CHECKS` undefined; the path is host-arch
specific and unrelated to guest execution. Gaby-VM does not need it.

## State diagram (per-instance)

```
+-------------------+
|  Simulator        |
|                   |
|  registers_[32]   |  <-- GPRs r0..r30 + r31(SP/XZR slot)
|  vregisters_[32]  |  <-- v0..v31 / z0..z31
|  pregisters_[16]  |  <-- p0..p15
|  ffr_register_    |  <-- SVE FFR
|                   |
|  pc_, pc_modified_|
|  btype_, next_btype_
|  nzcv_, fpcr_     |
|                   |
|  local_monitor_   |  <-- LDXR/STXR state
|  global_monitor_  |  <-- probabilistic contention
|                   |
|  memory_          |  <-- thin wrapper over host pointers
|     stack_        |  <-- SimStack::Allocated (caller-owned in Gaby-VM)
|     metadata_     |  <-- MTE/PAC depot
|                   |
|  decoder_  ------>  Decoder (external; visitor list owns auditor,
|  cpu_features_auditor_       PrintDisassembler, Simulator)
|  print_disasm_    |
|  debugger_        |  <-- unique_ptr, disabled by default
|                   |
|  rand_gen_        |  <-- LCG, fixed seed
|  trace_parameters_|
+-------------------+
```

## Where to read next

- [`vixl-decode-dispatch-pattern.md`](./vixl-decode-dispatch-pattern.md)
  — the per-instruction control flow (RunFrom → Decoder → visitors →
  leaf).
- [`vixl-extraction-map.md`](./vixl-extraction-map.md) — the file list
  for importing the components above into Gaby-VM.
- [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md)
  — Gaby-VM-specific changes (predecode cache, multi-instance
  concurrency, real atomic semantics, embedder-allocated stacks).
