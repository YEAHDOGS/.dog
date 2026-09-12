# dog — the `.dog` text format

`.dog` is a strict syntactic superset of JSON with an optional declared header.
A file with no header **is** JSON. A file starting with `!dog 1` can declare
directives (`@bare`, `@comment`, `@trail`, `@esc`, `@rle`) that let the body drop
bytes — barewords instead of quoted strings, and header-declared run-length
encoding for repeated characters — without the parser ever guessing.

```dog
!dog 1
@bare
@rle ~ 4

{row:00000000000000001~16}
```

That body is 26 bytes; the equivalent JSON is 42. The header is declared once and
amortizes over the file — which is the point: `.dog` wins on **run-heavy machine
text** (telemetry, bitmap-ish rows, padded dumps, logs), not on prose.

**The one rule that matters:** every valid JSON document is valid `.dog` with
identical semantics. No conversion step, no drift, no stranded files.

## Layout

- [`SPEC.md`](SPEC.md) — the normative v1 specification (start here).
- [`packages/dog/`](packages/dog/) — TypeScript reference implementation,
  zero dependencies: `parse()` / `stringify()` plus the test suite.
- [`vectors/`](vectors/) — paired conformance vectors (`*.dog` ↔ `*.expected.json`).

## Quick start

No install, no build — it's one file with zero dependencies. Node 22.18+ runs it
directly via type stripping; it also drops straight into a Svelte/TS frontend.

```ts
import { parse, stringify } from "./packages/dog/dog.ts";

// No header? It's just JSON.
parse('{"a": [1, 2]}'); // => { a: [1, 2] }

// Header? Declared relaxations, still JSON-compatible.
parse("!dog 1\n@bare\n\n{a: hello}"); // => { a: "hello" }

stringify({ row: "0".repeat(16) + "1".repeat(16) }, {
  bare: true,
  rle: { marker: "~", minrun: 4 },
});
// => '!dog 1\n@bare\n@rle ~ 4\n\n{row:00000000000000001~16}'
```

Run the tests:

```sh
cd packages/dog && node --test test/dog.test.ts
```

## Design constraints

- **Superset, not fork:** JSON ⊆ .dog, literally. RLE and escapes never apply
  inside double-quoted strings, so quoted content stays byte-identical JSON.
- **Declared, never guessed:** unknown directives and unknown major versions are
  rejected. Silent dialect drift is how "human JSON" variants rot.
- **Fail-closed RLE:** the encoder never emits an ambiguous run (no RLE before a
  digit, no RLE of the marker itself, marker doubling for literals), the decoder
  caps run lengths against decompression bombs.

## License

MIT — see [LICENSE](LICENSE).

---

This product was made by DOGS — https://wearedogs.net
