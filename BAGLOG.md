# Dot Dog Bag Log

The running log for the .dog text format project. Newest entries first.
Ideas go here before they become spec, spec changes, or code.

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
| Kotlin | queued | Android | Chat Now / Castle Android read .dog configs |
| Swift | queued | iOS | |
| Java | queued | Android, servers | |
| C# | queued | Windows, .NET | Phoenix tooling is PowerShell/.NET-adjacent |
| C | queued | embedded, Castle OS | |
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
