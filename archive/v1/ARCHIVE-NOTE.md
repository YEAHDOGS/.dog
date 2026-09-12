# Archive: .dog v1 (superseded)

This directory preserves the original v1 design of `.dog` from the old
`YEAHDOGS/dog` repository (single commit `d18c7ad`), consolidated here
when that repo was retired. **YEAHDOGS/.dog is the only canonical repo.**

v1 ("strict syntactic superset of JSON", `!dog 1` magic, `@bare`/`@rle`
directives) was superseded by DOG/2 (`.dog/2.0` magic, generic header
parameters — JSON, YAML, and XML are parameter bundles, no special cases).

Contents:
- `V1-SPEC.md` — the v1 specification (historical; see `/SPEC.md` for DOG/2)
- `V1-README.md` — the v1 readme
- `packages/` — v1 TypeScript reference implementation (`parse`/`stringify`) + tests
- `vectors/` — v1 conformance vectors (`*.dog` ↔ `*.expected.json`)

Nothing here is maintained. Porting the TypeScript implementation to DOG/2
is open work.
