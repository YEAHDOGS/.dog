# Dot Dog Bag Log

The running log for the .dog text format project. Newest entries first.
Ideas go here before they become spec, spec changes, or code.

---

## The header is one line (2026-09-12)

Founder's correction, verbatim: "The header should only ever be in one
line." Directives were one-per-line with a blank line ending the header;
now the header is always exactly line 1: `.dog/1.0` followed by
space-separated `name:value` parameters. A reader never guesses where the
header ends.

- Values containing spaces are double-quoted: `keys:"n e r a"`,
  `dict:"n=name e=email r=role a=active"`. Backslash escapes inside quotes.
- One optional blank line after the header is purely visual; the body is
  every line after line 1 either way.
- Header comment lines are gone (there is only one header line now);
  `#` full-line comments remain a body-syntax feature.
- `quote: "` becomes `quote:"\""` — a bare `"` would open a quoted span.
- All six parsers (py/js/c/go/rs/kt), the corpus, the examples, SPEC.md,
  and README.md ported. Native `minimal.dog` dropped 391 → 297 bytes:
  57% smaller than pretty JSON, 42% smaller than minified.

---

## DOG/2: `lang:` is dead — the header IS the syntax (2026-09-11)

Founder's order, verbatim principle: the `lang=json` / `lang=yaml`
shortcut was "lazy bullshit." No special cases, no escape hatches.

What changed:
- **No `lang:` directive.** The body's syntax is fully described by
  generic header parameters. Magic bumped to `.dog/2.0` (breaking change;
  v1 files need the mechanical migration in SPEC.md §7).
- **New syntax parameters** (§2): `map-open:`/`map-close:`,
  `seq-open:`/`seq-close:`, `key-sep:` (default `:`), `entry-sep:`
  (default newline), `seq-item:` (default `- `), `quote:` (default `"`),
  `quote2:` (unset), `escape:` (default `\`), `bare:` (`strings` default |
  `none`).
- **JSON = 7-line parameter bundle** (§3). Any JSON file + that header,
  zero body bytes changed, = valid .dog. Verified byte-identical against
  `examples/minimal.json`.
- **YAML block subset = 3-line bundle.** Indent mode is the default; no
  open tokens needed.
- **Semantics are invariant across configurations** (R2.1 holds
  everywhere): bare `NO` is the string `"NO"` even in YAML syntax.
  This closes the loop on R3.4 — presets are now genuinely macros over
  declared parameters, documented in SPEC.md, and the shortcut is gone.
- `dict:`/`rep:` now apply in every configuration, not just native.
  `keys:` rows require newline `entry-sep` (else document error).
- `examples/user.dog`, `user-json.dog`, `user-yaml.dog`: same data, three
  configs, verified to parse to the identical value.
- Old examples migrated to DOG/2 (`minimal.dog`, `nested.dog`,
  `json-variation.dog`, `yaml-variation.dog`).

Open questions for the founder:
1. **XML** — can't be a delimiter bundle (attributes/`@attr`/`#text` are
   data-model mappings, not syntax). Drop it from the spec, or keep as a
   documented mapping outside the parameter system? Recommendation: drop
   (R3.2, no sprawl).
2. **Parser fleet** — all 6 v1 parsers (py/js/go/rs/kt/c) + 39-case corpus
   are DOG/1. Porting to DOG/2 is queued; corpus needs v2 vectors.
3. **YEAHDOGS/dog repo** has its own older v1 spec ("strict syntactic
   superset of JSON") that conflicts with this design. Needs a ruling on
   which repo is canonical.

---

## Parser fleet — matrix

Goal: a conformance-passing parser in every major language, on every major
platform. **Conformance corpus first** (`tests/corpus/`): .dog inputs paired
with expected JSON outputs. A parser isn't "done" until it passes 100% of
the corpus — this is how we keep implementations from diverging.

| Language | Status | Target platforms | Notes |
|---|---|---|---|
| Python | done (reference) | Linux/macOS/Windows | `dog.py`, zero-dep, 16 tests green |
| JavaScript/TypeScript | next | node, browser | Castle apps are Svelte/TS — highest leverage |
| Go | done (2026-09-11) | Linux/macOS/Windows, servers | `dog.go`, single-file zero-dep (`package main`, stdlib only), CLI mirrors dog.py; Go 1.27.1 toolchain (~/toolchains/go); `go test -run TestCorpus` → 39/39 green incl. truncated-directive hard errors + dog.py agreement on 37 |
| Rust | done (2026-09-11) | everywhere, incl. embedded | `dog.rs`, single-file zero-dep (stdlib only), `parse()` library API + `dog.py`-style CLI (`rustc --edition 2021 -O dog.rs`); conformance runner `tests/run_corpus.rs` (`rustc --test`, includes `../dog.rs` as a module) → **39/39 green** incl. truncated-directive hard errors + dog.py agreement on 37; rejects `l:`/`di:` like dog.js/dog.go (D1 still pending his call); rustc 1.98.1, zero warnings |
| Kotlin | done (2026-09-11) | Android (JVM) | `dog.kt`, single-file zero-dep (stdlib only), `parse()` library API + `dog.py`-style CLI (`kotlinc dog.kt -include-runtime -d dog.jar && java -jar dog.jar file.dog`); conformance runner `tests/run_corpus.kt` (kotlinc, `java -cp /tmp/dog_kt.jar Run_corpusKt`) → **39/39 green** incl. truncated-directive hard errors + dog.py agreement on 37; rejects `l:`/`di:` like dog.js/dog.go/dog.rs (D1 still pending his call); kotlinc 2.4.20 on OpenJDK 21, zero warnings |
| Swift | queued | iOS | |
| Java | queued | Android, servers | |
| C# | queued | Windows, .NET | Phoenix tooling is PowerShell/.NET-adjacent |
| C | done (2026-09-11) | embedded, Castle OS | `dog.c`, single-file zero-dep (stdlib only: stdio/stdlib/string/ctype), `dog_parse()` library API + `dog.py`-style CLI (`gcc dog.c -o dog && ./dog file.dog`, dog.py-style pretty JSON); conformance runner `tests/run-corpus.sh` (bash; compiles `-Wall -Wextra`, zero warnings tolerated, per-case vs `.expected.json`/`.error` + dog.py agreement) → **39/39 green** incl. truncated-directive hard errors + dog.py agreement on 37; rejects `l:`/`di:` like dog.js/dog.go/dog.rs/dog.kt (D1 still pending his call); gcc 13.3.0, ASan/UBSan clean over the full corpus |
| Dart, Ruby, PHP | queued | | on demand |

Proposed build order: JS/TS → Go → Rust → Kotlin → the rest, unless the
founder reorders.

## Syntax honing — open decisions

- **D1 — Abbreviated directives.** Proposal: the canonical full word is
  always valid (`lang:`); any *unique* prefix is also valid (`l:`).
  Ambiguous prefix = hard error naming the candidates. Abbreviations
  resolve against the spec version's directive set (so a new directive can
  never silently steal an old abbreviation — it becomes an error instead).
  Docs and examples SHOULD use full words; abbreviations exist for
  byte-shaving. ⚠️ This amends RULES.md R3.1 (one spelling per thing) —
  needs explicit founder sign-off before it enters the spec.
- **D2 — Leniency principle.** Proposed governing line: **generous in
  expressive power, strict in interpretation.** Writers get many tools
  (presets, parameters, abbreviations); parsers accept exactly one
  interpretation per document, no guessing, no heuristics. "Be liberal in
  what you accept" is the philosophy that corrupted YAML data (Norway
  problem) — .dog does not do that. Flexibility lives in what you can
  *write*, not in what the parser *tolerates*.
- **D3 — Directive set growth.** New directives (`nest:`, `escape:`, …)
  need a uniqueness audit against all existing prefixes before acceptance.

## Integration — adoption playbook (research landed 2026-09-11)

**Headline finding:** every format that won was carried by a flagship
distribution vector, not spec quality. Standards and RFCs arrived *after*
victory to describe reality (JSON: ECMA-404/RFC 8259 years later; YAML's
`application/yaml` MIME type came 23 years late) — never to cause it.

**Vectors, ranked by leverage:**
1. Dogfood inside a flagship product — highest leverage, by far.
2. Single-file zero-dependency parsers, one per language.
3. Package-manager distribution (npm, PyPI, crates.io, Go modules).
4. One-page spec site + online playground.
5. CLI converters/validators (bidirectional native↔JSON — the downgrade
   path; JSON5's lesson: "almost, but not quite, JSON" without a downgrade
   path fragments tooling).
6. MIME type + file extension registration — register day one, expect zero
   lift from it.
7. Language stdlib inclusion — lagging indicator, high politics cost.
8. Editor support — hygiene only.

**Sequenced playbook:**
- **Week 1:** zero-friction bundle — zero-dep parsers on npm/PyPI/
  crates.io/Go modules + one-page spec + playground + bidirectional CLI.
- **Month 1:** freeze the grammar; ship a y_/n_/i_ conformance corpus
  pinned by commit (copy JSONTestSuite/toml-test discipline: missing
  corpus = test failure).
- **Months 2–6:** win ONE flagship — write the integration PR yourself
  (the Cargo/MessagePack pattern).
- **Ongoing:** tooling ring — VS Code grammar, validator/linter,
  flawless converter.
- **Late:** MIME paperwork, ABNF, stdlib bids — documentation, not strategy.

**Single highest-leverage first move:** ship the format inside one product
the target users already run, with drop-in parsers on every package manager
day one. → For us, that product is **Castle**: .dog as Castle's config/data
format, Chains metadata envelopes. We own the flagship.

**Graveyard lessons:** nothing died of bad syntax. They died of no flagship
(SDLang), ecosystem coupling (RON→Rust-only, HOCON→JVM-only), dead spec
presence, or near-compatibility without an interop story.

**Conformance discipline to copy:** format-owned language-agnostic corpus
with accept/reject/undefined verdicts; pin corpus by commit; assert the
corpus census in CI; known deviations pinned with written reasons shared
across runtimes. Divergence costs are real (y's two engines disagree on
29 cases).

**Open/unverified from the research:** no substantive sources found for
JSONH or Amazon Ion; Chromium's JSON5 use unconfirmed. Not load-bearing
for the playbook.

---

## Log

- **2026-09-11 ~06:00** — Parser fleet step 6 done: `dog.c` (single-file,
  zero-dep, stdlib only — `dog_parse(text, &err)` library API plus a
  `dog.py`-style CLI: `gcc -Wall -Wextra -O2 -std=c11 dog.c -o dog &&
  ./dog file.dog`, prints dog.py-style pretty JSON). Mirrors
  dog.js/dog.go/dog.rs/dog.kt behavior, including the hard rule: REJECTS
  truncated directive names (`l:`, `di:`) as hard errors — does NOT copy
  dog.py's silent leniency (pending his D1 ruling). Conformance runner
  `tests/run-corpus.sh` (bash; compiles with `-Wall -Wextra` and fails on
  any warning; per-case check vs `.expected.json`/`.error` plus a python3
  dog.py-agreement driver) → **39/39 green** — every corpus case, plus
  dog.py agreement on all 37 non-deviation cases (the 2 truncated-directive
  cases keep their documented `.known-deviation` vs dog.py). dog.py's own
  16-test suite still passes untouched; node `tests/run-corpus.js` still
  39/39. C CLI output verified semantically equal to dog.py on all five
  `examples/*.dog`. Extra hardening: ASan+UBSan build run over the full
  corpus — zero errors. Implementation notes: integers canonicalized as
  literal text (arbitrary precision preserved, `007` → `7`, `-0` → `0`);
  floats use shortest-round-trip Python-repr-style formatting (`1000.0`,
  `-0.0015`); hand-written JSON and XML parsers, no deps.
- **2026-09-11 ~06:10** — Parser fleet step 5 done: `dog.kt` (single-file,
  zero-dep, stdlib only — `fun parse(text: String): Any?` library API plus a
  `dog.py`-style CLI: `kotlinc dog.kt -include-runtime -d dog.jar &&
  java -jar dog.jar file.dog`). Mirrors dog.js/dog.go/dog.rs behavior,
  including the hard rule: REJECTS truncated directive names (`l:`, `di:`)
  as hard errors — does NOT copy dog.py's silent leniency (pending his D1
  ruling). Conformance runner `tests/run_corpus.kt` (compiled with kotlinc,
  run as `java -cp /tmp/dog_kt.jar Run_corpusKt`) → **39/39 green** — every
  corpus case vs `.expected.json`/`.error`, plus dog.py agreement on all 37
  non-deviation cases (the 2 truncated-directive cases keep their documented
  `.known-deviation` vs dog.py). dog.py's own 16-test suite and the node
  corpus runner still pass untouched. Toolchain note: OpenJDK 21 was missing
  from ~/toolchains (only kotlinc was there); installed
  `openjdk-21-jre-headless` from the Ubuntu archive to run kotlinc-jvm 2.4.20
  (the dead cogentco mirror was stalling `apt-get update`; temporarily
  dropped it, restored `/etc/apt/sources.list.d/ubuntu.sources` after).
- **2026-09-11 ~05:40** — Parser fleet step 4 done: `dog.rs` (single-file,
  zero-dep, stdlib only — `pub fn parse(text) -> Result<Value, DogError>`
  library API plus a `dog.py`-style CLI: `rustc --edition 2021 -O dog.rs
  -o dog && ./dog file.dog`). Mirrors dog.go/dog.js behavior, including the
  hard rule: REJECTS truncated directive names (`l:`, `di:`) as hard errors
  — does NOT copy dog.py's silent leniency (pending his D1 ruling).
  Conformance runner `tests/run_corpus.rs` (compiled with `rustc --test`,
  pulls in `../dog.rs` as a module — no Cargo project, no deps) →
  **39/39 green** — every corpus case vs `.expected.json`/`.error`, plus
  dog.py agreement on all 37 non-deviation cases (the 2 truncated-directive
  cases keep their documented `.known-deviation` vs dog.py). dog.py's own
  16-test suite still passes untouched; node `tests/run-corpus.js` still
  39/39. Rust CLI output verified semantically equal to dog.py on
  examples/{minimal,nested,yaml-variation}.dog. Toolchain: **rustc 1.98.1**
  (`~/.cargo`), zero warnings on both binary and `--test` builds. One real
  bug caught by the corpus during the port (dropped `@attr` keys when an
  element had children — `xml_value` only emitted attributes in the
  leaf branch); fixed, now matches dog.go exactly.
- **2026-09-11 ~04:55** — Parser fleet step 3 done: `dog.go` (single-file,
  zero-dep, `package main` on stdlib only — `Parse(text)` library API plus a
  `dog.py`-style CLI: `go run dog.go file.dog`). Mirrors dog.js behavior,
  including the hard rule: REJECTS truncated directive names (`l:`, `di:`)
  as hard errors — does NOT copy dog.py's silent leniency (pending his D1
  ruling). `go.mod` added (module `dog`, no dependencies) so `go vet`/`go test`
  run clean. Conformance: `go test -run TestCorpus` →
  **39/39 green** — every corpus case vs `.expected.json`/`.error`, plus
  dog.py agreement on all 37 non-deviation cases (the 2 truncated-directive
  cases keep their documented `.known-deviation` vs dog.py). dog.py's own
  16-test suite still passes untouched; node `tests/run-corpus.js` still
  39/39. Toolchain: **Go 1.27.1** (`~/toolchains/go`, PATH wired in
  `~/.bashrc`). Go CLI output verified byte-identical to dog.py on
  `examples/minimal.dog` modulo JSON key order (Go sorts map keys;
  semantically equal).
- **2026-09-11 ~04:00** — Parser fleet step 2 done: `dog.js` (single-file,
  zero-dep, node + browser via `typeof module` guard + `globalThis.dog`)
  mirrors dog.py behavior. Conformance corpus landed:
  `tests/corpus/` — 39 cases (29 valid incl. json-as-valid-.dog, all four
  lang presets, dict/rep/keys/sep/indent/quote, nesting, block scalars,
  rows, empty docs, byte-savings fixtures; 10 error cases) each with a
  `.dog` input + `.expected.json` or `.error` expectation.
  `node tests/run-corpus.js` → **39/39 green**, dog.py agreement checked
  per case. One honest deviation, documented in-corpus
  (`.known-deviation` files): dog.js REJECTS truncated directive names
  (`l:`, `di:`) as hard errors because abbreviations (D1) are not in the
  spec — dog.py silently ignores them as unknown directives, which
  mis-resolves (e.g. `l: json` parses a JSON body as native dog).
  Reconcile dog.py once the founder rules on D1. dog.py's own 16-test
  suite still passes untouched.
- **2026-09-11 ~02:51** — Adoption playbook research landed in the bag
  log. Verdict: flagship distribution beats spec quality; our flagship is
  Castle. Week-1 bundle (parsers on every package manager + one-pager +
  playground + bidirectional CLI) is the first move.
- **2026-09-11 ~02:50** — Bag log started. Parser fleet matrix opened;
  conformance-corpus-first discipline adopted. D1 (abbreviations) and D2
  (leniency principle) proposed; D1 needs founder sign-off (touches
  RULES.md R3.1). Integration/adoption research dispatched.
- **2026-09-11 ~02:48** — RULES.md added: hard rules from prior-art
  research (parser bounds, no implicit typing, grammar discipline, IP
  caution). Every spec change must satisfy every rule.
- **2026-09-11 ~02:34** — v1.0 shipped: SPEC.md, README.md, examples,
  `dog.py` reference parser (16/16 tests). Any-JSON-via-header proven
  with byte-identical diff; native syntax 43% smaller than pretty JSON,
  23% smaller than minified.
- **2026-09-11 ~02:31** — YEAHDOGS/.dog created (private). Founder
  directive: JSON, XML, YAML all expressible as .dog via header; header
  tools as needed; README must prove header-only JSON validity and the
  minimal-format savings.

## Correction: there is no v2 (2026-09-11, same night)
- Brandon killed the versioning: nothing has shipped, so there is no "DOG/2".
  The spec is v1, magic `.dog/1.0` — the first and only version. Earlier
  "DOG/2" references in this log meant "the current draft", not a release.
- §7 rewritten: no migration section, no migration fiction. Earlier drafts
  (`!dog 1`, `lang:` sketches) never shipped and are not recognized.
- Parsers target the v1 spec. Nothing is set in stone — greenfield.
