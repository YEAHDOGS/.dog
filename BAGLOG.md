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
| Go | queued | Linux/macOS/Windows, servers | |
| Rust | queued | everywhere, incl. embedded | candidate for the canonical fast parser |
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

## Integration — ideas (research running)

- Dogfood it: .dog as the config/data format across Castle; Chains
  metadata envelopes.
- Adoption playbook research in flight — will land here as the sequenced
  plan (killer vector, conformance strategy, graveyard lessons).

---

## Log

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
