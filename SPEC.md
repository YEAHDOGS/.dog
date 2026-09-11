# .dog/1.0 — Format Specification (DOG/1)

`.dog` is a minimal plain-text data format. A short **header** carries encoding
tools — key dictionaries, repeated-string avoidance, positional row layouts —
and the **body** carries the data in one of four languages: native `dog`,
or byte-identical `json`, `xml`, `yaml`.

Design goals, in order:

1. **Any JSON file is valid .dog.** Add the header; change zero body bytes.
2. **Minimal when it matters.** Native `dog` syntax plus header tools cuts
   typical JSON payloads roughly in half — smaller than minified JSON while
   staying human-readable.
3. **One data model.** Every .dog document, in any body language, denotes
   exactly one JSON-equivalent value (object, array, string, number,
   boolean, or null).

## 1. Document layout

```
.dog/1.0
lang: dog
dict: n=name a=age
rep: ~=https://example.com/

n: Ava
a: 29
```

- **Line 1** is the magic: exactly `.dog/1.0`. Anything else is not a .dog file.
- **Header**: directive lines, one per line, `name: value`. Ends at the first
  blank line (a file that ends without a blank line is accepted leniently).
- **Body**: everything after the header. Interpreted per `lang:`.

## 2. Header directives

| Directive | Default | Meaning |
|-----------|---------|---------|
| `lang:`   | `dog`   | Body language: `dog`, `json`, `xml`, `yaml` |
| `dict:`   | —       | Key dictionary: `alias=real` pairs, space-separated. Body keys written as aliases expand to the real key. |
| `rep:`    | —       | Repeated-string avoidance: `token=expansion` pairs, space-separated. The token expands everywhere in native-dog scalar values and keys. |
| `keys:`   | —       | Positional key order. When present, every body line is a `sep`-separated row of bare values mapped to these keys in order. Aliases from `dict:` are allowed here. |
| `sep:`    | `\|`    | Field separator for `keys:` rows. |
| `indent:` | `2`     | Advisory indent width for writers. Readers accept any consistent deeper indentation. |
| `quote:`  | `"`     | Quote character for string scalars. |

Rules:

- Directive names are case-insensitive; values are case-sensitive (`lang:` aside).
- `#` starts a comment line in the header. There are no inline comments.
- A repeated directive: the last occurrence wins.
- **Unknown directives are ignored.** A v1 reader skips what it doesn't know,
  which is how the format grows without breaking old files.
- `rep:` pairs split on whitespace, so an expansion cannot contain a literal
  space in v1 — use `%20` or choose tokens accordingly.

### Expansion order and escaping

1. `dict:` expands **keys** (map keys, and `keys:` row headers). Unknown
   aliases are kept as-is.
2. `rep:` expands **scalar values and keys**, longest token first, so
   overlapping tokens behave predictably.
3. A backslash escapes the next character: `\~` is a literal `~` even when
   `~` is a rep token; `\\` is a literal backslash. Escaped characters are
   hidden from substitution entirely.

`dict:`, `rep:`, and `keys:` are **native-dog tools**. Bodies in `json`,
`xml`, or `yaml` are byte-identical to the source format — no expansion is
applied to them, ever.

## 3. `lang: json` / `xml` / `yaml` — the header is the format

The body is raw JSON, XML, or YAML, byte for byte. The header alone makes it
a valid .dog file:

```
.dog/1.0
lang: json

{"name": "Ava", "age": 29}
```

This is the whole trick behind goal #1: take any JSON file, prepend the
header, and it is .dog. Tooling can then translate it into native `dog`
syntax to harvest the byte savings, or leave it untouched.

The reference parser maps each body to the same JSON-equivalent value:

- **JSON**: parsed as JSON, errors reported as dog errors.
- **XML**: parsed with a standard XML parser. Attributes become `"@name"`
  keys (always strings), repeated sibling elements become arrays, element
  text becomes `"#text"` when the element also has attributes/children,
  otherwise a coerced scalar. `<user active="true"><name>Ava</name></user>`
  becomes `{"@active": "true", "name": "Ava"}`.
- **YAML**: the common block subset — mappings, sequences, nesting by
  indentation, plain/quoted scalars. Anchors, flow styles, and tags are out
  of scope for v1; the reference parser reuses the native-dog grammar for
  this subset.

## 4. `lang: dog` — native minimal syntax

### Scalars

`42`, `3.14`, `-7`, `true`, `false`, `null` behave like JSON. Anything else
is a string. Quote with `"` when a value needs to survive literally
(leading/trailing spaces, a value that looks like a number, etc.).
`""` is the empty string; a bare empty value means null (see maps).

### Maps

```dog
name: Ava Reyes
age: 29
admin: true
```

`key: value`, one per line. Keys are single tokens (no whitespace); the value
runs to end of line and may contain colons (`url: https://x` is fine —
split on the *first* colon). A bare scalar line inside a map is an error;
ambiguity is not a feature.

Nesting is by indentation:

```dog
server:
  host: castle
  ports:
    http: 80
```

`key:` with nothing after it takes the deeper-indented block as its value,
or null if nothing follows. Duplicate keys: last wins.

### Sequences

```dog
- Ava
- Ben
- name: Cy
  age: 41
```

`- ` starts an item: a scalar, or `key: value` beginning a map whose
continuation lines sit deeper-indented. `- key:` (empty) takes the deeper
block as that key's value, the YAML rule.

### Block scalars

```dog
note: |
  Runs behind the modem.
  No cookies. No banners.
```

`| ` consumes the following deeper-indented lines as one literal string,
newlines preserved, no trailing newline added. Relative indentation inside
the block is kept.

### Positional rows (`keys:`)

```dog
.dog/1.0
lang: dog
dict: n=name a=age
rep: ~=@example.com
keys: n a
sep: |

Ava|29
Ben|~41
```

When `keys:` is declared, **every** body line is a row: `sep`-separated
fields mapped positionally to the declared keys. Empty field → null.
Field count must match the key count exactly, or the document is invalid.
Rows must not be indented. This is where the byte savings live: the key
names are declared once instead of repeated on every row.

## 5. Data model

Every .dog document denotes exactly one JSON-equivalent value. Native `dog`
bodies map to JSON as: maps → objects, sequences → arrays, scalars per
§4, rows → arrays of objects. A conforming parser MUST round-trip:
`parse(native_dog)` ≡ `parse(equivalent_json)`.

## 6. Errors

A conforming reader reports, at minimum: missing magic line, malformed
directive, unknown `lang:`, bad indentation (tabs are forbidden; mixed
levels are an error), bare scalar inside a map, row field-count mismatch,
and malformed `json`/`xml` bodies. It MUST NOT silently reinterpret.

## 7. Versioning

This is DOG/1, magic `.dog/1.0`. Future versions bump the magic
(`.dog/2.0`). Unknown directives are ignored (see §2), so additive header
tools never break v1 readers.
