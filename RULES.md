# .dog Hard Rules

Non-negotiable design constraints, learned from the formats that came before.
**Every change to the .dog spec must satisfy every rule below.** If a proposed
feature breaks one, the feature loses — no exceptions.

Derived from prior-art research (2026-09-11): XML's collapse under verbosity
and spec-stack sprawl, YAML's implicit-typing data corruption ("Norway
problem"), billion-laughs entity-expansion denial of service, and arbitrary
code execution via unsafe deserialization.

---

## 1. Parser safety — the header is an entity mechanism, treat it like one

A header that defines aliases (`dict:`), substitutions (`rep:`), or row
layouts (`keys:`) is exactly the mechanism behind XML's billion-laughs
attack. Therefore:

- **R1.1** — No unbounded recursive expansion. Aliases and substitutions
  expand exactly once, never recursively, never in cycles.
- **R1.2** — Hard bounds, always enforced: nesting depth, dictionary size,
  expansion ratio (decoded bytes vs. source bytes), token/string length, and
  total decoded output. A document exceeding any bound is rejected, not
  truncated.
- **R1.3** — Parsing never executes code, instantiates arbitrary classes,
  fetches remote resources, or resolves external entities. Not by default, not
  via opt-in header directive. The parser reads data; that is all it does.
- **R1.4** — Unknown header directives are ignored for forward compatibility,
  never executed or fetched.

## 2. Type discipline — what you write is what you get

- **R2.1** — No implicit scalar typing. A bare `NO` is the string `"NO"`,
  never `false`. (YAML's Norway problem corrupted real data this way.)
- **R2.2** — No version-dependent interpretation. Identical bytes parse
  identically under every compliant parser, forever. The spec version in the
  magic line (`.dog/1.0`) selects behavior explicitly — never heuristics.
- **R2.3** — Deterministic canonical interpretation across implementations.
  If two parsers can disagree, the spec is wrong; fix the spec.

## 3. Grammar discipline — one minimal core, no sprawl

- **R3.1** — One spelling per thing. No equivalent-but-different syntaxes for
  the same construct, no heuristic parsing.
- **R3.2** — One bounded core grammar. The format must not grow overlapping
  schema, query, transformation, or namespace stacks (this is how XML died).
- **R3.3** — Extensions are explicit, versioned, ignorable, and deny-by-default.
  A parser that doesn't know an extension skips it safely; it never guesses.
- **R3.4** — `lang:` presets (json/xml/yaml) are macros over declared
  parameters, not special cases. A preset must be expressible as its
  parameter bundle, documented in SPEC.md.

## 4. Minimalism budget

- **R4.1** — Every new header directive must justify its bytes: it must earn
  back more than it costs across realistic documents, or it doesn't ship.
- **R4.2** — Convenience never overrides R1–R3. A friendlier syntax that
  weakens a safety bound is rejected.

## 5. Intellectual-property caution

- **R5.1** — Prior-art web research (2026-09-11) found no patent assertions,
  claims, or litigation for any format reviewed (MessagePack, Smile, UBJSON,
  BSON, JSON5, HOCON, TOML, RON, SDLang, JSONH, CSVY, CBOR, zstd/Brotli).
  **This is a factual flag, not a clearance** — the sweep was search-driven,
  not a patent-database query.
- **R5.2** — Independent patent review is required before the spec is
  published publicly or any patent filing proceeds.
- **R5.3** — No public disclosure of novel mechanisms beyond what the founder
  explicitly approves.

---

*These rules are the memory of other people's failures. Respect them.*
