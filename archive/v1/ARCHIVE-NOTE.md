# Archive: earlier .dog draft (never shipped)

This directory preserves the earlier draft design of `.dog` from the old
`YEAHDOGS/dog` repository (single commit `d18c7ad`), consolidated here
when that repo was retired. **YEAHDOGS/.dog is the only canonical repo.**

The draft ("strict syntactic superset of JSON", `!dog 1` magic,
`@bare`/`@rle` directives) never shipped. The current spec (`/SPEC.md`,
magic `.dog/1.0`) is v1 — the first and only version. Greenfield: nothing
is set in stone.

Contents:
- `V1-SPEC.md` — the draft specification (historical; see `/SPEC.md` for v1)
- `V1-README.md` — the draft readme
- `packages/` — draft TypeScript reference implementation (`parse`/`stringify`) + tests
- `vectors/` — draft conformance vectors (`*.dog` ↔ `*.expected.json`)

Nothing here is maintained. A TypeScript implementation of the v1 spec is
open work.
