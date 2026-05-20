## MODIFIED Requirements

### Requirement: Imported simulator code is preserved structurally

The imported decode → visitor → leaf execution flow SHALL be preserved at the
leaf-semantic and namespace level. The marker convention (see the
*All edits to imported files use the documented marker convention* requirement)
remains the only legitimate source of drift from upstream.

Specifically:

- Class declarations for `Simulator`, `Decoder`, `Instruction`, and the rest
  of the imported subsystem SHALL retain their upstream member variables,
  constructor signatures, and method declarations. Removing or renaming an
  existing member is NOT permitted.
- Structural additions (new methods, fields, helper classes) on imported
  types ARE permitted when they follow the marker convention AND when each
  marker reason names the design document that motivates the addition.
- The leaf `VisitXxx` and `Simulate_*` member functions, plus the
  `LogicAArch64` helpers they call, SHALL preserve upstream semantics. Any
  marker-bracketed deviation in a leaf body SHALL document why the deviation
  cannot be expressed at a higher layer.
- The imported `Decoder → VisitNamedInstruction → leaf` dispatch flow SHALL
  remain reachable from the simulator. A non-cached execution path (e.g.,
  for tracing, debugging, shadow self-test, or bring-up) SHALL be available
  that exercises this flow.
- The `vixl::aarch64` namespace SHALL be preserved (no rename to
  `gaby_vm::aarch64` or other namespace).
- Alternate dispatch paths (e.g., a predecode cache) MAY be introduced
  alongside the imported flow, provided each addition is marker-bracketed and
  references a design document.

#### Scenario: Imported `Simulator` class declaration matches upstream

- **WHEN** `src/aarch64/simulator-aarch64.h`'s `Simulator` class declaration is compared with upstream
- **THEN** the public, protected, and private member variables and method signatures match (additions are permitted only inside marker-commented regions; removals are not permitted)

#### Scenario: `vixl::aarch64` namespace is unchanged

- **WHEN** `git grep -n 'namespace vixl' src/` is run
- **THEN** the imported files declare and use the `vixl` and `vixl::aarch64` namespaces (no renames to `gaby_vm` or other namespace introduced)

#### Scenario: Imported dispatch flow remains reachable

- **WHEN** any non-cached execution path through the simulator is exercised (e.g., a `DebugRunFrom` entry, a tracing loop, or any path that calls `Decoder::Decode`)
- **THEN** the path goes through the imported `Decoder → VisitNamedInstruction → leaf` flow
- **AND** alternative dispatch paths (predecode cache, etc.) MAY exist alongside, each introduced with marker comments whose reason text references a design document under `docs/refs/`

#### Scenario: Marker reason cites a design document for additive structural changes

- **WHEN** an imported file contains a `// gaby-vm:` or `// gaby-vm BEGIN` marker that introduces a new method, field, or helper class
- **THEN** the marker's reason text contains a path or filename matching `docs/refs/<doc>.md` (or an equivalent in-tree design document)
