// dog.rs -- parser for the .dog text data format (DOG/1, magic `.dog/1.0`).
//
// Single-file, zero dependencies (stdlib only). Mirrors the dog.go / dog.js
// behavior: in particular it REJECTS truncated directive names (l:, di:, ...)
// as hard errors, because abbreviated directives are not in the spec
// (bag log D1). It does NOT copy dog.py's silent leniency.
//
// Library use:
//
//     let v: dog::Value = dog::parse(text)?;
//
// CLI use (mirrors dog.py):
//
//     rustc --edition 2021 -O dog.rs -o dog && ./dog file.dog
//
// Errors are returned as dog::DogError.
//
// This product was made by DOGS -- https://wearedogs.net

use std::collections::HashMap;
use std::fmt;

/* ------------------------------------------------------------------ */
/* value model                                                         */
/* ------------------------------------------------------------------ */

/// A JSON-equivalent value: object (insertion-ordered), array, string,
/// integer, float, boolean, or null.
#[derive(Clone, Debug)]
pub enum Value {
    Null,
    Bool(bool),
    Int(i64),
    Float(f64),
    Str(String),
    Array(Vec<Value>),
    Object(Vec<(String, Value)>),
}

/// Insert or replace (last duplicate wins, first position kept).
fn obj_set(o: &mut Vec<(String, Value)>, k: String, v: Value) {
    match o.iter_mut().find(|(ek, _)| ek == &k) {
        Some((_, e)) => *e = v,
        None => o.push((k, v)),
    }
}

/* ------------------------------------------------------------------ */
/* errors                                                              */
/* ------------------------------------------------------------------ */

/// Raised for any malformed .dog document.
#[derive(Clone, Debug)]
pub struct DogError(pub String);

impl fmt::Display for DogError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.0)
    }
}

impl std::error::Error for DogError {}

fn dog_err(msg: String) -> DogError {
    DogError(msg)
}

const MAGIC: &str = ".dog/1.0";

const KNOWN_DIRECTIVES: &[&str] = &["lang", "dict", "rep", "keys", "sep", "indent", "quote"];

const PLACEHOLDER: char = '\u{E000}'; // must match dog.py / dog.js / dog.go escape placeholder

/// JSON.stringify-ish quoting for error messages (matches dog.js output).
fn quote_js(s: &str) -> String {
    let mut b = String::from("\"");
    for r in s.chars() {
        match r {
            '"' => b.push_str("\\\""),
            '\\' => b.push_str("\\\\"),
            '\n' => b.push_str("\\n"),
            '\r' => b.push_str("\\r"),
            '\t' => b.push_str("\\t"),
            _ if (r as u32) < 0x20 => b.push_str(&format!("\\u{:04x}", r as u32)),
            _ => b.push(r),
        }
    }
    b.push('"');
    b
}

/* ------------------------------------------------------------------ */
/* header                                                              */
/* ------------------------------------------------------------------ */

fn split_lines(text: &str) -> Vec<String> {
    // mirrors dog.js: text.split(/\r\n|[\n\r\x85\u2028\u2029]/)
    let mut lines = Vec::new();
    let mut cur = String::new();
    let mut chars = text.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '\r' {
            if chars.peek() == Some(&'\n') {
                chars.next();
            }
            lines.push(std::mem::take(&mut cur));
        } else if c == '\n' || c == '\u{85}' || c == '\u{2028}' || c == '\u{2029}' {
            lines.push(std::mem::take(&mut cur));
        } else {
            cur.push(c);
        }
    }
    lines.push(cur);
    lines
}

fn split_header(text: &str) -> Result<(HashMap<String, String>, Vec<String>), DogError> {
    let text = text.strip_prefix('\u{FEFF}').unwrap_or(text);
    let lines = split_lines(text);
    if lines.is_empty() || lines[0].trim() != MAGIC {
        return Err(dog_err("first line must be the magic '.dog/1.0'".to_string()));
    }
    let mut directives: HashMap<String, String> = HashMap::new();
    let mut body_start = lines.len();
    for idx in 1..lines.len() {
        let raw = &lines[idx];
        if raw.trim().is_empty() {
            body_start = idx + 1;
            break;
        }
        let s = raw.trim();
        if s.starts_with('#') {
            continue;
        }
        let ci = match s.find(':') {
            Some(i) => i,
            None => {
                return Err(dog_err(format!(
                    "bad header directive on line {}: {}",
                    idx + 1,
                    quote_js(raw)
                )))
            }
        };
        let name = s[..ci].trim().to_lowercase();
        if name.is_empty()
            || name
                .chars()
                .any(|c| matches!(c, ' ' | '\t' | '\n' | '\r' | '\u{c}' | '\u{b}'))
        {
            return Err(dog_err(format!(
                "bad directive name on line {}: {}",
                idx + 1,
                quote_js(raw)
            )));
        }
        // Abbreviated directives are NOT in the spec (needs founder sign-off,
        // bag log D1). A truncated known-directive name is a hard error here,
        // never silently resolved -- and never confused with the unknown-
        // directive ignore rule, which only covers names that are not
        // truncations of known directives.
        for full in KNOWN_DIRECTIVES {
            if name != *full && full.starts_with(&name) {
                return Err(dog_err(format!(
                    "truncated directive '{}:' is not valid -- abbreviations are not in the spec (want '{}:')",
                    name, full
                )));
            }
        }
        directives.insert(name, s[ci + 1..].trim().to_string()); // last one wins
    }
    Ok((directives, lines[body_start..].to_vec()))
}

#[allow(dead_code)] // `indent` is advisory for writers; readers only validate it
struct Config {
    lang: String,
    dict: HashMap<String, String>,
    rep: Vec<(String, String)>, // longest token first
    keys: Option<Vec<String>>,  // positional key order for row-oriented bodies (None = off)
    sep: String,
    indent: i64,
    quote: String,
}

fn is_int_sign(s: &str) -> bool {
    // ^[+-]?\d+$
    let t = s
        .strip_prefix(|c| c == '+' || c == '-')
        .unwrap_or(s);
    !t.is_empty() && t.bytes().all(|b| b.is_ascii_digit())
}

fn build_config(directives: &HashMap<String, String>) -> Result<Config, DogError> {
    let lang = directives
        .get("lang")
        .map(|v| v.trim().to_lowercase())
        .unwrap_or_else(|| "dog".to_string());
    match lang.as_str() {
        "dog" | "json" | "xml" | "yaml" => {}
        _ => {
            let raw = directives.get("lang").cloned().unwrap_or_default();
            return Err(dog_err(format!(
                "unsupported lang: {} (want dog|json|xml|yaml)",
                quote_js(&raw)
            )));
        }
    }
    let mut dict: HashMap<String, String> = HashMap::new();
    if let Some(v) = directives.get("dict") {
        for pair in v.split_whitespace() {
            let ei = match pair.find('=') {
                Some(i) => i,
                None => {
                    return Err(dog_err(format!(
                        "bad dict pair {} (want alias=key)",
                        quote_js(pair)
                    )))
                }
            };
            let (alias, real) = (&pair[..ei], &pair[ei + 1..]);
            if alias.is_empty() || real.is_empty() {
                return Err(dog_err(format!("bad dict pair {}", quote_js(pair))));
            }
            dict.insert(alias.to_string(), real.to_string());
        }
    }
    let mut rep: Vec<(String, String)> = Vec::new();
    if let Some(v) = directives.get("rep") {
        for pair in v.split_whitespace() {
            let ei = match pair.find('=') {
                Some(i) => i,
                None => {
                    return Err(dog_err(format!(
                        "bad rep pair {} (want token=expansion)",
                        quote_js(pair)
                    )))
                }
            };
            let (tok, exp) = (&pair[..ei], &pair[ei + 1..]);
            if tok.is_empty() {
                return Err(dog_err(format!("empty rep token in {}", quote_js(pair))));
            }
            rep.push((tok.to_string(), exp.to_string()));
        }
        rep.sort_by(|a, b| b.0.len().cmp(&a.0.len())); // longest token wins (stable)
    }
    let mut keys: Option<Vec<String>> = None;
    if let Some(v) = directives.get("keys") {
        let ks: Vec<String> = v.split_whitespace().map(|s| s.to_string()).collect();
        if ks.is_empty() {
            return Err(dog_err("empty keys: directive".to_string()));
        }
        keys = Some(ks);
    }
    let sep = directives.get("sep").cloned().unwrap_or_else(|| "|".to_string());
    let mut indent: i64 = 2;
    if let Some(v) = directives.get("indent") {
        if !is_int_sign(v) {
            return Err(dog_err("indent: must be an integer".to_string()));
        }
        // mirror Go's ignored Atoi error: unparseable -> 0 -> the >= 1 error
        let n: i64 = v.parse().unwrap_or(0);
        if n < 1 {
            return Err(dog_err("indent: must be >= 1".to_string()));
        }
        indent = n;
    }
    let quote = directives
        .get("quote")
        .cloned()
        .unwrap_or_else(|| "\"".to_string());
    if sep.is_empty() {
        return Err(dog_err("sep: must not be empty".to_string()));
    }
    if quote.is_empty() {
        return Err(dog_err("quote: must not be empty".to_string()));
    }
    Ok(Config {
        lang,
        dict,
        rep,
        keys,
        sep,
        indent,
        quote,
    })
}

/* ------------------------------------------------------------------ */
/* expansion                                                           */
/* ------------------------------------------------------------------ */

fn expand_rep(s: &str, cfg: &Config) -> String {
    if cfg.rep.is_empty() {
        return s.to_string();
    }
    let mut saved: Vec<char> = Vec::new();
    let mut out: Vec<char> = Vec::new();
    let chars: Vec<char> = s.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        if chars[i] == '\\' && i + 1 < chars.len() {
            saved.push(chars[i + 1]);
            out.push(PLACEHOLDER);
            i += 2;
        } else {
            out.push(chars[i]);
            i += 1;
        }
    }
    let mut t: String = out.into_iter().collect();
    for (tok, exp) in &cfg.rep {
        t = t.replace(tok, exp);
    }
    let mut res = String::new();
    let mut si = 0;
    for c in t.chars() {
        if c == PLACEHOLDER {
            if si < saved.len() {
                res.push(saved[si]);
            }
            si += 1;
        } else {
            res.push(c);
        }
    }
    res
}

fn is_int_lit(s: &str) -> bool {
    // ^-?\d+$
    let t = s.strip_prefix('-').unwrap_or(s);
    !t.is_empty() && t.bytes().all(|b| b.is_ascii_digit())
}

fn is_float_lit(s: &str) -> bool {
    // ^-?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?$
    let t = s.strip_prefix('-').unwrap_or(s);
    let (mant, exp_ok) = match t.find(|c| c == 'e' || c == 'E') {
        Some(i) => {
            let mut e = &t[i + 1..];
            e = e.strip_prefix(|c| c == '+' || c == '-').unwrap_or(e);
            (&t[..i], !e.is_empty() && e.bytes().all(|b| b.is_ascii_digit()))
        }
        None => (t, true),
    };
    if !exp_ok {
        return false;
    }
    let b = mant.as_bytes();
    if b.is_empty() {
        return false;
    }
    if b.iter().all(|c| c.is_ascii_digit()) {
        return true;
    }
    match mant.find('.') {
        Some(di) => {
            let (a, c) = (&mant[..di], &mant[di + 1..]);
            if c.contains('.') {
                return false;
            }
            let a_ok = !a.is_empty() && a.bytes().all(|x| x.is_ascii_digit());
            let c_ok = c.bytes().all(|x| x.is_ascii_digit());
            (a_ok && c_ok) || (a.is_empty() && !c.is_empty() && c_ok)
        }
        None => false,
    }
}

fn parse_scalar(s: &str, cfg: &Config) -> Value {
    let s = expand_rep(s, cfg);
    let q = cfg.quote.as_str();
    if q.chars().count() == 1 {
        let qc = q.chars().next().unwrap();
        if s.len() >= 2 * qc.len_utf8() && s.starts_with(qc) && s.ends_with(qc) {
            let inner = &s[qc.len_utf8()..s.len() - qc.len_utf8()];
            let bs_q = format!("\\{}", qc);
            let un = inner.replace(&bs_q, q).replace("\\\\", "\\");
            return Value::Str(un);
        }
    }
    let low = s.to_lowercase();
    if low == "null" {
        return Value::Null;
    }
    if low == "true" {
        return Value::Bool(true);
    }
    if low == "false" {
        return Value::Bool(false);
    }
    if is_int_lit(&s) {
        if let Ok(n) = s.parse::<i64>() {
            return Value::Int(n);
        }
    }
    if is_float_lit(&s) {
        // mirror Go: ParseFloat overflow/underflow is an error there, so a
        // non-finite result falls through to a plain string
        if let Ok(f) = s.parse::<f64>() {
            if f.is_finite() {
                return Value::Float(f);
            }
        }
    }
    Value::Str(s)
}

fn default_config() -> Config {
    Config {
        lang: "dog".to_string(),
        dict: HashMap::new(),
        rep: Vec::new(),
        keys: None,
        sep: "|".to_string(),
        indent: 2,
        quote: "\"".to_string(),
    }
}

fn coerce_loose(s: &str) -> Value {
    parse_scalar(s, &default_config())
}

/* ------------------------------------------------------------------ */
/* JSON: minimal parser (for `lang: json` bodies) + serializer (CLI)   */
/* ------------------------------------------------------------------ */

struct JsonParser<'a> {
    s: &'a [u8],
    pos: usize,
}

impl<'a> JsonParser<'a> {
    fn skip_ws(&mut self) {
        while self.pos < self.s.len()
            && matches!(self.s[self.pos], b' ' | b'\t' | b'\n' | b'\r')
        {
            self.pos += 1;
        }
    }

    fn parse_value(&mut self) -> Result<Value, String> {
        self.skip_ws();
        if self.pos >= self.s.len() {
            return Err("unexpected end of input".to_string());
        }
        match self.s[self.pos] {
            b'{' => self.parse_object(),
            b'[' => self.parse_array(),
            b'"' => Ok(Value::Str(self.parse_string()?)),
            b't' => self.lit("true", Value::Bool(true)),
            b'f' => self.lit("false", Value::Bool(false)),
            b'n' => self.lit("null", Value::Null),
            b'-' | b'0'..=b'9' => self.parse_number(),
            c => Err(format!("unexpected character '{}'", c as char)),
        }
    }

    fn lit(&mut self, word: &str, v: Value) -> Result<Value, String> {
        let ok = self
            .s
            .get(self.pos..)
            .map_or(false, |r| r.starts_with(word.as_bytes()));
        if ok {
            self.pos += word.len();
            Ok(v)
        } else {
            Err(format!("bad literal at offset {}", self.pos))
        }
    }

    fn parse_object(&mut self) -> Result<Value, String> {
        self.pos += 1; // consume '{'
        let mut o: Vec<(String, Value)> = Vec::new();
        self.skip_ws();
        if self.pos < self.s.len() && self.s[self.pos] == b'}' {
            self.pos += 1;
            return Ok(Value::Object(o));
        }
        loop {
            self.skip_ws();
            if self.pos >= self.s.len() || self.s[self.pos] != b'"' {
                return Err("expected string key".to_string());
            }
            let k = self.parse_string()?;
            self.skip_ws();
            if self.pos >= self.s.len() || self.s[self.pos] != b':' {
                return Err("expected ':'".to_string());
            }
            self.pos += 1;
            let v = self.parse_value()?;
            obj_set(&mut o, k, v); // last duplicate wins
            self.skip_ws();
            if self.pos >= self.s.len() {
                return Err("unterminated object".to_string());
            }
            match self.s[self.pos] {
                b',' => self.pos += 1,
                b'}' => {
                    self.pos += 1;
                    return Ok(Value::Object(o));
                }
                _ => return Err("expected ',' or '}'".to_string()),
            }
        }
    }

    fn parse_array(&mut self) -> Result<Value, String> {
        self.pos += 1; // consume '['
        let mut a: Vec<Value> = Vec::new();
        self.skip_ws();
        if self.pos < self.s.len() && self.s[self.pos] == b']' {
            self.pos += 1;
            return Ok(Value::Array(a));
        }
        loop {
            let v = self.parse_value()?;
            a.push(v);
            self.skip_ws();
            if self.pos >= self.s.len() {
                return Err("unterminated array".to_string());
            }
            match self.s[self.pos] {
                b',' => self.pos += 1,
                b']' => {
                    self.pos += 1;
                    return Ok(Value::Array(a));
                }
                _ => return Err("expected ',' or ']'".to_string()),
            }
        }
    }

    fn parse_hex4(&mut self) -> Result<u32, String> {
        if self.pos + 4 > self.s.len() {
            return Err("bad \\u escape".to_string());
        }
        let mut v: u32 = 0;
        for i in 0..4 {
            let d = (self.s[self.pos + i] as char)
                .to_digit(16)
                .ok_or_else(|| "bad \\u escape".to_string())?;
            v = v * 16 + d;
        }
        self.pos += 4;
        Ok(v)
    }

    fn parse_string(&mut self) -> Result<String, String> {
        self.pos += 1; // consume opening '"'
        let mut out = String::new();
        loop {
            if self.pos >= self.s.len() {
                return Err("unterminated string".to_string());
            }
            match self.s[self.pos] {
                b'"' => {
                    self.pos += 1;
                    return Ok(out);
                }
                b'\\' => {
                    self.pos += 1;
                    if self.pos >= self.s.len() {
                        return Err("unterminated escape".to_string());
                    }
                    match self.s[self.pos] {
                        b'"' => {
                            out.push('"');
                            self.pos += 1;
                        }
                        b'\\' => {
                            out.push('\\');
                            self.pos += 1;
                        }
                        b'/' => {
                            out.push('/');
                            self.pos += 1;
                        }
                        b'b' => {
                            out.push('\u{8}');
                            self.pos += 1;
                        }
                        b'f' => {
                            out.push('\u{c}');
                            self.pos += 1;
                        }
                        b'n' => {
                            out.push('\n');
                            self.pos += 1;
                        }
                        b'r' => {
                            out.push('\r');
                            self.pos += 1;
                        }
                        b't' => {
                            out.push('\t');
                            self.pos += 1;
                        }
                        b'u' => {
                            self.pos += 1;
                            let cp = self.parse_hex4()?;
                            if (0xD800..0xDC00).contains(&cp) {
                                // high surrogate: needs a low surrogate next
                                if self.s.get(self.pos..self.pos + 2) == Some(&b"\\u"[..]) {
                                    let save = self.pos;
                                    self.pos += 2;
                                    match self.parse_hex4() {
                                        Ok(lo) if (0xDC00..0xE000).contains(&lo) => {
                                            let c = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                            out.push(char::from_u32(c).unwrap());
                                        }
                                        _ => {
                                            // not a pair: replacement char, then
                                            // re-scan the \uXXXX normally
                                            self.pos = save;
                                            out.push('\u{FFFD}');
                                        }
                                    }
                                } else {
                                    out.push('\u{FFFD}');
                                }
                            } else if (0xDC00..0xE000).contains(&cp) {
                                out.push('\u{FFFD}');
                            } else {
                                out.push(char::from_u32(cp).unwrap_or('\u{FFFD}'));
                            }
                        }
                        _ => return Err("bad escape".to_string()),
                    }
                }
                0x00..=0x1F => return Err("unescaped control character in string".to_string()),
                _ => {
                    let rest = std::str::from_utf8(&self.s[self.pos..])
                        .map_err(|e| e.to_string())?;
                    let ch = rest.chars().next().unwrap();
                    out.push(ch);
                    self.pos += ch.len_utf8();
                }
            }
        }
    }

    fn parse_number(&mut self) -> Result<Value, String> {
        let start = self.pos;
        if self.s[self.pos] == b'-' {
            self.pos += 1;
        }
        if self.pos >= self.s.len() {
            return Err("bad number".to_string());
        }
        if self.s[self.pos] == b'0' {
            self.pos += 1;
        } else if self.s[self.pos].is_ascii_digit() {
            while self.pos < self.s.len() && self.s[self.pos].is_ascii_digit() {
                self.pos += 1;
            }
        } else {
            return Err("bad number".to_string());
        }
        if self.pos < self.s.len() && self.s[self.pos] == b'.' {
            self.pos += 1;
            let fs = self.pos;
            while self.pos < self.s.len() && self.s[self.pos].is_ascii_digit() {
                self.pos += 1;
            }
            if self.pos == fs {
                return Err("bad number".to_string());
            }
        }
        if self.pos < self.s.len() && (self.s[self.pos] == b'e' || self.s[self.pos] == b'E') {
            self.pos += 1;
            if self.pos < self.s.len()
                && (self.s[self.pos] == b'+' || self.s[self.pos] == b'-')
            {
                self.pos += 1;
            }
            let es = self.pos;
            while self.pos < self.s.len() && self.s[self.pos].is_ascii_digit() {
                self.pos += 1;
            }
            if self.pos == es {
                return Err("bad number".to_string());
            }
        }
        let lit = std::str::from_utf8(&self.s[start..self.pos]).unwrap();
        if is_int_lit(lit) {
            if let Ok(n) = lit.parse::<i64>() {
                return Ok(Value::Int(n));
            }
        }
        lit.parse::<f64>()
            .map(Value::Float)
            .map_err(|_| format!("bad number {}", lit))
    }
}

/// Parse a JSON text into a Value. Public so the conformance runner (and
/// embedders) can reuse it for `.expected.json` files.
pub fn parse_json(text: &str) -> Result<Value, String> {
    let mut p = JsonParser {
        s: text.as_bytes(),
        pos: 0,
    };
    let v = p.parse_value()?;
    p.skip_ws();
    if p.pos != p.s.len() {
        return Err("unexpected trailing data".to_string());
    }
    Ok(v)
}

fn parse_json_body(body_lines: &[String]) -> Result<Value, DogError> {
    let text = body_lines.join("\n");
    let t = text.trim();
    if t.is_empty() {
        return Ok(Value::Null);
    }
    let mut p = JsonParser {
        s: t.as_bytes(),
        pos: 0,
    };
    let v = p
        .parse_value()
        .map_err(|e| dog_err(format!("bad JSON body: {}", e)))?;
    p.skip_ws();
    if p.pos != p.s.len() {
        return Err(dog_err("bad JSON body: unexpected trailing data".to_string()));
    }
    Ok(v)
}

/// JSON string escaping for the CLI (Go encoding/json style with HTML
/// escaping off: no <>& escaping, DEL left raw).
#[cfg_attr(test, allow(dead_code))]
fn json_escape_into(s: &str, out: &mut String) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            _ if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            _ => out.push(c),
        }
    }
    out.push('"');
}

/// Pretty-print a Value as JSON with 2-space indent (mirrors Go's
/// json.Encoder with SetIndent("", "  ")).
#[cfg_attr(test, allow(dead_code))]
fn write_json(v: &Value, indent: usize, out: &mut String) {
    match v {
        Value::Null => out.push_str("null"),
        Value::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Value::Int(n) => out.push_str(&n.to_string()),
        Value::Float(f) => {
            if f.is_finite() && f.fract() == 0.0 && f.abs() < 1e21 {
                out.push_str(&(*f as i64).to_string());
            } else {
                out.push_str(&format!("{}", f));
            }
        }
        Value::Str(s) => json_escape_into(s, out),
        Value::Array(a) => {
            if a.is_empty() {
                out.push_str("[]");
                return;
            }
            out.push_str("[\n");
            for (i, e) in a.iter().enumerate() {
                out.push_str(&"  ".repeat(indent + 1));
                write_json(e, indent + 1, out);
                if i + 1 < a.len() {
                    out.push(',');
                }
                out.push('\n');
            }
            out.push_str(&"  ".repeat(indent));
            out.push(']');
        }
        Value::Object(o) => {
            if o.is_empty() {
                out.push_str("{}");
                return;
            }
            out.push_str("{\n");
            for (i, (k, e)) in o.iter().enumerate() {
                out.push_str(&"  ".repeat(indent + 1));
                json_escape_into(k, out);
                out.push_str(": ");
                write_json(e, indent + 1, out);
                if i + 1 < o.len() {
                    out.push(',');
                }
                out.push('\n');
            }
            out.push_str(&"  ".repeat(indent));
            out.push('}');
        }
    }
}

/* ------------------------------------------------------------------ */
/* XML: minimal parser for `lang: xml` bodies (byte-identical source)  */
/* ------------------------------------------------------------------ */

struct XmlNode {
    tag: String,
    attrs: Vec<(String, String)>,
    children: Vec<XmlNode>,
    text: String,
}

struct XmlParser<'a> {
    s: &'a [u8],
    pos: usize,
}

fn str_from(b: &[u8]) -> &str {
    // all call sites slice at ASCII boundaries
    std::str::from_utf8(b).unwrap()
}

fn find_from(hay: &[u8], from: usize, needle: &[u8]) -> Option<usize> {
    if from > hay.len() {
        return None;
    }
    hay[from..]
        .windows(needle.len())
        .position(|w| w == needle)
        .map(|p| from + p)
}

fn unescape_xml(s: &str) -> Result<String, DogError> {
    // mirrors Go's regexp replace: &(#\d+|#[xX][0-9a-fA-F]+|[a-zA-Z]+);
    // the LAST bad entity wins the error, valid entities still decode
    let mut out = String::new();
    let mut err: Option<DogError> = None;
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() {
        if b[i] != b'&' {
            let ch = str_from(&b[i..]).chars().next().unwrap();
            out.push(ch);
            i += ch.len_utf8();
            continue;
        }
        let mut j = i + 1;
        let is_num = j < b.len() && b[j] == b'#';
        if is_num {
            j += 1;
        }
        let hex = is_num && j < b.len() && (b[j] == b'x' || b[j] == b'X');
        if hex {
            j += 1;
        }
        let ds = j;
        while j < b.len()
            && (if hex {
                (b[j] as char).is_ascii_hexdigit()
            } else if is_num {
                (b[j] as char).is_ascii_digit()
            } else {
                (b[j] as char).is_ascii_alphabetic()
            })
        {
            j += 1;
        }
        let mut decoded = false;
        if j > ds && j < b.len() && b[j] == b';' {
            let body = &s[i + 1..j];
            let m = &s[i..=j];
            let rep: Option<char> = match body {
                "amp" => Some('&'),
                "lt" => Some('<'),
                "gt" => Some('>'),
                "quot" => Some('"'),
                "apos" => Some('\''),
                _ if is_num => {
                    let digits = if hex { &body[2..] } else { &body[1..] };
                    match i64::from_str_radix(digits, if hex { 16 } else { 10 }) {
                        Ok(code)
                            if code >= 0
                                && code <= 0x10FFFF
                                && !(0xD800..=0xDFFF).contains(&code) =>
                        {
                            char::from_u32(code as u32)
                        }
                        _ => None,
                    }
                }
                _ => None,
            };
            match rep {
                Some(c) => {
                    out.push(c);
                    i = j + 1;
                    decoded = true;
                }
                None => {
                    let kind = if is_num {
                        "bad character reference"
                    } else {
                        "unknown entity"
                    };
                    err = Some(dog_err(format!("bad XML body: {} {}", kind, m)));
                }
            }
        }
        if !decoded {
            out.push('&');
            i += 1;
        }
    }
    if let Some(e) = err {
        return Err(e);
    }
    Ok(out)
}

fn is_xml_space(b: u8) -> bool {
    matches!(b, b' ' | b'\t' | b'\n' | b'\r' | 0x0C | 0x0B)
}

impl<'a> XmlParser<'a> {
    fn fail(&self, msg: &str) -> DogError {
        dog_err(format!("bad XML body: {}", msg))
    }

    fn starts_with(&self, pat: &[u8]) -> bool {
        self.s.len() - self.pos >= pat.len()
            && self.s[self.pos..self.pos + pat.len()] == *pat
    }

    fn skip_ws(&mut self) {
        while self.pos < self.s.len() && is_xml_space(self.s[self.pos]) {
            self.pos += 1;
        }
    }

    fn read_name(&mut self) -> Result<String, DogError> {
        let start = self.pos;
        while self.pos < self.s.len()
            && !is_xml_space(self.s[self.pos])
            && self.s[self.pos] != b'/'
            && self.s[self.pos] != b'>'
            && self.s[self.pos] != b'='
        {
            self.pos += 1;
        }
        if self.pos == start {
            return Err(self.fail(&format!("expected a name at offset {}", self.pos)));
        }
        Ok(str_from(&self.s[start..self.pos]).to_string())
    }

    fn skip_misc(&mut self) -> Result<(), DogError> {
        loop {
            if self.starts_with(b"<!--") {
                match find_from(self.s, self.pos + 4, b"-->") {
                    Some(e) => self.pos = e + 3,
                    None => return Err(self.fail("unterminated comment")),
                }
            } else if self.starts_with(b"<?") {
                match find_from(self.s, self.pos + 2, b"?>") {
                    Some(e) => self.pos = e + 2,
                    None => return Err(self.fail("unterminated processing instruction")),
                }
            } else if self.starts_with(b"<!") {
                let mut d = self.pos + 2;
                let mut depth: i32 = 0;
                while d < self.s.len() {
                    if self.s[d] == b'>' && depth == 0 {
                        break;
                    }
                    if self.s[d] == b'[' {
                        depth += 1;
                    }
                    if self.s[d] == b']' {
                        depth -= 1;
                    }
                    d += 1;
                }
                if d >= self.s.len() {
                    return Err(self.fail("unterminated declaration"));
                }
                self.pos = d + 1;
            } else {
                return Ok(());
            }
        }
    }

    fn parse_element(&mut self) -> Result<XmlNode, DogError> {
        self.pos += 1; // consume '<'
        let name = self.read_name()?;
        let mut attrs: Vec<(String, String)> = Vec::new();
        loop {
            self.skip_ws();
            if self.starts_with(b"/>") {
                self.pos += 2;
                return Ok(XmlNode {
                    tag: name,
                    attrs,
                    children: Vec::new(),
                    text: String::new(),
                });
            }
            if self.pos < self.s.len() && self.s[self.pos] == b'>' {
                self.pos += 1;
                break;
            }
            if self.pos >= self.s.len() {
                return Err(self.fail(&format!("unterminated start tag <{}>", name)));
            }
            let an = self.read_name()?;
            self.skip_ws();
            if self.pos >= self.s.len() || self.s[self.pos] != b'=' {
                return Err(self.fail(&format!("expected '=' after attribute {}", an)));
            }
            self.pos += 1;
            self.skip_ws();
            if self.pos >= self.s.len()
                || (self.s[self.pos] != b'"' && self.s[self.pos] != b'\'')
            {
                return Err(self.fail("attribute value must be quoted"));
            }
            let q = self.s[self.pos];
            self.pos += 1;
            let start = self.pos;
            while self.pos < self.s.len() && self.s[self.pos] != q {
                self.pos += 1;
            }
            if self.pos >= self.s.len() {
                return Err(self.fail("unterminated attribute value"));
            }
            let v = str_from(&self.s[start..self.pos]).to_string();
            self.pos += 1;
            let uv = unescape_xml(&v)?;
            match attrs.iter_mut().find(|(k, _)| k == &an) {
                Some((_, e)) => *e = uv,
                None => attrs.push((an, uv)),
            }
        }
        let mut children: Vec<XmlNode> = Vec::new();
        let mut text_acc = String::new(); // leading text before the first child (mirror ET's .text)
        let mut seen_child = false;
        loop {
            if self.pos >= self.s.len() {
                return Err(self.fail(&format!("unterminated element <{}>", name)));
            }
            if self.s[self.pos] == b'<' {
                if self.starts_with(b"</") {
                    self.pos += 2;
                    self.skip_ws();
                    let cn = self.read_name()?;
                    self.skip_ws();
                    if self.pos >= self.s.len() || self.s[self.pos] != b'>' {
                        return Err(self.fail("expected '>' in closing tag"));
                    }
                    self.pos += 1;
                    if cn != name {
                        return Err(self.fail(&format!(
                            "mismatched close tag </{}> for <{}>",
                            cn, name
                        )));
                    }
                    break;
                } else if self.starts_with(b"<!--") {
                    match find_from(self.s, self.pos + 4, b"-->") {
                        Some(e) => self.pos = e + 3,
                        None => return Err(self.fail("unterminated comment")),
                    }
                } else if self.starts_with(b"<?") {
                    match find_from(self.s, self.pos + 2, b"?>") {
                        Some(e) => self.pos = e + 2,
                        None => return Err(self.fail("unterminated processing instruction")),
                    }
                } else if self.starts_with(b"<![CDATA[") {
                    match find_from(self.s, self.pos + 9, b"]]>") {
                        Some(e) => {
                            if !seen_child {
                                text_acc.push_str(str_from(&self.s[self.pos + 9..e]));
                            }
                            self.pos = e + 3;
                        }
                        None => return Err(self.fail("unterminated CDATA section")),
                    }
                } else if self.starts_with(b"<!") {
                    return Err(self.fail("unexpected declaration inside element"));
                } else {
                    let child = self.parse_element()?;
                    children.push(child);
                    seen_child = true;
                }
            } else {
                let end = find_from(self.s, self.pos, b"<").unwrap_or(self.s.len());
                if !seen_child {
                    text_acc.push_str(str_from(&self.s[self.pos..end]));
                } // tail after children is ignored, like ET
                self.pos = end;
            }
        }
        Ok(XmlNode {
            tag: name,
            attrs,
            children,
            text: text_acc,
        })
    }
}

fn xml_value(node: &XmlNode) -> Result<Value, DogError> {
    let mut d: Vec<(String, Value)> = Vec::new();
    for (k, v) in &node.attrs {
        d.push((format!("@{}", k), Value::Str(v.clone())));
    }
    if node.children.is_empty() {
        let t = unescape_xml(&node.text)?.trim().to_string();
        if !node.attrs.is_empty() {
            if !t.is_empty() {
                d.push(("#text".to_string(), Value::Str(t)));
            }
            return Ok(Value::Object(d));
        }
        return Ok(coerce_loose(&t));
    }
    for child in &node.children {
        let v = xml_value(child)?;
        match d.iter_mut().find(|(k, _)| k == &child.tag) {
            Some((_, existing)) => match existing {
                Value::Array(arr) => arr.push(v),
                _ => {
                    let old = std::mem::replace(existing, Value::Null);
                    *existing = Value::Array(vec![old, v]);
                }
            },
            None => d.push((child.tag.clone(), v)),
        }
    }
    Ok(Value::Object(d))
}

fn parse_xml_body(body_lines: &[String]) -> Result<Value, DogError> {
    let text = body_lines.join("\n");
    let t = text.trim();
    if t.is_empty() {
        return Ok(Value::Null);
    }
    let mut p = XmlParser {
        s: t.as_bytes(),
        pos: 0,
    };
    p.skip_ws();
    p.skip_misc()?;
    p.skip_ws();
    if p.pos >= p.s.len() || p.s[p.pos] != b'<' {
        return Err(p.fail("no root element"));
    }
    let root = p.parse_element()?;
    p.skip_ws();
    p.skip_misc()?;
    p.skip_ws();
    if p.pos != p.s.len() {
        return Err(p.fail("junk after root element"));
    }
    let rv = xml_value(&root)?;
    Ok(Value::Object(vec![(root.tag.clone(), rv)]))
}

/* ------------------------------------------------------------------ */
/* native `dog` body: indentation-based maps / lists / scalars / rows  */
/* ------------------------------------------------------------------ */

struct Item {
    indent: usize,
    text: String,
    raw: String,
}

fn indent_of(raw: &str) -> Result<usize, DogError> {
    let mut i = 0;
    for c in raw.chars() {
        if c == ' ' {
            i += 1;
        } else {
            break;
        }
    }
    if raw[i..].starts_with('\t') {
        return Err(dog_err(format!(
            "tabs are not allowed for indentation: {}",
            quote_js(raw)
        )));
    }
    Ok(i)
}

fn body_items(body_lines: &[String]) -> Result<Vec<Item>, DogError> {
    let mut items = Vec::new();
    for raw in body_lines {
        let ind = indent_of(raw)?; // may throw on tabs -- same order as dog.py
        if raw.trim().is_empty() {
            continue;
        }
        if raw.trim_start_matches(' ').starts_with('#') {
            continue;
        }
        items.push(Item {
            indent: ind,
            text: raw.trim().to_string(),
            raw: raw.clone(),
        });
    }
    Ok(items)
}

/// `^([^\s:]+)\s*:(?:[ \t]+(.*))?$` -- key, then optional value.
fn match_pair(text: &str) -> Option<(String, Option<String>)> {
    let mut key_end: Option<usize> = None;
    for (bi, c) in text.char_indices() {
        if c == ':' || c.is_whitespace() {
            key_end = Some(bi);
            break;
        }
    }
    let ke = key_end?;
    if ke == 0 {
        return None;
    }
    let key = text[..ke].to_string();
    // skip \s*
    let mut k = 0;
    for (bi, c) in text[ke..].char_indices() {
        if c.is_whitespace() {
            k = bi + c.len_utf8();
        } else {
            break;
        }
    }
    let mut rest = &text[ke + k..];
    if !rest.starts_with(':') {
        return None;
    }
    rest = &rest[1..];
    // (?:[ \t]+(.*))?$
    if rest.starts_with(' ') || rest.starts_with('\t') {
        let mut m = 0;
        for (bi, c) in rest.char_indices() {
            if c == ' ' || c == '\t' {
                m = bi + 1;
            } else {
                break;
            }
        }
        Some((key, Some(rest[m..].to_string())))
    } else {
        Some((key, None))
    }
}

fn parse_block_scalar(
    items: &[Item],
    mut i: usize,
    level: usize,
) -> Result<(String, usize), DogError> {
    if i >= items.len() || items[i].indent <= level {
        return Err(dog_err("expected an indented block after `|`".to_string()));
    }
    let base = items[i].indent;
    let mut buf: Vec<String> = Vec::new();
    while i < items.len() && items[i].indent >= base {
        let raw = &items[i].raw;
        buf.push(if raw.len() >= base {
            raw[base..].to_string()
        } else {
            String::new()
        });
        i += 1;
    }
    Ok((buf.join("\n"), i))
}

fn dict_get<'a>(dict: &'a HashMap<String, String>, key: &'a str) -> &'a str {
    dict.get(key).map(|s| s.as_str()).unwrap_or(key)
}

fn parse_map(
    items: &[Item],
    mut i: usize,
    level: usize,
    cfg: &Config,
    seed: Option<(String, Value)>,
) -> Result<(Value, usize), DogError> {
    let mut d: Vec<(String, Value)> = Vec::new();
    if let Some((k, v)) = seed {
        d.push((k, v));
    }
    while i < items.len() {
        let it = &items[i];
        if it.indent < level {
            break;
        }
        if it.indent != level {
            return Err(dog_err(format!(
                "bad indentation in map at {}",
                quote_js(&it.text)
            )));
        }
        let (k1, val) = match_pair(&it.text).ok_or_else(|| {
            dog_err(format!(
                "expected `key: value` in map, got {}",
                quote_js(&it.text)
            ))
        })?;
        let key = expand_rep(dict_get(&cfg.dict, &k1), cfg);
        i += 1;
        let v = match val.as_deref() {
            None | Some("") => {
                if i < items.len() && items[i].indent > level {
                    let (vv, ni) = parse_value(items, i, items[i].indent, cfg)?;
                    i = ni;
                    vv
                } else {
                    Value::Null
                }
            }
            Some("|") => {
                let (b, ni) = parse_block_scalar(items, i, level)?;
                i = ni;
                Value::Str(b)
            }
            Some(s) => parse_scalar(s, cfg),
        };
        obj_set(&mut d, key, v); // last duplicate wins
    }
    Ok((Value::Object(d), i))
}

fn parse_list(
    items: &[Item],
    mut i: usize,
    level: usize,
    cfg: &Config,
) -> Result<(Value, usize), DogError> {
    let mut out: Vec<Value> = Vec::new();
    while i < items.len() {
        let it = &items[i];
        if it.indent < level {
            break;
        }
        if it.indent != level {
            return Err(dog_err(format!(
                "bad indentation in list at {}",
                quote_js(&it.text)
            )));
        }
        if !(it.text == "-" || it.text.starts_with("- ")) {
            break;
        }
        let rest = it.text[1..].trim().to_string();
        i += 1;
        if rest.is_empty() {
            let v = if i < items.len() && items[i].indent > level {
                let (vv, ni) = parse_value(items, i, items[i].indent, cfg)?;
                i = ni;
                vv
            } else {
                Value::Null
            };
            out.push(v);
        } else if let Some((k1, val)) = match_pair(&rest) {
            let key = expand_rep(dict_get(&cfg.dict, &k1), cfg);
            match val.as_deref() {
                None | Some("") => {
                    // `- key:` -- deeper block is the VALUE of key (YAML rule)
                    let v = if i < items.len() && items[i].indent > level {
                        let (vv, ni) = parse_value(items, i, items[i].indent, cfg)?;
                        i = ni;
                        vv
                    } else {
                        Value::Null
                    };
                    out.push(Value::Object(vec![(key, v)]));
                }
                Some("|") => {
                    let (b, ni) = parse_block_scalar(items, i, level)?;
                    i = ni;
                    out.push(Value::Object(vec![(key, Value::Str(b))]));
                }
                Some(s) => {
                    let seed_val = parse_scalar(s, cfg);
                    if i < items.len() && items[i].indent > level {
                        let (d, ni) =
                            parse_map(items, i, items[i].indent, cfg, Some((key, seed_val)))?;
                        out.push(d);
                        i = ni;
                    } else {
                        out.push(Value::Object(vec![(key, seed_val)]));
                    }
                }
            }
        } else {
            out.push(parse_scalar(&rest, cfg));
        }
    }
    Ok((Value::Array(out), i))
}

fn parse_value(
    items: &[Item],
    i: usize,
    level: usize,
    cfg: &Config,
) -> Result<(Value, usize), DogError> {
    let it = &items[i];
    if it.indent != level {
        return Err(dog_err(format!(
            "bad indentation at {}",
            quote_js(&it.text)
        )));
    }
    if it.text == "-" || it.text.starts_with("- ") {
        return parse_list(items, i, level, cfg);
    }
    if match_pair(&it.text).is_some() {
        return parse_map(items, i, level, cfg, None);
    }
    Ok((parse_scalar(&it.text, cfg), i + 1))
}

fn parse_rows(items: &[Item], cfg: &Config) -> Result<Value, DogError> {
    // aliases from dict: are allowed in keys: -- dict expands, rep does not
    let keys: Vec<String> = cfg.keys.as_ref().unwrap().iter().map(|k| dict_get(&cfg.dict, k).to_string()).collect();
    let sep = cfg.sep.as_str();
    let mut out: Vec<Value> = Vec::new();
    for it in items {
        if it.indent != 0 {
            return Err(dog_err(format!(
                "positional rows must not be indented: {}",
                quote_js(&it.text)
            )));
        }
        let fields: Vec<&str> = it.text.split(sep).collect();
        if fields.len() != keys.len() {
            return Err(dog_err(format!(
                "row has {} fields but keys: declares {}: {}",
                fields.len(),
                keys.len(),
                quote_js(&it.text)
            )));
        }
        let mut d: Vec<(String, Value)> = Vec::new();
        for (f, k) in keys.iter().enumerate() {
            let fv = fields[f].trim();
            d.push((
                k.clone(),
                if fv.is_empty() {
                    Value::Null
                } else {
                    parse_scalar(fv, cfg)
                },
            ));
        }
        out.push(Value::Object(d));
    }
    Ok(Value::Array(out))
}

fn parse_dog_body(body_lines: &[String], cfg: &Config) -> Result<Value, DogError> {
    let items = body_items(body_lines)?;
    if items.is_empty() {
        return Ok(Value::Null);
    }
    if cfg.keys.is_some() {
        return parse_rows(&items, cfg);
    }
    let (v, nxt) = parse_value(&items, 0, 0, cfg)?;
    if nxt != items.len() {
        return Err(dog_err(format!(
            "unexpected trailing content: {}",
            quote_js(&items[nxt].text)
        )));
    }
    if items[0].indent != 0 {
        return Err(dog_err("top-level content must not be indented".to_string()));
    }
    Ok(v)
}

/* ------------------------------------------------------------------ */
/* top-level                                                           */
/* ------------------------------------------------------------------ */

/// Parse a .dog document and return the equivalent JSON-able value:
/// Object, Array, Str, Int, Float, Bool, or Null.
pub fn parse(text: &str) -> Result<Value, DogError> {
    let (directives, body) = split_header(text)?;
    let mut cfg = build_config(&directives)?;
    match cfg.lang.as_str() {
        "json" => parse_json_body(&body),
        "xml" => parse_xml_body(&body),
        "yaml" => {
            // header tools are native-dog features; YAML bodies stay byte-identical
            cfg.dict.clear();
            cfg.rep.clear();
            cfg.keys = None;
            parse_dog_body(&body, &cfg) // common block-mapping subset
        }
        _ => parse_dog_body(&body, &cfg),
    }
}

/* ------------------------------------------------------------------ */
/* CLI (mirrors dog.py's main)                                         */
/* ------------------------------------------------------------------ */

#[cfg_attr(test, allow(dead_code))]
fn main() {
    let argv: Vec<String> = std::env::args().collect();
    if argv.len() != 2 || argv[1] == "-h" || argv[1] == "--help" {
        eprintln!("usage: {} file.dog", argv[0]);
        std::process::exit(2);
    }
    let bytes = match std::fs::read(&argv[1]) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("dog: cannot read {}: {}", argv[1], e);
            std::process::exit(2);
        }
    };
    let text = String::from_utf8_lossy(&bytes);
    match parse(&text) {
        Ok(v) => {
            let mut out = String::new();
            write_json(&v, 0, &mut out);
            println!("{}", out);
        }
        Err(e) => {
            eprintln!("dog error: {}", e);
            std::process::exit(1);
        }
    }
}
