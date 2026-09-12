# The `.dog` Text Format Specification — Version 1

**Status:** v1 normative. Reference implementation: `packages/dog/dog.ts` (TypeScript,
zero dependencies). Conformance vectors: `vectors/`.

## 1. Overview

`.dog` is a **strict syntactic superset of JSON with an optional declared header**.

- A file with **no header is just JSON** — parsed with JSON semantics, byte for byte.
  This is the on-ramp: rename `.json` → `.dog`, keep every existing tool, add a
  header later only where the savings pay for it.
- A file whose first line is `!dog 1` declares **encoding relaxations and
  run-length rules up front**, so the body can drop bytes without the parser guessing.
  Directives are declared once, paid once.

Design principles:

1. **JSON ⊆ .dog, literally.** Every valid JSON document is a valid `.dog` document
   with identical semantics. Every JSON test vector in the world is a `.dog`
   conformance test for free, and no `.dog` file is ever stranded: strip the header
   extensions and it is still JSON.
2. **Declared, never guessed.** The parser never infers an encoding from the payload.
   Unknown directives and unknown major versions are rejected — fail closed, never
   silently misparse.
3. **Minimal > pretty, but stays human-readable.** `.dog` is positioned for
   **run-heavy machine text** (telemetry, pixel/bitmap-ish rows, padded fixed-width
   dumps, logs), not as a general JSON replacement. On prose and config payloads the
   header is pure overhead — that is why every extension is opt-in per file.
4. **RLE never touches quoted strings.** Double-quoted strings stay byte-identical
   JSON. That is what keeps the JSON-subset promise airtight.

## 2. File structure

```
dog-file   := header? body
header     := magic-line directive* blank-line?
magic-line := "!dog" SP major-version        ; e.g. "!dog 1"
directive  := "@" name (SP args)? 
body       := json-value                      ; JSON grammar, plus §4 extensions
```

- The header is present **iff** the file's first line matches `!dog <N>` exactly
  (a trailing `\r` is tolerated for CRLF files; a leading BOM is tolerated).
  Otherwise the whole file is parsed as JSON. This is unambiguous: `!` is never a
  valid start of a JSON document.
- After the magic line, every line starting with `@` is a directive; the body starts
  at the first subsequent line that does not start with `@` (blank lines between are
  ignored). `@` can never start a body value — not in JSON, and not in any declared
  extension — so the split is unambiguous.
- Parsers MUST reject unknown `@` directives and unknown major versions.
- An empty body under a header is an error.

## 3. Header directives (v1)

| Directive  | Args              | Meaning |
|------------|-------------------|---------|
| `@bare`    | —                 | Allow bareword scalars/keys and single-quoted strings (§4). Without it the body is strict JSON. |
| `@comment` | `[<char>]`        | Allow line comments starting with `<char>` (default `#`). Without it, no comments. |
| `@trail`   | —                 | Allow trailing commas in objects and arrays. |
| `@esc`     | `[<char>]`        | Escape char for bare scalars (default `~`). Doubled = literal. |
| `@rle`     | `<marker> <minrun>` | Enable run-length runs: `<ch><marker><N>` in bare scalars expands to `<ch>` × N. |

Rules:

- `@rle`'s marker and `@esc`'s char are the file's **special char** for bare scalars.
  If both are declared they MUST be identical, otherwise the file is rejected —
  two different special chars would be silently ambiguous.
- All declared special chars (comment char, esc/marker) MUST be pairwise distinct
  and MUST NOT be: whitespace, a bare-charset character (§4), or one of
  ``{}[]",:'@``. (E.g. `@comment @` is rejected — `@` starts directives.)
- `@rle`'s `minrun` MUST be a positive integer. It guides the *encoder*: runs
  shorter than `minrun` stay literal. The *decoder* expands any well-formed run.
- Directives apply file-wide. A repeated directive: last one wins.

## 4. Body grammar

The body is JSON, plus these extensions — each active **only when declared**:

| Feature | JSON | `.dog` (declared) | Example |
|---|---|---|---|
| Bare keys / scalars | `{"a":"b"}` | `@bare` → `{a:b}` | saves 4 quotes per pair |
| Single-quoted strings | — | `@bare` → `{'it\'s'}` | avoids `\"` storms |
| Line comments | — | `@comment` → `# note` | dropped on round-trip |
| Trailing commas | — | `@trail` | `{a:1,}` |
| RLE runs | — | `@rle ~ 4` → `0~20` | 20 zeros in 4 chars |
| Numbers, nesting, `true`/`false`/`null` | as-is | as-is | JSON-identical |

**Bareword charset:** `[A-Za-z0-9_][A-Za-z0-9_.:/+-]*` — anything that cannot be
confused with structural punctuation or a JSON number. Two refinements the charset
alone does not capture:

- **Bare keys MUST NOT contain `:`.** The key scanner stops at `:` so the key
  separator is never swallowed. A key containing `:` (or anything outside the
  charset) MUST be quoted: `{"a:b": 1}`.
- **A bare token that lexes as a JSON number, `true`, `false`, or `null` IS that
  value** (`{n:123}` has a numeric value). Consequently, `stringify` MUST quote any
  *string* that would lex that way (`"123"`, `"true"`, `""`). What looks like a
  number parses as a number — no exceptions, no guessing.

**Single-quoted strings** (under `@bare`) support the JSON escape set plus `\'`.
They are exactly equivalent to double-quoted strings.

**Comments** (under `@comment`) run from the comment char to end of line and are
dropped by the parser — round-trip preservation is NOT guaranteed.

**Duplicate keys:** last wins, exactly like `JSON.parse`. Not an error, not merged.

**Numbers:** JSON number grammar, unchanged. No hex, no `NaN`/`Infinity` in v1.

**Unicode:** UTF-8, no BOM requirement. `\uXXXX` works in quoted strings per JSON.

**Binary data:** NOT a binary format — base64/hex *strings*, same as JSON.

## 5. The bare-scalar codec (RLE + escapes)

This is the header's repeated-character-avoidance machinery, and the one genuinely
new trick in `.dog`: the classic RLE ambiguity — *how does the decoder tell a count
from a literal?* — is solved by declaration. In a file with `@rle ~ 4`, `0~20` is
unambiguous *in that file*.

Let `M` be the file's special char (`@rle` marker, else `@esc` char).

**Decoder** (scans a bare token left to right):

1. `MM` → literal `M`.
2. `<ch>M<digits>` → `<ch>` repeated N times (**only when `@rle` is declared**;
   with `@esc` alone there is no rule 2). Decoded run lengths are capped
   (reference implementation: 10,000,000) as a decompression-bomb guard.
3. `M` in any other position → literal `M`.
4. A bare token containing `M` is **always a string**, never a number.

**Encoder** MUST obey these rules (they are what make decoding unambiguous):

1. **RLE NEVER applies inside double-quoted strings.** Quoted strings are
   byte-identical JSON, always.
2. **Never RLE a run whose next character is a digit.** `0~161~16` is ambiguous
   (161 zeros? 16 zeros then sixteen ones?), so the encoder leaves such runs
   literal instead. Hand authors: write the encoder's form.
3. **Never RLE runs of `M` itself** — emit doubled `M`s instead (rule: a run of
   eight `~` encodes as sixteen `~`, decoding back to eight).
4. **Double `M` when followed by `M` or a digit**, otherwise emit it literally.
   (Literal `a~1b` encodes as `a~~1b`; literal `a~b` stays `a~b`.)
5. Only runs with length ≥ `minrun` are encoded; the decoder does not care about
   `minrun` at all.

Because of rule 2, the canonical encoding of sixteen `0`s followed by sixteen `1`s
is `00000000000000001~16` — sixteen literal zeros, then `1~16`. It is slightly less
compact than the hand-written `0~16 1~16`, but it needs no whitespace hacks and no
adjacent-scalar concatenation semantics (which `.dog` deliberately does NOT have:
`{row:0~16 1~16}` is two adjacent scalars and is **rejected** — silently gluing
them would mask missing commas).

## 6. Worked examples (byte counts measured from the reference implementation)

**Ex. 1 — simple object.** JSON `{"name":"sam","role":"dev"}` is 27 bytes.
```dog
!dog 1
@bare

{name:sam,role:dev}
```
Body 19 bytes; the 14-byte header amortizes over the file.

**Ex. 2 — nested, with comment + trailing comma.**
```dog
!dog 1
@bare
@comment
@trail

{
  # applicant record
  applicant: {
    handle: sampleuser,
    platform: tiktok,
    tags: [freestyle, icecream,],
  },
}
```

**Ex. 3 — array-heavy telemetry (repeated keys = the real-world case).**
JSON `[{"x":0,"y":0},{"x":1,"y":0},{"x":2,"y":0}]` is 43 bytes; the `@bare` body
`[{x:0,y:0},{x:1,y:0},{x:2,y:0}]` is 31.

**Ex. 4 — repeated-character payload (the header paying for itself).**
JSON `{"row":"00000000000000001111111111111111"}` is 42 bytes.
```dog
!dog 1
@bare
@rle ~ 4

{row:00000000000000001~16}
```
Body 26 bytes. One row does not beat the 23-byte header — RLE is a win for
*run-heavy* payloads (ten such rows: file well under the JSON size), dead weight
for prose. That is exactly why it is opt-in per file.

**Ex. 5 — JSON passthrough (no header = JSON, zero changes).**
```json
{"already":"json","stays":[1,2,{"valid":true}]}
```
Parses as `.dog` untouched. The whole migration story: rename `.json` → `.dog`,
add a header later when the savings are worth it.

## 7. Conformance

`vectors/` holds paired test vectors: `NNN-name.json`, `NNN-name.dog`, and
`NNN-name.expected.json`. Conformance = `parse(NNN-name.dog)` deep-equals
`JSON.parse(NNN-name.expected.json)`. The reference implementation's suite
(`packages/dog/test/`) runs these plus unit tests for every rule in §3–§5,
including: unknown-directive/version rejection, every-JSON-is-valid-`.dog`
round-trips, RLE-never-in-quoted-strings, the digit-follow guard, marker doubling,
run-length caps, and the no-concatenation rule.

## 8. Non-goals and edge cases (v1)

- **Streaming:** v1 is whole-document parse. No length-prefix framing.
- **Canonical form:** none in v1 (key order preserved as-written, not significant).
- **Schema/types beyond JSON:** no dates, no bigints, no custom scalars
  (`@types` is the v2 hook).
- **Repeated object shapes:** the other redundancy class (array-of-objects key
  repetition) is a v2 `@dict`/`@template` candidate, not v1.
- **Comments** do not survive round-trips (§4).
- Nesting depth is capped by implementations (reference: 10,000) as a
  stack-exhaustion guard.

## 9. Versioning

The major version rides in the magic line: `!dog 1`. Parsers MUST reject unknown
major versions. Minor extensions MUST be new `@` directives (unknown ones already
fail closed), never silent grammar changes — v1 documents stay parseable forever.

## 10. Security notes for implementers

- Fail closed on unknown directives/versions; never guess an encoding.
- Cap decoded run lengths (decompression bombs) and nesting depth.
- Special chars are validated against the reserved set (§3) — a marker that
  collides with the bare charset or structural punctuation is rejected, not
  worked around.
- `.dog` is a data format, not a code format: no includes, no imports, no
  executable content.
