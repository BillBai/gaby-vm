## 1. Author the spec delta

- [ ] 1.1 Write `specs/aarch64-simulator/spec.md` under this change directory,
      using the `## MODIFIED Requirements` heading. The file SHALL contain the
      full, post-change body of the *Imported simulator code is preserved
      structurally* requirement (including its scenarios), so `openspec archive`
      replaces the live spec's text wholesale.
- [ ] 1.2 Verify the modified-requirement body matches the four design
      decisions in `design.md` (D1–D5): leaf-semantics preservation explicit,
      `SHALL remain reachable` instead of `invoked unchanged`, no-cache bullet
      removed, additive-permission bullet added, scenarios updated.

## 2. Validate before archive

- [ ] 2.1 Run `openspec validate relax-vixl-import-boundary --strict` from the
      repo root. Confirm `valid: true`.
- [ ] 2.2 Read the in-flight change as a whole (proposal + design + delta) one
      more time and check for inconsistencies (a decision in `design.md` not
      reflected in the spec delta is the most likely failure mode).

## 3. Apply and archive

- [ ] 3.1 Run `openspec archive relax-vixl-import-boundary --yes` to fold the
      delta into `openspec/specs/aarch64-simulator/spec.md` and move the change
      directory under `openspec/changes/archive/<date>-relax-vixl-import-boundary/`.
- [ ] 3.2 Confirm `git status` shows: live spec modified, change directory moved
      into archive, no other paths touched.
- [ ] 3.3 Re-read the live spec around the modified requirement; confirm the
      surrounding requirements (byte-identical, marker convention, license,
      etc.) are unchanged.

## 4. Cross-references

- [ ] 4.1 The predecode cache design doc
      (`docs/refs/gaby-vm-predecode-cache-design.md` §2.3 / §6.5) declares it
      depends on this relaxation. After archive, that dependency is satisfied;
      no edit to the design doc is required by this change.
