# .dog — the minimal text data format

`.dog` is plain text for data. A tiny **header** declares the syntax —
every brace, quote, hyphen, and separator is a header parameter — and the
body is your data. Two promises:

1. **JSON is just .dog with different settings.** No `lang:` shortcut, no
   special cases: the header parameters alone make any JSON file valid .dog.
   Same for YAML. Same for XML.
2. **Native .dog cuts the bytes dramatically** — smaller than minified JSON,
   while staying human-readable.

## Demo 1: JSON in, .dog out, body untouched

`examples/minimal.json` is ordinary pretty-printed JSON.
`examples/user-json.dog` shows the trick on a small record — the body is
byte-identical JSON, the 7-line header is the entire format declaration:

```
.dog/1.0
map-open: {
map-close: }
seq-open: [
seq-close: ]
key-sep: :
entry-sep: ,
bare: none

{"name": "Ava Reyes", "age": 29, ...}
```

Proof the body is byte-identical:

```sh
sed -n '10,$p' examples/user-json.dog | python3 -c "import json,sys; json.load(sys.stdin); print('VALID JSON')"
```

## Demo 2: the same data, three syntaxes, one value

`examples/user.dog` (native), `examples/user-json.dog` (JSON bundle), and
`examples/user-yaml.dog` (YAML bundle) carry identical data. A conforming
parser MUST produce the identical value for all three — syntax is
configurable, semantics are not. (YAML's syntax, without YAML's
Norway-problem implicit typing: a bare `NO` is the string `"NO"` in every
configuration.)

## Demo 3: the byte counts

The same data, rewritten in native .dog with the header tools:

```
.dog/1.0
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

`dog.py` — zero dependencies, stdlib only. Parses native .dog plus
`dict:`/`rep:`/`keys:` expansion, and emits equivalent JSON. (v2 ports of
the 6-language parser fleet are queued — see BAGLOG.)

```sh
python3 dog.py examples/minimal.dog
python3 test_dog.py   # 16 tests: round-trips, expansions, errors, byte savings
```

## Layout

```
SPEC.md            the format specification (v1)
README.md          this file
dog.py             reference parser (JSON out)
dog.js             JS parser, node + browser (zero-dep)
dog.go             Go parser, single-file zero-dep (stdlib only)
dog.rs             Rust parser, single-file zero-dep (stdlib only)
dog.kt             Kotlin parser, single-file zero-dep (stdlib only)
dog.c              C parser, single-file zero-dep (stdlib only: stdio/stdlib/string/ctype)
go.mod             Go module (no dependencies)
dog_corpus_test.go Go conformance runner: 39/39 corpus cases + dog.py agreement
tests/run-corpus.js   JS conformance runner: 39/39 corpus cases + dog.py agreement
tests/run_corpus.rs   Rust conformance runner: 39/39 corpus cases + dog.py agreement
tests/run_corpus.kt   Kotlin conformance runner: 39/39 corpus cases + dog.py agreement
tests/run-corpus.sh   C conformance runner: 39/39 corpus cases + dog.py agreement
test_dog.py        test suite
examples/
  minimal.json        sample data as pretty JSON (692 bytes)
  minimal.dog         same data, native dog (391 bytes)
  user.dog            the same record, native .dog config (v1)
  user-json.dog       the same record, JSON bundle config — body is byte-identical JSON
  user-yaml.dog       the same record, YAML bundle config
  nested.dog          nesting, lists, block scalars, rep in action
```

Spec: [SPEC.md](SPEC.md).

---
This product was made by DOGS — https://wearedogs.net
