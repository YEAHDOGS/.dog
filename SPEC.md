# .dog/1.0 — Format Specification (DOG/1)

`.dog` is a minimal plain-text data format. A short **header** declares the
syntax — every brace, quote, hyphen, and separator is a header parameter —
and the **body** carries the data in that syntax.

Design goals, in order:

1. **JSON is just .dog with different settings.** There is no `lang:`
   shortcut and no special case: the generic header parameters are
   expressive enough that a JSON document is valid .dog purely through
   header configuration. Same for YAML. Same for XML.
2. **Minimal when it matters.** The native configuration plus header tools
   (key dictionaries, repeated-string avoidance, positional rows) cuts
   typical JSON payloads roughly in half — smaller than minified JSON while
   staying human-readable.
3. **One data model, one type discipline.** Every .dog document, in any
   header configuration, denotes exactly one JSON-equivalent value. Syntax
   is configurable; semantics are not: no implicit typing, ever.

## 1. Document layout

```
.dog/1.0 map-open:{ map-close:} seq-open:[ seq-close:] key-sep:: entry-sep:, bare:none

{"name": "Ava", "age": 29}
```

- **Line 1** is the magic: exactly `.dog/1.0`. Anything else is not a .dog
  file.
- **Header**: exactly **one line** — line 1. It starts with the magic,
  followed by zero or more space-separated `name:value` parameters.
  A reader never has to guess where the header ends: it is always line 1.
- Parameter values containing spaces must be double-quoted:
  `keys:"n e r a"`. Inside quotes, a backslash escapes the next character
  (`\"`, `\\`). An unterminated quote is an error.
- A token without a `:` is an error. Directive names are case-insensitive;
  values are case-sensitive and taken exactly as written (quoting, not
  whitespace stripping, is how a value keeps its spaces).
- There are no header comment lines — the header is one line. (`#`
  full-line comments remain a body-syntax feature wherever the body
  syntax supports them.)
- **Body**: every line after line 1, parsed per the header's syntax
  parameters. One optional blank line immediately after the header is
  skipped, purely visual.
- A repeated directive: the last occurrence wins.
- **Unknown directives are ignored** — this is how the format grows without
  breaking old readers. There is no `lang:` directive: a `lang:` parameter
  in a `.dog/1.0` file is unknown and ignored (see §7).

## 2. Syntax parameters

| Directive   | Default    | Meaning |
|-------------|------------|---------|
| `map-open:` | (unset)    | Token opening a map, e.g. `{` |
| `map-close:`| (unset)    | Token closing a map, e.g. `}` |
| `seq-open:` | (unset)    | Token opening a sequence, e.g. `[` |
| `seq-close:`| (unset)    | Token closing a sequence, e.g. `]` |
| `key-sep:`  | `:`        | Token between a key and its value |
| `entry-sep:`| (newline)  | Token between entries, e.g. `,` |
| `seq-item:` | `- `       | Marker prefixing each sequence item in indent mode |
| `quote:`    | `"`        | String quote character |
| `quote2:`   | (unset)    | Alternate string quote character, e.g. `'` |
| `escape:`   | `\`        | Escape character inside quoted strings |
| `bare:`     | `strings`  | `strings`: bare scalars allowed. `none`: a bare scalar that isn't a number/`true`/`false`/`null` is an error |
| `indent:`   | `2`        | Advisory indent width for writers |
| `dict:`     | —          | Key dictionary: `alias=real` pairs, space-separated |
| `rep:`      | —          | Repeated-string avoidance: `token=expansion` pairs, space-separated |
| `keys:`     | —          | Positional key order: every body line is a `sep`-separated row of bare values |
| `sep:`      | `\|`       | Field separator for `keys:` rows |
| `tag-open:` | (unset)    | Token opening a named element, e.g. `<` |
| `tag-close:`| (unset)    | Token closing an element's open tag, e.g. `>` |
| `tag-end:`  | (unset)    | Token opening an element's close tag, e.g. `</` |
| `attr-eq:`  | (unset)    | Token between an attribute name and its value, e.g. `=` |
| `attr-key:` | `@`        | Key holding an element's attributes |
| `text-key:` | `#text`    | Key holding an element's text when it also has children/attributes |
| `empty-tag:`| (unset)    | Self-closing element suffix, e.g. `/>` |
| `dup-keys:` | `error`    | `array`: repeated sibling keys collect into a sequence; `error`: a repeated key is a document error |

### Grammar

- **Delimiter mode** — a construct whose open token is set: the open/close
  tokens bound it, entries are separated by `entry-sep` (a trailing
  separator is an error), and whitespace — including newlines — around
  structural tokens is insignificant.
- **Indent mode** — open token unset: entries are newline-delimited, nesting
  is by deeper indentation (readers accept any consistent deeper indent;
  tabs are forbidden; mixed levels are an error), and sequence items are
  prefixed with the `seq-item` marker.
- **Tag mode** — when `tag-open` is set, elements are named
  constructs: `tag-open name (attr attr-eq quoted-value)* tag-close
  content tag-end name tag-close`. Attributes collect under `attr-key`
  (default `@`). An element whose content is only text denotes that
  scalar directly; an element with children denotes a map of them; an
  element with both text and children is a map with its text under
  `text-key`. `empty-tag` (`<name/>`) denotes null. Repeated sibling
  element names collect into a sequence iff `dup-keys: array`, otherwise
  a repeated key is a document error. Tag mode is not XML-specific:
  BBCode (`[b]bold[/b]`) is the same bundle with `[`, `]`, `[/`.
- A map entry is `key key-sep value`. Keys are quoted strings or bare
  tokens (per `bare:`); `dict:` aliases expand them. A value is a quoted
  string, a bare scalar, a nested map/sequence, or a block scalar: `|`
  consumes the following deeper-indented lines as one literal string,
  newlines preserved, no trailing newline added.
- Quoted strings use `quote:` or `quote2:`. `escape:` introduces the
  standard escapes — `\"` `\'` `\\` `\n` `\t` `\r` `\b` `\f` `\/`
  `\uXXXX` — processed identically under both quote characters.
- Bare scalars: JSON number grammar, `true`, `false`, `null`. Anything else
  is a string iff `bare: strings`, otherwise a document error.
- `dict:` expands **keys**, `rep:` expands **string scalar values and
  keys** — longest token first, exactly once, never recursively (R1.1) — in
  every configuration, not just native. A backslash escapes the next
  character: `\~` is a literal `~` even when `~` is a rep token; `\\` is a
  literal backslash. Escaped characters are hidden from substitution.
- `keys:` declares the row layout: every body line is a row, `sep`-separated
  fields mapped positionally to the declared keys. It is only valid when
  `entry-sep` is newline; declaring it with an explicit `entry-sep` is a
  document error. Empty field → null. Field count must match exactly. Rows
  must not be indented.

### Expansion order

1. `dict:` expands keys (map keys and `keys:` row headers). Unknown aliases
   are kept as-is.
2. `rep:` expands scalar values and keys, longest token first.
3. `rep:` pairs split on whitespace, so an expansion cannot contain a
   literal space in v1 — use `%20` or choose tokens accordingly.

## 3. Configurations: JSON and YAML are parameter bundles

**JSON.** The body is byte-identical JSON — any JSON file becomes .dog by
prepending this header and changing zero body bytes:

```
.dog/1.0 map-open:{ map-close:} seq-open:[ seq-close:] key-sep:: entry-sep:, bare:none
```

**YAML** (block subset: mappings, sequences, nesting, plain/quoted
scalars). Indent mode is the default, so no open tokens are needed:

```
.dog/1.0 seq-item:"- " key-sep:: bare:strings
```

Anchors, flow styles, and tags are out of scope for now.

**Native .dog.** The header is just the magic line: indent mode, `- `
items, `:` key separator, bare strings allowed — plus `dict:`/`rep:`/
`keys:` where the bytes matter.

**XML.** Named elements fall out of the tag parameters — no special case:

```
.dog/1.0 tag-open:< tag-close:> tag-end:</ attr-eq:= quote:"\"" empty-tag:/> dup-keys:array
```

`examples/user-xml.dog` carries the same record as §4's trio: attributes
land under `@`, repeated `<tag>` siblings collect into an array because
`dup-keys: array`, and a bare `29` is still the number `29` — XML's
syntax, .dog's type discipline, none of XML's stringly-typed footguns.

Syntax is configurable; the data model and type discipline are invariant
across every configuration: a bare `NO` is the string `"NO"` in ALL of
them (R2.1 — YAML's Norway problem stays dead even in YAML syntax),
parsing never executes code or fetches resources (R1.3), and the R1.2
bounds (depth, dictionary size, expansion ratio, output size) are always
enforced.

## 4. Three configurations, one value

`examples/user.dog` (native), `examples/user-json.dog` (JSON bundle),
and `examples/user-yaml.dog` (YAML bundle) carry identical data: a
conforming parser MUST produce the identical JSON-equivalent value for
all three. `examples/user-xml.dog` carries the same record through the
tag-mode mapping, which is deterministic but shape-honest: attributes
live under `attr-key`, repeated siblings collect under their tag name,
and the single root element wraps the document. Any XML document is
expressible; the mapping never silently reinterprets.

## 5. Data model

Every .dog document denotes exactly one JSON-equivalent value (object,
array, string, number, boolean, or null). Maps → objects, sequences →
arrays, scalars per §2, rows → arrays of objects. A conforming parser MUST
round-trip: `parse(dog)` ≡ `parse(equivalent_json)`.

## 6. Errors

A conforming reader reports, at minimum: missing magic line, malformed
directive, bad indentation (tabs forbidden, mixed levels are an error),
bare scalar inside a map in indent mode, `bare: none` violation, row
field-count mismatch, `keys:` with an explicit `entry-sep`, and malformed
bodies. It MUST NOT silently reinterpret.

## 7. Versioning

This is v1, magic `.dog/1.0` — the first and only version. Nothing has
shipped before it, so there is nothing to migrate from: earlier drafts
(`!dog 1`, `lang:`-based sketches) were never released and are not
recognized. A `lang:` line in a v1 file is an unknown directive and is
ignored.

Unknown directives are ignored, so additive header tools never break v1
readers. A future breaking change bumps the magic (`.dog/2.0`).
