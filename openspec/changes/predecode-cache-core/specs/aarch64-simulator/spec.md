## REMOVED Requirements

### Requirement: `simulator_correctness` test is registered with CTest using the privileged build pattern

**Reason**: `predecode-cache-core` reworks `test/simulator_correctness.cc`
into a dual-path harness driven entirely through the `gaby_vm::Simulator`
public API (`RunFrom` and `DebugRunFrom`). The reworked test no longer uses
the privileged build pattern — it needs no `PRIVATE ${PROJECT_SOURCE_DIR}/src`
include and no `VIXL_*` compile definitions, because it consumes only
`gaby_vm::` public types. A requirement mandating a build pattern the test no
longer uses is obsolete.

**Migration**: CTest registration of the reworked dual-path test is specified
by the `predecode-cache` capability's *Dual-path correctness and the shadow
oracle are registered with CTest* requirement. The imported simulator's
minimal end-to-end build-and-execute proof remains the `aarch64-simulator`
*Simulator constructs and executes a single NOP* requirement (the
`simulator_smoke` test), which still uses the privileged build pattern.

### Requirement: `simulator_correctness` exercises baseline AArch64 instruction families and asserts on post-`RunFrom` state

**Reason**: The single-path coverage this requirement defined — hand-encoded
sequences driven through `vixl::aarch64::Simulator::RunFrom` only — is
superseded by the dual-path harness `predecode-cache-core` introduces. The
imported `Decoder → VisitNamedInstruction → leaf` flow this requirement
verified is now exercised as the `DebugRunFrom` leg of that harness, alongside
the cache-track `RunFrom` leg.

**Migration**: Baseline-family coverage (integer arithmetic, logical,
load/store, conditional control flow, procedure call/return) and the
assert-on-post-run-state contract are carried forward by the `predecode-cache`
capability's *Dual-path correctness and the shadow oracle are registered with
CTest* requirement, which mandates the same families across both execution
tracks. No coverage is lost — it moves to the capability that owns the cache
the test now exercises.
