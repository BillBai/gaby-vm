# docs/

This directory holds **cross-cutting prose** for the gaby-vm project — content
that doesn't belong in a single capability spec. Concretely:

- Capability and behavioural documentation lives under `openspec/specs/` (each
  capability gets its own normative spec, kept in lock-step with the change
  artifacts under `openspec/changes/`).
- Build, integration, and quick-start instructions live in the repository
  root [`README.md`](../README.md).
- Anything that doesn't fit either of the above — design rationale, porting
  notes, platform-specific embedding constraints, contributor guidance —
  lives here.

The directory deliberately starts as a single file. Subdirectories
(`architecture/`, `aarch64/`, `platform/`, etc.) get introduced only when
real content arrives that would benefit from grouping. Empty stubs are
anti-value.

## License headers for new files

Every new C/C++/CMake source file authored for gaby-vm carries the BSD
3-clause header below. Vendored files (e.g. anything imported from VIXL)
keep their original headers verbatim — do not rewrite upstream copyrights.

The 2026 year stays as-is for files added in 2026. Files added in later
years update the year in the first line; the rest of the header is
unchanged.

```
// Copyright 2026, the gaby-vm authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the gaby-vm authors nor the names of its
//     contributors may be used to endorse or promote products derived from
//     this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

For shell, Python, or `.cmake` files, replace the leading `//` with `#` on
each line; the rest of the text is identical.
