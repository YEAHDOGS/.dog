// dog.go -- parser for the .dog text data format (DOG/1, magic `.dog/1.0`).
//
// Single-file, zero dependencies (stdlib only). Mirrors the dog.js behavior:
// in particular it REJECTS truncated directive names (l:, di:, ...) as hard
// errors, because abbreviated directives are not in the spec (bag log D1).
//
// Library use:
//
//	dog.Parse(text) -> (JSON-equivalent value, error)
//
// CLI use:
//
//	go run dog.go file.dog        # print equivalent JSON to stdout
//
// Errors are returned/thrown as dog.DogError.
//
// This product was made by DOGS -- https://wearedogs.net
package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

const MAGIC = ".dog/1.0"

var KNOWN_DIRECTIVES = []string{"lang", "dict", "rep", "keys", "sep", "indent", "quote"}

const placeholder = '\uE000' // must match dog.py / dog.js escape placeholder

// DogError is raised for any malformed .dog document.
type DogError struct{ msg string }

func (e *DogError) Error() string { return e.msg }

func dogErr(format string, args ...interface{}) *DogError {
	return &DogError{msg: fmt.Sprintf(format, args...)}
}

/* ------------------------------------------------------------------ */
/* header                                                              */
/* ------------------------------------------------------------------ */

func splitLines(text string) []string {
	// mirrors dog.js: text.split(/\r\n|[\n\r\x85\u2028\u2029]/)
	var lines []string
	var cur strings.Builder
	runes := []rune(text)
	i := 0
	for i < len(runes) {
		r := runes[i]
		if r == '\r' && i+1 < len(runes) && runes[i+1] == '\n' {
			lines = append(lines, cur.String())
			cur.Reset()
			i += 2
			continue
		}
		if r == '\n' || r == '\r' || r == '\x85' || r == '\u2028' || r == '\u2029' {
			lines = append(lines, cur.String())
			cur.Reset()
			i++
			continue
		}
		cur.WriteRune(r)
		i++
	}
	lines = append(lines, cur.String())
	return lines
}

func quoteJS(s string) string {
	// JSON.stringify-ish quoting for error messages (matches dog.js output)
	var b strings.Builder
	b.WriteByte('"')
	for _, r := range s {
		switch r {
		case '"':
			b.WriteString("\\\"")
		case '\\':
			b.WriteString("\\\\")
		case '\n':
			b.WriteString("\\n")
		case '\r':
			b.WriteString("\\r")
		case '\t':
			b.WriteString("\\t")
		default:
			if r < 0x20 {
				fmt.Fprintf(&b, "\\u%04x", r)
			} else {
				b.WriteRune(r)
			}
		}
	}
	b.WriteByte('"')
	return b.String()
}

// tokenizeHeaderLine splits a header line into whitespace-separated tokens.
// Double-quoted spans may contain spaces (`keys:"a b"`); inside quotes a
// backslash escapes the next character (`\"`, `\\`).
func tokenizeHeaderLine(line string) ([]string, error) {
	var tokens []string
	var cur strings.Builder
	flush := func() {
		if cur.Len() > 0 {
			tokens = append(tokens, cur.String())
			cur.Reset()
		}
	}
	inQuotes := false
	escaped := false
	for _, ch := range line {
		switch {
		case inQuotes && escaped:
			cur.WriteRune(ch)
			escaped = false
		case inQuotes && ch == '\\':
			escaped = true
		case inQuotes && ch == '"':
			inQuotes = false
		case inQuotes:
			cur.WriteRune(ch)
		case ch == '"':
			inQuotes = true
		case ch == ' ' || ch == '\t' || ch == '\r' || ch == '\v' || ch == '\f':
			flush()
		default:
			cur.WriteRune(ch)
		}
	}
	if inQuotes || escaped {
		return nil, dogErr("unterminated quote in header line: %s", quoteJS(line))
	}
	flush()
	return tokens, nil
}

// splitHeader: the header is exactly ONE line -- line 1, starting with the
// magic `.dog/1.0` followed by space-separated `name:value` params. A reader
// never has to guess where the header ends. The body is every line after
// line 1; one optional blank line right after the header is skipped.
func splitHeader(text string) (map[string]string, []string, error) {
	text = strings.TrimPrefix(text, "\ufeff")
	lines := splitLines(text)
	if len(lines) == 0 {
		return nil, nil, dogErr("first line must be the magic '.dog/1.0'")
	}
	tokens, err := tokenizeHeaderLine(lines[0])
	if err != nil {
		return nil, nil, err
	}
	if len(tokens) == 0 || tokens[0] != MAGIC {
		return nil, nil, dogErr("first line must be the magic '.dog/1.0'")
	}
	directives := map[string]string{}
	for _, tok := range tokens[1:] {
		ci := strings.Index(tok, ":")
		if ci < 0 {
			return nil, nil, dogErr("bad header directive %s (want name:value)", quoteJS(tok))
		}
		name := strings.ToLower(tok[:ci])
		if name == "" || strings.ContainsAny(name, " \t\n\r\f\v") {
			return nil, nil, dogErr("bad directive name in %s", quoteJS(tok))
		}
		// Abbreviated directives are NOT in the spec (needs founder sign-off,
		// bag log D1). A truncated known-directive name is a hard error here,
		// never silently resolved -- and never confused with the unknown-
		// directive ignore rule, which only covers names that are not
		// truncations of known directives.
		for _, full := range KNOWN_DIRECTIVES {
			if name != full && strings.HasPrefix(full, name) {
				return nil, nil, dogErr("truncated directive '%s:' is not valid -- abbreviations are not in the spec (want '%s:')", name, full)
			}
		}
		directives[name] = tok[ci+1:] // last one wins
	}
	body := lines[1:]
	if len(body) > 0 && strings.TrimSpace(body[0]) == "" {
		body = body[1:]
	}
	return directives, body, nil
}

type repPair struct{ tok, exp string }

type config struct {
	lang   string
	dict   map[string]string
	rep    []repPair // longest token first
	keys   []string  // positional key order for row-oriented bodies (nil = off)
	sep    string
	indent int
	quote  string
}

var indentRe = regexp.MustCompile(`^[+-]?\d+$`)

func buildConfig(directives map[string]string) (*config, error) {
	cfg := &config{
		dict:   map[string]string{},
		sep:    "|",
		indent: 2,
		quote:  "\"",
	}
	if v, ok := directives["lang"]; ok {
		cfg.lang = strings.ToLower(strings.TrimSpace(v))
	} else {
		cfg.lang = "dog"
	}
	switch cfg.lang {
	case "dog", "json", "xml", "yaml":
	default:
		raw := ""
		if v, ok := directives["lang"]; ok {
			raw = v
		}
		return nil, dogErr("unsupported lang: %s (want dog|json|xml|yaml)", quoteJS(raw))
	}
	if v, ok := directives["dict"]; ok {
		for _, pair := range strings.Fields(v) {
			ei := strings.Index(pair, "=")
			if ei < 0 {
				return nil, dogErr("bad dict pair %s (want alias=key)", quoteJS(pair))
			}
			alias, real := pair[:ei], pair[ei+1:]
			if alias == "" || real == "" {
				return nil, dogErr("bad dict pair %s", quoteJS(pair))
			}
			cfg.dict[alias] = real
		}
	}
	if v, ok := directives["rep"]; ok {
		var pairs []repPair
		for _, pair := range strings.Fields(v) {
			ei := strings.Index(pair, "=")
			if ei < 0 {
				return nil, dogErr("bad rep pair %s (want token=expansion)", quoteJS(pair))
			}
			tok, exp := pair[:ei], pair[ei+1:]
			if tok == "" {
				return nil, dogErr("empty rep token in %s", quoteJS(pair))
			}
			pairs = append(pairs, repPair{tok, exp})
		}
		sort.SliceStable(pairs, func(a, b int) bool {
			return len(pairs[a].tok) > len(pairs[b].tok)
		}) // longest token wins
		cfg.rep = pairs
	}
	if v, ok := directives["keys"]; ok {
		keys := strings.Fields(v)
		if len(keys) == 0 {
			return nil, dogErr("empty keys: directive")
		}
		cfg.keys = keys
	}
	if v, ok := directives["sep"]; ok {
		cfg.sep = v
	}
	if v, ok := directives["indent"]; ok {
		if !indentRe.MatchString(v) {
			return nil, dogErr("indent: must be an integer")
		}
		n, _ := strconv.Atoi(v)
		if n < 1 {
			return nil, dogErr("indent: must be >= 1")
		}
		cfg.indent = n
	}
	if v, ok := directives["quote"]; ok {
		cfg.quote = v
	}
	if cfg.sep == "" {
		return nil, dogErr("sep: must not be empty")
	}
	if cfg.quote == "" {
		return nil, dogErr("quote: must not be empty")
	}
	return cfg, nil
}

/* ------------------------------------------------------------------ */
/* expansion                                                           */
/* ------------------------------------------------------------------ */

func expandRep(s string, cfg *config) string {
	if len(cfg.rep) == 0 {
		return s
	}
	var saved []rune
	var out []rune
	rs := []rune(s)
	i, n := 0, len(rs)
	for i < n {
		if rs[i] == '\\' && i+1 < n {
			saved = append(saved, rs[i+1])
			out = append(out, placeholder)
			i += 2
		} else {
			out = append(out, rs[i])
			i++
		}
	}
	t := string(out)
	for _, p := range cfg.rep {
		t = strings.ReplaceAll(t, p.tok, p.exp)
	}
	var res []rune
	si := 0
	for _, r := range t {
		if r == placeholder {
			if si < len(saved) {
				res = append(res, saved[si])
			}
			si++
		} else {
			res = append(res, r)
		}
	}
	return string(res)
}

var intRe = regexp.MustCompile(`^-?\d+$`)
var floatRe = regexp.MustCompile(`^-?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?$`)

func parseScalar(s string, cfg *config) interface{} {
	s = expandRep(s, cfg)
	q := cfg.quote
	if qr := []rune(q); len(qr) == 1 && len(s) >= 2 && s[0] == q[0] && s[len(s)-1] == q[0] {
		inner := s[1 : len(s)-1]
		inner = strings.ReplaceAll(inner, "\\"+q, q)
		inner = strings.ReplaceAll(inner, "\\\\", "\\")
		return inner
	}
	low := strings.ToLower(s)
	if low == "null" {
		return nil
	}
	if low == "true" {
		return true
	}
	if low == "false" {
		return false
	}
	if intRe.MatchString(s) {
		if n, err := strconv.ParseInt(s, 10, 64); err == nil {
			return n
		}
	}
	if floatRe.MatchString(s) {
		if f, err := strconv.ParseFloat(s, 64); err == nil {
			return f
		}
	}
	return s
}

func coerceLoose(s string) interface{} {
	return parseScalar(s, &config{dict: map[string]string{}, quote: "\""})
}

/* ------------------------------------------------------------------ */
/* native `dog` body: indentation-based maps / lists / scalars / rows  */
/* ------------------------------------------------------------------ */

var pairRe = regexp.MustCompile(`^([^\s:]+)\s*:(?:[ \t]+(.*))?$`)

type item struct {
	indent int
	text   string
	raw    string
}

func indentOf(raw string) (int, error) {
	i := 0
	for i < len(raw) && raw[i] == ' ' {
		i++
	}
	if i < len(raw) && raw[i] == '\t' {
		return 0, dogErr("tabs are not allowed for indentation: %s", quoteJS(raw))
	}
	return i, nil
}

func bodyItems(bodyLines []string) ([]item, error) {
	var items []item
	for _, raw := range bodyLines {
		ind, err := indentOf(raw) // may throw on tabs -- same order as dog.js
		if err != nil {
			return nil, err
		}
		if strings.TrimSpace(raw) == "" {
			continue
		}
		if strings.HasPrefix(strings.TrimLeft(raw, " "), "#") {
			continue
		}
		items = append(items, item{indent: ind, text: strings.TrimSpace(raw), raw: raw})
	}
	return items, nil
}

func parseBlockScalar(items []item, i, level int) (string, int, error) {
	if i >= len(items) || items[i].indent <= level {
		return "", 0, dogErr("expected an indented block after `|`")
	}
	base := items[i].indent
	var buf []string
	for i < len(items) && items[i].indent >= base {
		raw := items[i].raw
		if len(raw) >= base {
			buf = append(buf, raw[base:])
		} else {
			buf = append(buf, "")
		}
		i++
	}
	return strings.Join(buf, "\n"), i, nil
}

func dictGet(dict map[string]string, key string) string {
	if v, ok := dict[key]; ok {
		return v
	}
	return key
}

func parseMap(items []item, i, level int, cfg *config, seedKey string, seedVal interface{}, hasSeed bool) (map[string]interface{}, int, error) {
	d := map[string]interface{}{}
	if hasSeed {
		d[seedKey] = seedVal
	}
	for i < len(items) {
		it := items[i]
		if it.indent < level {
			break
		}
		if it.indent != level {
			return nil, 0, dogErr("bad indentation in map at %s", quoteJS(it.text))
		}
		m := pairRe.FindStringSubmatch(it.text)
		if m == nil {
			return nil, 0, dogErr("expected `key: value` in map, got %s", quoteJS(it.text))
		}
		key := expandRep(dictGet(cfg.dict, m[1]), cfg)
		val := m[2] // "" when the value group did not participate either
		i++
		var v interface{}
		if val == "" {
			if i < len(items) && items[i].indent > level {
				var err error
				v, i, err = parseValue(items, i, items[i].indent, cfg)
				if err != nil {
					return nil, 0, err
				}
			} else {
				v = nil
			}
		} else if val == "|" {
			b, ni, err := parseBlockScalar(items, i, level)
			if err != nil {
				return nil, 0, err
			}
			v, i = b, ni
		} else {
			v = parseScalar(val, cfg)
		}
		d[key] = v // last duplicate wins
	}
	return d, i, nil
}

func parseList(items []item, i, level int, cfg *config) ([]interface{}, int, error) {
	var out []interface{}
	for i < len(items) {
		it := items[i]
		if it.indent < level {
			break
		}
		if it.indent != level {
			return nil, 0, dogErr("bad indentation in list at %s", quoteJS(it.text))
		}
		if !(it.text == "-" || strings.HasPrefix(it.text, "- ")) {
			break
		}
		rest := strings.TrimSpace(it.text[1:])
		i++
		if rest == "" {
			var v interface{}
			if i < len(items) && items[i].indent > level {
				var err error
				v, i, err = parseValue(items, i, items[i].indent, cfg)
				if err != nil {
					return nil, 0, err
				}
			} else {
				v = nil
			}
			out = append(out, v)
		} else if m := pairRe.FindStringSubmatch(rest); m != nil {
			key := expandRep(dictGet(cfg.dict, m[1]), cfg)
			val := m[2]
			if val == "" {
				// `- key:` -- deeper block is the VALUE of key (YAML rule)
				var v interface{}
				if i < len(items) && items[i].indent > level {
					var err error
					v, i, err = parseValue(items, i, items[i].indent, cfg)
					if err != nil {
						return nil, 0, err
					}
				} else {
					v = nil
				}
				out = append(out, map[string]interface{}{key: v})
			} else if val == "|" {
				b, ni, err := parseBlockScalar(items, i, level)
				if err != nil {
					return nil, 0, err
				}
				out = append(out, map[string]interface{}{key: b})
				i = ni
			} else {
				seedVal := parseScalar(val, cfg)
				if i < len(items) && items[i].indent > level {
					d, ni, err := parseMap(items, i, items[i].indent, cfg, key, seedVal, true)
					if err != nil {
						return nil, 0, err
					}
					out = append(out, d)
					i = ni
				} else {
					out = append(out, map[string]interface{}{key: seedVal})
				}
			}
		} else {
			out = append(out, parseScalar(rest, cfg))
		}
	}
	return out, i, nil
}

func parseValue(items []item, i, level int, cfg *config) (interface{}, int, error) {
	it := items[i]
	if it.indent != level {
		return nil, 0, dogErr("bad indentation at %s", quoteJS(it.text))
	}
	if it.text == "-" || strings.HasPrefix(it.text, "- ") {
		return parseList(items, i, level, cfg)
	}
	if pairRe.MatchString(it.text) {
		return parseMap(items, i, level, cfg, "", nil, false)
	}
	return parseScalar(it.text, cfg), i + 1, nil
}

func parseRows(items []item, cfg *config) ([]interface{}, error) {
	// aliases from dict: are allowed in keys: -- dict expands, rep does not
	keys := make([]string, len(cfg.keys))
	for k, name := range cfg.keys {
		keys[k] = dictGet(cfg.dict, name)
	}
	sep := cfg.sep
	var out []interface{}
	for _, it := range items {
		if it.indent != 0 {
			return nil, dogErr("positional rows must not be indented: %s", quoteJS(it.text))
		}
		fields := strings.Split(it.text, sep)
		if len(fields) != len(keys) {
			return nil, dogErr("row has %d fields but keys: declares %d: %s",
				len(fields), len(keys), quoteJS(it.text))
		}
		d := map[string]interface{}{}
		for f, k := range keys {
			fv := strings.TrimSpace(fields[f])
			if fv == "" {
				d[k] = nil
			} else {
				d[k] = parseScalar(fv, cfg)
			}
		}
		out = append(out, d)
	}
	return out, nil
}

func parseDogBody(bodyLines []string, cfg *config) (interface{}, error) {
	items, err := bodyItems(bodyLines)
	if err != nil {
		return nil, err
	}
	if len(items) == 0 {
		return nil, nil
	}
	if cfg.keys != nil {
		return parseRows(items, cfg)
	}
	v, nxt, err := parseValue(items, 0, 0, cfg)
	if err != nil {
		return nil, err
	}
	if nxt != len(items) {
		return nil, dogErr("unexpected trailing content: %s", quoteJS(items[nxt].text))
	}
	if items[0].indent != 0 {
		return nil, dogErr("top-level content must not be indented")
	}
	return v, nil
}

/* ------------------------------------------------------------------ */
/* json / xml / yaml bodies -- byte-identical, header alone makes .dog */
/* ------------------------------------------------------------------ */

func parseJsonBody(bodyLines []string) (interface{}, error) {
	text := strings.TrimSpace(strings.Join(bodyLines, "\n"))
	if text == "" {
		return nil, nil
	}
	var v interface{}
	dec := json.NewDecoder(strings.NewReader(text))
	if err := dec.Decode(&v); err != nil {
		return nil, dogErr("bad JSON body: %s", err.Error())
	}
	if dec.More() {
		return nil, dogErr("bad JSON body: unexpected trailing data")
	}
	return v, nil
}

// Minimal XML parser: elements, attributes, text, comments, PIs, DOCTYPE
// skipping, and the five predefined + numeric entities. Enough for the
// v1 mapping (attributes -> "@name", repeated siblings -> arrays,
// element text -> "#text" when attributes/children exist).
type xmlNode struct {
	tag      string
	attrs    map[string]string
	children []*xmlNode
	text     string
}

type xmlParser struct {
	s   string
	pos int
}

func (p *xmlParser) fail(format string, args ...interface{}) *DogError {
	return dogErr("bad XML body: "+format, args...)
}

var xmlEntRe = regexp.MustCompile(`&(#\d+|#[xX][0-9a-fA-F]+|[a-zA-Z]+);`)

func unescapeXml(s string) (string, *DogError) {
	var uerr *DogError
	out := xmlEntRe.ReplaceAllStringFunc(s, func(m string) string {
		e := xmlEntRe.FindStringSubmatch(m)[1]
		switch e {
		case "amp":
			return "&"
		case "lt":
			return "<"
		case "gt":
			return ">"
		case "quot":
			return "\""
		case "apos":
			return "'"
		}
		if strings.HasPrefix(e, "#") {
			hex := len(e) > 1 && (e[1] == 'x' || e[1] == 'X')
			digits := e[1:]
			if hex {
				digits = e[2:]
			}
			base := 10
			if hex {
				base = 16
			}
			code, err := strconv.ParseInt(digits, base, 64)
			if err != nil || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF) || code < 0 {
				uerr = dogErr("bad XML body: bad character reference %s", m)
				return m
			}
			return string(rune(code))
		}
		uerr = dogErr("bad XML body: unknown entity %s", m)
		return m
	})
	if uerr != nil {
		return "", uerr
	}
	return out, nil
}

func isXmlSpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'
}

func (p *xmlParser) skipWs() {
	for p.pos < len(p.s) && isXmlSpace(p.s[p.pos]) {
		p.pos++
	}
}

func (p *xmlParser) readName() (string, *DogError) {
	start := p.pos
	for p.pos < len(p.s) {
		c := p.s[p.pos]
		if isXmlSpace(c) || c == '/' || c == '>' || c == '=' {
			break
		}
		p.pos++
	}
	if p.pos == start {
		return "", p.fail("expected a name at offset %d", p.pos)
	}
	return p.s[start:p.pos], nil
}

func (p *xmlParser) skipMisc() *DogError {
	for {
		switch {
		case strings.HasPrefix(p.s[p.pos:], "<!--"):
			e := strings.Index(p.s[p.pos+4:], "-->")
			if e < 0 {
				return p.fail("unterminated comment")
			}
			p.pos += 4 + e + 3
		case strings.HasPrefix(p.s[p.pos:], "<?"):
			e := strings.Index(p.s[p.pos+2:], "?>")
			if e < 0 {
				return p.fail("unterminated processing instruction")
			}
			p.pos += 2 + e + 2
		case strings.HasPrefix(p.s[p.pos:], "<!"):
			d := p.pos + 2
			depth := 0
			for d < len(p.s) {
				if p.s[d] == '>' && depth == 0 {
					break
				}
				if p.s[d] == '[' {
					depth++
				}
				if p.s[d] == ']' {
					depth--
				}
				d++
			}
			if d >= len(p.s) {
				return p.fail("unterminated declaration")
			}
			p.pos = d + 1
		default:
			return nil
		}
	}
}

func (p *xmlParser) parseElement() (*xmlNode, *DogError) {
	p.pos++ // consume '<'
	name, err := p.readName()
	if err != nil {
		return nil, err
	}
	attrs := map[string]string{}
	for {
		p.skipWs()
		if strings.HasPrefix(p.s[p.pos:], "/>") {
			p.pos += 2
			return &xmlNode{tag: name, attrs: attrs}, nil
		}
		if p.pos < len(p.s) && p.s[p.pos] == '>' {
			p.pos++
			break
		}
		if p.pos >= len(p.s) {
			return nil, p.fail("unterminated start tag <%s>", name)
		}
		an, err := p.readName()
		if err != nil {
			return nil, err
		}
		p.skipWs()
		if p.pos >= len(p.s) || p.s[p.pos] != '=' {
			return nil, p.fail("expected '=' after attribute %s", an)
		}
		p.pos++
		p.skipWs()
		if p.pos >= len(p.s) || (p.s[p.pos] != '"' && p.s[p.pos] != '\'') {
			return nil, p.fail("attribute value must be quoted")
		}
		q := p.s[p.pos]
		p.pos++
		start := p.pos
		for p.pos < len(p.s) && p.s[p.pos] != q {
			p.pos++
		}
		if p.pos >= len(p.s) {
			return nil, p.fail("unterminated attribute value")
		}
		v := p.s[start:p.pos]
		p.pos++
		uv, uerr := unescapeXml(v)
		if uerr != nil {
			return nil, uerr
		}
		attrs[an] = uv
	}
	var children []*xmlNode
	var textAcc strings.Builder // leading text before the first child (mirror ET's .text)
	seenChild := false
	for {
		if p.pos >= len(p.s) {
			return nil, p.fail("unterminated element <%s>", name)
		}
		if p.s[p.pos] == '<' {
			switch {
			case strings.HasPrefix(p.s[p.pos:], "</"):
				p.pos += 2
				p.skipWs()
				cn, err := p.readName()
				if err != nil {
					return nil, err
				}
				p.skipWs()
				if p.pos >= len(p.s) || p.s[p.pos] != '>' {
					return nil, p.fail("expected '>' in closing tag")
				}
				p.pos++
				if cn != name {
					return nil, p.fail("mismatched close tag </%s> for <%s>", cn, name)
				}
				return &xmlNode{tag: name, attrs: attrs, children: children, text: textAcc.String()}, nil
			case strings.HasPrefix(p.s[p.pos:], "<!--"):
				e := strings.Index(p.s[p.pos+4:], "-->")
				if e < 0 {
					return nil, p.fail("unterminated comment")
				}
				p.pos += 4 + e + 3
			case strings.HasPrefix(p.s[p.pos:], "<?"):
				e := strings.Index(p.s[p.pos+2:], "?>")
				if e < 0 {
					return nil, p.fail("unterminated processing instruction")
				}
				p.pos += 2 + e + 2
			case strings.HasPrefix(p.s[p.pos:], "<![CDATA["):
				e := strings.Index(p.s[p.pos+9:], "]]>")
				if e < 0 {
					return nil, p.fail("unterminated CDATA section")
				}
				if !seenChild {
					textAcc.WriteString(p.s[p.pos+9 : p.pos+9+e])
				}
				p.pos += 9 + e + 3
			case strings.HasPrefix(p.s[p.pos:], "<!"):
				return nil, p.fail("unexpected declaration inside element")
			default:
				child, err := p.parseElement()
				if err != nil {
					return nil, err
				}
				children = append(children, child)
				seenChild = true
			}
		} else {
			e2 := strings.IndexByte(p.s[p.pos:], '<')
			var t string
			if e2 < 0 {
				t = p.s[p.pos:]
				p.pos = len(p.s)
			} else {
				t = p.s[p.pos : p.pos+e2]
				p.pos += e2
			}
			if !seenChild {
				textAcc.WriteString(t) // tail after children is ignored, like ET
			}
		}
	}
}

func xmlValue(node *xmlNode) (interface{}, *DogError) {
	d := map[string]interface{}{}
	for k, v := range node.attrs {
		d["@"+k] = v
	}
	if len(node.children) == 0 {
		t, uerr := unescapeXml(node.text)
		if uerr != nil {
			return nil, uerr
		}
		t = strings.TrimSpace(t)
		if len(node.attrs) > 0 {
			if t != "" {
				d["#text"] = t
			}
			return d, nil
		}
		return coerceLoose(t), nil
	}
	for _, child := range node.children {
		v, verr := xmlValue(child)
		if verr != nil {
			return nil, verr
		}
		if cur, ok := d[child.tag]; ok {
			if arr, isArr := cur.([]interface{}); isArr {
				d[child.tag] = append(arr, v)
			} else {
				d[child.tag] = []interface{}{cur, v}
			}
		} else {
			d[child.tag] = v
		}
	}
	return d, nil
}

func parseXmlBody(bodyLines []string) (interface{}, error) {
	text := strings.TrimSpace(strings.Join(bodyLines, "\n"))
	if text == "" {
		return nil, nil
	}
	p := &xmlParser{s: text}
	p.skipWs()
	if err := p.skipMisc(); err != nil {
		return nil, err
	}
	p.skipWs()
	if p.pos >= len(p.s) || p.s[p.pos] != '<' {
		return nil, p.fail("no root element")
	}
	root, err := p.parseElement()
	if err != nil {
		return nil, err
	}
	p.skipWs()
	if err := p.skipMisc(); err != nil {
		return nil, err
	}
	p.skipWs()
	if p.pos != len(p.s) {
		return nil, p.fail("junk after root element")
	}
	rv, verr := xmlValue(root)
	if verr != nil {
		return nil, verr
	}
	return map[string]interface{}{root.tag: rv}, nil
}

/* ------------------------------------------------------------------ */
/* top-level                                                           */
/* ------------------------------------------------------------------ */

// Parse parses a .dog document and returns the equivalent JSON-able value:
// map[string]interface{}, []interface{}, string, int64, float64, bool, or nil.
func Parse(text string) (interface{}, error) {
	directives, body, err := splitHeader(text)
	if err != nil {
		return nil, err
	}
	cfg, err := buildConfig(directives)
	if err != nil {
		return nil, err
	}
	switch cfg.lang {
	case "json":
		return parseJsonBody(body)
	case "xml":
		return parseXmlBody(body)
	case "yaml":
		// header tools are native-dog features; YAML bodies stay byte-identical
		cfg = &config{
			lang: cfg.lang, dict: map[string]string{},
			sep: cfg.sep, indent: cfg.indent, quote: cfg.quote,
		}
		return parseDogBody(body, cfg) // common block-mapping subset
	default:
		return parseDogBody(body, cfg)
	}
}

/* ------------------------------------------------------------------ */
/* CLI (mirrors dog.py's main)                                         */
/* ------------------------------------------------------------------ */

func main() {
	argv := os.Args
	if len(argv) != 2 || argv[1] == "-h" || argv[1] == "--help" {
		fmt.Fprintf(os.Stderr, "usage: %s file.dog\n", argv[0])
		os.Exit(2)
	}
	text, err := os.ReadFile(argv[1])
	if err != nil {
		fmt.Fprintf(os.Stderr, "dog: cannot read %s: %s\n", argv[1], err)
		os.Exit(2)
	}
	data, perr := Parse(string(text))
	if perr != nil {
		fmt.Fprintf(os.Stderr, "dog error: %s\n", perr)
		os.Exit(1)
	}
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	enc.SetIndent("", "  ")
	if err := enc.Encode(data); err != nil {
		fmt.Fprintf(os.Stderr, "dog error: %s\n", err)
		os.Exit(1)
	}
	fmt.Print(buf.String())
}
