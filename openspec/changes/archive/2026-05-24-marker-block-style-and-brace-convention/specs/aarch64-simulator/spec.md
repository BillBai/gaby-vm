## MODIFIED Requirements

### Requirement: Imported files are byte-identical to upstream except at marked locations

Each imported file's content SHALL match its upstream counterpart in `../vixl/` byte-for-byte, except inside regions that are bracketed by Gaby-VM marker comments. Marker comments are the *only* legitimate source of drift from upstream.

#### Scenario: Unmodified file matches upstream byte-for-byte
- **WHEN** an imported file containing no `gaby-vm` marker is compared (`diff`) against its upstream counterpart
- **THEN** the diff is empty

#### Scenario: Modified file matches upstream outside marker regions
- **WHEN** an imported file containing one or more `gaby-vm` markers is compared against upstream
- **THEN** every differing line is one of: a marker comment line (a `// gaby-vm:`, `// gaby-vm BEGIN:`, or `// gaby-vm END` token line, or one of the ordinary `//` reason lines that immediately follow a `// gaby-vm:` or `// gaby-vm BEGIN:` token line); a line that lies between a `// gaby-vm BEGIN:` / `// gaby-vm END` pair; or the single changed line immediately below a `// gaby-vm:` marker block

### Requirement: All edits to imported files use the documented marker convention

Any deviation from upstream content inside an imported file SHALL be bracketed by a comment marker in one of two forms. In both forms the marker *token* SHALL occupy its own line, and the reason text SHALL follow on the next line(s) as ordinary `//` comments:

- Single-line edit: a `// gaby-vm:` token line, followed by one or more ordinary `//` reason lines, placed immediately above the single modified line.
- Multi-line edit (including deletions): a `// gaby-vm BEGIN:` token line followed by one or more ordinary `//` reason lines above the changed region, and a `// gaby-vm END` line below it. Removed code is left commented out within the block so the deletion is reviewable.

The marker token `gaby-vm` SHALL be lowercase. A token line SHALL carry no reason text after the token. A `// gaby-vm:` or `// gaby-vm BEGIN:` marker SHALL have at least one reason line. Reason lines SHALL NOT contain the literal sequences `gaby-vm:`, `gaby-vm BEGIN`, or `gaby-vm END`, so that they are not picked up by the enumeration grep. The command `git grep -nE 'gaby-vm( BEGIN| END|:)' src/` SHALL enumerate every modified location.

#### Scenario: Single-line edit is preceded by a single-line marker
- **WHEN** a single line of an imported file differs from upstream
- **THEN** the lines immediately above it are a `// gaby-vm:` token line carrying no inline reason, followed by one or more ordinary `//` reason lines

#### Scenario: Multi-line edit is bracketed by BEGIN/END markers
- **WHEN** two or more contiguous lines of an imported file differ from upstream
- **THEN** the region is preceded by a `// gaby-vm BEGIN:` token line carrying no inline reason, followed by one or more ordinary `//` reason lines, and the line immediately below the region is `// gaby-vm END`

#### Scenario: Marker token line carries no inline reason
- **WHEN** any `// gaby-vm:`, `// gaby-vm BEGIN:`, or `// gaby-vm END` token line in an imported file is inspected
- **THEN** the line contains only the marker token plus leading indentation, with the reason text (if any) on the following ordinary `//` lines

#### Scenario: Marker grep enumerates every drifted location
- **WHEN** running `git grep -nE 'gaby-vm( BEGIN| END|:)' src/`
- **THEN** the output includes at least one match for every region that differs from upstream, and ordinary `//` reason lines (which carry no marker token) are not matched
