# .dog — the minimal text data format

`.dog` is plain text for data. A tiny **header** carries encoding tools
(key dictionaries, repeated-string avoidance, positional rows); the body is
your data. Two promises:

1. **Any JSON file becomes valid .dog by adding the header.** Zero body changes.
2. **Native .dog cuts the bytes dramatically** — smaller than minified JSON,
   while staying human-readable.

## Demo 1: JSON in, .dog out, body untouched

`examples/minimal.json` is ordinary pretty-printed JSON. `examples/json-variation.dog`
is that exact file with a 3-line header:

```
.dog/1.0
lang: json

[{"active":true,"email":"ava.reyes@example.com", ... }]
```

Proof the body is byte-identical:

```sh
diff <(sed -n '4,$p' examples/json-variation.dog) examples/minimal.json && echo IDENTICAL
```

`lang: json` (or `xml`, `yaml`) means: the body is raw JSON, untouched —
the header alone makes it .dog.

## Demo 2: the byte counts

The same data, rewritten in native `lang: dog` with the header tools:

```
.dog/1.0
lang: dog
dict: n=name e=email r=role a=active
rep: ~=@example.com
keys: n e r a
sep: |

Ava Reyes|ava.reyes~|admin|true
Ben Okafor|ben.okafor~|member|true
Cy Lindqvist|cy.lindqvist~|member|true
Dee Park|dee.park~|member|false
Eli Vance|eli.vance~|moderator|true
Fay Hassan|fay.hassan~|member|true
```

`dict:` aliases the repeated keys. `rep:` kills the repeated `@example.com`.
`keys:` declares the shape once so six rows carry zero key names. Measured
with `wc -c`, same data, all three verified to parse identically:

| format | bytes |
|---|---|
| pretty-printed JSON (`minimal.json`) | 692 |
| minified JSON | 510 |
| **native .dog (`minimal.dog`)** | **391** |

That's **43% smaller than the JSON a human writes**, and **23% smaller than
minified JSON** — while remaining readable and editable by hand. The header
is the compression dictionary, in plain sight.

## The reference tool

`dog.py` — zero dependencies, stdlib only. Parses all four body languages
plus `dict:`/`rep:`/`keys:` expansion, and emits equivalent JSON:

```sh
python3 dog.py examples/minimal.dog
python3 dog.py examples/xml-variation.dog
python3 test_dog.py   # 16 tests: round-trips, expansions, errors, byte savings
```

## Layout

```
SPEC.md            the format specification (DOG/1)
README.md          this file
dog.py             reference parser (JSON out)
test_dog.py        test suite
examples/
  minimal.json        sample data as pretty JSON (692 bytes)
  minimal.dog         same data, native dog (391 bytes)
  json-variation.dog  same JSON with only a header added
  xml-variation.dog   same data as XML
  yaml-variation.dog  same data as YAML
  nested.dog          nesting, lists, block scalars, rep in action
```

Spec: [SPEC.md](SPEC.md).

---
This product was made by DOGS — https://wearedogs.net
