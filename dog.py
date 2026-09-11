#!/usr/bin/env python3
"""
dog.py -- reference parser for the .dog text data format (DOG/1, magic `.dog/1.0`).

Zero dependencies (stdlib only). Reads a .dog file, applies the header tools
(dict / rep / keys), and emits equivalent JSON.

Usage:
    python3 dog.py file.dog        # print equivalent JSON to stdout
"""

import json
import re
import sys
from xml.etree import ElementTree as ET

MAGIC = ".dog/1.0"

KNOWN_DIRECTIVES = {"lang", "dict", "rep", "keys", "sep", "indent", "quote"}


class DogError(Exception):
    """Raised for any malformed .dog document."""


# ---------------------------------------------------------------------------
# header
# ---------------------------------------------------------------------------

def split_header(text):
    """Split raw text into (directives dict, body lines).

    Line 1 must be the magic `.dog/1.0`. Header directives are `name: value`,
    one per line; a blank line ends the header (EOF also ends it, leniently).
    `#` lines are comments. Unknown directives are ignored (forward compat).
    """
    text = text.lstrip("\ufeff")
    lines = text.splitlines()
    if not lines or lines[0].strip() != MAGIC:
        raise DogError("first line must be the magic %r" % MAGIC)
    directives = {}
    body_start = len(lines)
    for idx in range(1, len(lines)):
        raw = lines[idx]
        if raw.strip() == "":
            body_start = idx + 1
            break
        s = raw.strip()
        if s.startswith("#"):
            continue
        if ":" not in s:
            raise DogError("bad header directive on line %d: %r" % (idx + 1, raw))
        name, val = s.split(":", 1)
        name = name.strip().lower()
        if not name or re.search(r"\s", name):
            raise DogError("bad directive name on line %d: %r" % (idx + 1, raw))
        directives[name] = val.strip()  # last one wins
    return directives, lines[body_start:]


def build_config(directives):
    cfg = {
        "lang": directives.get("lang", "dog").strip().lower(),
        "dict": {},
        "rep": [],          # [(token, expansion)], longest token first
        "keys": None,       # positional key order for row-oriented bodies
        "sep": directives.get("sep", "|"),
        "indent": 2,
        "quote": directives.get("quote", '"'),
    }
    if cfg["lang"] not in ("dog", "json", "xml", "yaml"):
        raise DogError("unsupported lang: %r (want dog|json|xml|yaml)"
                       % directives.get("lang"))
    if "dict" in directives:
        for pair in directives["dict"].split():
            if "=" not in pair:
                raise DogError("bad dict pair %r (want alias=key)" % pair)
            alias, real = pair.split("=", 1)
            if not alias or not real:
                raise DogError("bad dict pair %r" % pair)
            cfg["dict"][alias] = real
    if "rep" in directives:
        pairs = []
        for pair in directives["rep"].split():
            if "=" not in pair:
                raise DogError("bad rep pair %r (want token=expansion)" % pair)
            tok, exp = pair.split("=", 1)
            if not tok:
                raise DogError("empty rep token in %r" % pair)
            pairs.append((tok, exp))
        pairs.sort(key=lambda p: -len(p[0]))  # longest token wins on overlap
        cfg["rep"] = pairs
    if "keys" in directives:
        keys = directives["keys"].split()
        if not keys:
            raise DogError("empty keys: directive")
        cfg["keys"] = keys
    if "indent" in directives:
        try:
            n = int(directives["indent"])
        except ValueError:
            raise DogError("indent: must be an integer")
        if n < 1:
            raise DogError("indent: must be >= 1")
        cfg["indent"] = n
    if not cfg["sep"]:
        raise DogError("sep: must not be empty")
    if not cfg["quote"]:
        raise DogError("quote: must not be empty")
    return cfg


# ---------------------------------------------------------------------------
# expansion
# ---------------------------------------------------------------------------

def expand_rep(s, cfg):
    """Apply rep: substitutions to a scalar string.

    A backslash escapes the next character (so ``\\~`` is a literal tilde when
    ``~`` is a rep token). Longest tokens substitute first. Escaped characters
    are hidden behind a placeholder during substitution so a token can never
    match inside them.
    """
    if not cfg["rep"]:
        return s
    PH = "\ue000"
    saved = []
    out = []
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == "\\" and i + 1 < n:
            saved.append(s[i + 1])
            out.append(PH)
            i += 2
        else:
            out.append(c)
            i += 1
    s = "".join(out)
    for tok, exp in cfg["rep"]:
        s = s.replace(tok, exp)
    res = []
    si = 0
    for c in s:
        if c == PH:
            res.append(saved[si])
            si += 1
        else:
            res.append(c)
    return "".join(res)


def parse_scalar(s, cfg):
    """Parse a scalar string: quoting, then JSON-ish literal coercion."""
    s = expand_rep(s, cfg)
    q = cfg["quote"]
    if len(s) >= 2 and s[0] == q and s[-1] == q:
        inner = s[1:-1]
        return inner.replace("\\" + q, q).replace("\\\\", "\\")
    low = s.lower()
    if low == "null":
        return None
    if low == "true":
        return True
    if low == "false":
        return False
    if re.fullmatch(r"-?\d+", s):
        return int(s)
    if re.fullmatch(r"-?(\d+\.\d*|\.\d+|\d+)([eE][-+]?\d+)?", s):
        return float(s)
    return s


def coerce_loose(s):
    """Scalar coercion without rep (for XML text / YAML subset values)."""
    return parse_scalar(s, {"rep": [], "quote": '"'})


# ---------------------------------------------------------------------------
# native `dog` body: indentation-based maps / lists / scalars / rows
# ---------------------------------------------------------------------------

PAIR_RE = re.compile(r"^([^\s:]+)\s*:(?:[ \t]+(.*))?$")


def _indent_of(raw):
    i = 0
    while i < len(raw) and raw[i] == " ":
        i += 1
    if i < len(raw) and raw[i] == "\t":
        raise DogError("tabs are not allowed for indentation: %r" % raw)
    return i


def _body_items(body_lines):
    items = []
    for raw in body_lines:
        if raw.strip() == "" or raw.lstrip(" ").startswith("#"):
            continue
        items.append((_indent_of(raw), raw.strip(), raw))
    return items


def parse_block_scalar(items, i, level):
    """`key: |` -- following deeper-indented lines become one literal string."""
    if i >= len(items) or items[i][0] <= level:
        raise DogError("expected an indented block after `|`")
    base = items[i][0]
    buf = []
    while i < len(items) and items[i][0] >= base:
        ind, text, raw = items[i]
        buf.append(raw[base:] if len(raw) >= base else "")
        i += 1
    return "\n".join(buf), i


def parse_map(items, i, level, cfg, seed=None):
    d = {}
    if seed is not None:
        d[seed[0]] = seed[1]
    while i < len(items):
        ind, text, raw = items[i]
        if ind < level:
            break
        if ind != level:
            raise DogError("bad indentation in map at %r" % text)
        m = PAIR_RE.match(text)
        if not m:
            raise DogError("expected `key: value` in map, got %r" % text)
        raw_key, val = m.group(1), m.group(2)
        key = expand_rep(cfg["dict"].get(raw_key, raw_key), cfg)
        i += 1
        if val is None or val == "":
            if i < len(items) and items[i][0] > level:
                v, i = parse_value(items, i, items[i][0], cfg)
            else:
                v = None
        elif val == "|":
            v, i = parse_block_scalar(items, i, level)
        else:
            v = parse_scalar(val, cfg)
        d[key] = v  # last duplicate wins
    return d, i


def parse_list(items, i, level, cfg):
    out = []
    while i < len(items):
        ind, text, raw = items[i]
        if ind < level:
            break
        if ind != level:
            raise DogError("bad indentation in list at %r" % text)
        if not (text == "-" or text.startswith("- ")):
            break
        rest = text[1:].strip()
        i += 1
        if rest == "":
            if i < len(items) and items[i][0] > level:
                v, i = parse_value(items, i, items[i][0], cfg)
            else:
                v = None
            out.append(v)
        elif PAIR_RE.match(rest):
            m = PAIR_RE.match(rest)
            raw_key, val = m.group(1), m.group(2)
            key = expand_rep(cfg["dict"].get(raw_key, raw_key), cfg)
            if val is None or val == "":
                # `- key:` -- deeper block is the VALUE of key (YAML rule)
                if i < len(items) and items[i][0] > level:
                    v, i = parse_value(items, i, items[i][0], cfg)
                else:
                    v = None
                out.append({key: v})
            elif val == "|":
                v, i = parse_block_scalar(items, i, level)
                out.append({key: v})
            else:
                seed = (key, parse_scalar(val, cfg))
                if i < len(items) and items[i][0] > level:
                    d, i = parse_map(items, i, items[i][0], cfg, seed=seed)
                else:
                    d = {key: seed[1]}
                out.append(d)
        else:
            out.append(parse_scalar(rest, cfg))
    return out, i


def parse_value(items, i, level, cfg):
    ind, text, raw = items[i]
    if ind != level:
        raise DogError("bad indentation at %r" % text)
    if text == "-" or text.startswith("- "):
        return parse_list(items, i, level, cfg)
    if PAIR_RE.match(text):
        return parse_map(items, i, level, cfg)
    return parse_scalar(text, cfg), i + 1


def parse_rows(items, cfg):
    """keys: mode -- every body line is a sep-separated positional row."""
    keys = [cfg["dict"].get(k, k) for k in cfg["keys"]]  # aliases allowed
    sep = cfg["sep"]
    out = []
    for ind, text, raw in items:
        if ind != 0:
            raise DogError("positional rows must not be indented: %r" % text)
        fields = text.split(sep)
        if len(fields) != len(keys):
            raise DogError("row has %d fields but keys: declares %d: %r"
                           % (len(fields), len(keys), text))
        d = {}
        for k, f in zip(keys, fields):
            f = f.strip()
            d[k] = parse_scalar(f, cfg) if f != "" else None
        out.append(d)
    return out


def parse_dog_body(body_lines, cfg):
    items = _body_items(body_lines)
    if not items:
        return None
    if cfg["keys"] is not None:
        return parse_rows(items, cfg)
    val, nxt = parse_value(items, 0, 0, cfg)
    if nxt != len(items):
        raise DogError("unexpected trailing content: %r" % items[nxt][1])
    if items[0][0] != 0:
        raise DogError("top-level content must not be indented")
    return val


# ---------------------------------------------------------------------------
# json / xml / yaml bodies -- byte-identical, header alone makes them .dog
# ---------------------------------------------------------------------------

def parse_json_body(body_lines):
    text = "\n".join(body_lines).strip()
    if not text:
        return None
    try:
        return json.loads(text)
    except json.JSONDecodeError as e:
        raise DogError("bad JSON body: %s" % e)


def _xml_elem(elem):
    d = {}
    for k, v in elem.attrib.items():
        d["@" + k] = v  # XML attributes stay strings
    children = list(elem)
    text = (elem.text or "").strip()
    if not children:
        if d:
            if text:
                d["#text"] = text
            return d
        return coerce_loose(text)
    for child in children:
        v = _xml_elem(child)
        if child.tag in d:
            cur = d[child.tag]
            if isinstance(cur, list):
                cur.append(v)
            else:
                d[child.tag] = [cur, v]
        else:
            d[child.tag] = v
    return d


def parse_xml_body(body_lines):
    text = "\n".join(body_lines).strip()
    if not text:
        return None
    try:
        root = ET.fromstring(text)
    except ET.ParseError as e:
        raise DogError("bad XML body: %s" % e)
    return {root.tag: _xml_elem(root)}


# ---------------------------------------------------------------------------
# top-level
# ---------------------------------------------------------------------------

def parse(text):
    """Parse a .dog document, return the equivalent JSON-able Python value."""
    directives, body = split_header(text)
    cfg = build_config(directives)
    lang = cfg["lang"]
    if lang == "json":
        return parse_json_body(body)
    if lang == "xml":
        return parse_xml_body(body)
    if lang == "yaml":
        # header tools are native-dog features; YAML bodies stay byte-identical
        cfg = dict(cfg, dict={}, rep=[], keys=None)
        return parse_dog_body(body, cfg)  # common block-mapping subset
    return parse_dog_body(body, cfg)


def main(argv):
    if len(argv) != 2 or argv[1] in ("-h", "--help"):
        sys.stderr.write("usage: python3 dog.py file.dog\n")
        return 2
    try:
        with open(argv[1], encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        sys.stderr.write("dog: cannot read %s: %s\n" % (argv[1], e))
        return 2
    try:
        data = parse(text)
    except DogError as e:
        sys.stderr.write("dog error: %s\n" % e)
        return 1
    print(json.dumps(data, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
