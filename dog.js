/*
 * dog.js -- parser for the .dog text data format (DOG/1, magic `.dog/1.0`).
 *
 * Single-file, zero dependencies. Runs in node and in browsers (no fs,
 * no require, no DOM). Mirrors the reference parser dog.py.
 *
 * Usage (node, CommonJS):  const dog = require("./dog.js");
 *                          dog.parse(text) -> JSON-equivalent value
 * Usage (browser):         <script src="dog.js"></script> -> globalThis.dog
 *
 * Errors throw dog.DogError.
 *
 * This product was made by DOGS -- https://wearedogs.net
 */
(function () {
  "use strict";

  var MAGIC = ".dog/1.0";
  var KNOWN_DIRECTIVES = ["lang", "dict", "rep", "keys", "sep", "indent", "quote"];
  var PLACEHOLDER = ""; // U+E000 -- must match dog.py's escape placeholder

  function DogError(message) {
    this.name = "DogError";
    this.message = message || "";
    if (Error.captureStackTrace) Error.captureStackTrace(this, DogError);
  }
  DogError.prototype = Object.create(Error.prototype);
  DogError.prototype.constructor = DogError;

  function hasOwn(obj, key) {
    return Object.prototype.hasOwnProperty.call(obj, key);
  }

  // Plain dicts without a prototype, so keys like "__proto__" behave like
  // Python dict keys instead of mutating the prototype chain.
  function newObj() {
    return Object.create(null);
  }

  function splitWs(s) {
    return s.split(/\s+/).filter(function (p) { return p !== ""; });
  }

  /* ------------------------------------------------------------------ */
  /* header                                                              */
  /* ------------------------------------------------------------------ */

  function splitHeader(text) {
    text = text.replace(/^﻿/, "");
    var lines = text.split(/\r\n|[\n\r\x85\u2028\u2029]/);
    if (lines.length === 0 || lines[0].trim() !== MAGIC) {
      throw new DogError("first line must be the magic '.dog/1.0'");
    }
    var directives = newObj();
    var bodyStart = lines.length;
    for (var idx = 1; idx < lines.length; idx++) {
      var raw = lines[idx];
      if (raw.trim() === "") { bodyStart = idx + 1; break; }
      var s = raw.trim();
      if (s.charAt(0) === "#") continue;
      var ci = s.indexOf(":");
      if (ci < 0) {
        throw new DogError("bad header directive on line " + (idx + 1) +
                           ": " + JSON.stringify(raw));
      }
      var name = s.slice(0, ci).trim().toLowerCase();
      if (!name || /\s/.test(name)) {
        throw new DogError("bad directive name on line " + (idx + 1) +
                           ": " + JSON.stringify(raw));
      }
      // Abbreviated directives are NOT in the spec (needs founder sign-off,
      // bag log D1). A truncated known-directive name is a hard error here,
      // never silently resolved -- and never confused with the unknown-
      // directive ignore rule, which only covers names that are not
      // truncations of known directives.
      for (var k = 0; k < KNOWN_DIRECTIVES.length; k++) {
        var full = KNOWN_DIRECTIVES[k];
        if (name !== full && full.indexOf(name) === 0) {
          throw new DogError("truncated directive '" + name +
            ":' is not valid -- abbreviations are not in the spec (want '" +
            full + ":')");
        }
      }
      directives[name] = s.slice(ci + 1).trim(); // last one wins
    }
    return { directives: directives, body: lines.slice(bodyStart) };
  }

  function buildConfig(directives) {
    var cfg = {
      lang: (directives.lang !== undefined ? directives.lang : "dog").trim().toLowerCase(),
      dict: newObj(),
      rep: [],          // [token, expansion] pairs, longest token first
      keys: null,       // positional key order for row-oriented bodies
      sep: directives.sep !== undefined ? directives.sep : "|",
      indent: 2,
      quote: directives.quote !== undefined ? directives.quote : '"'
    };
    if (["dog", "json", "xml", "yaml"].indexOf(cfg.lang) < 0) {
      throw new DogError("unsupported lang: " + JSON.stringify(directives.lang) +
                         " (want dog|json|xml|yaml)");
    }
    if (directives.dict !== undefined) {
      splitWs(directives.dict).forEach(function (pair) {
        var ei = pair.indexOf("=");
        if (ei < 0) {
          throw new DogError("bad dict pair " + JSON.stringify(pair) +
                             " (want alias=key)");
        }
        var alias = pair.slice(0, ei), real = pair.slice(ei + 1);
        if (!alias || !real) throw new DogError("bad dict pair " + JSON.stringify(pair));
        cfg.dict[alias] = real;
      });
    }
    if (directives.rep !== undefined) {
      var pairs = [];
      splitWs(directives.rep).forEach(function (pair) {
        var ei = pair.indexOf("=");
        if (ei < 0) {
          throw new DogError("bad rep pair " + JSON.stringify(pair) +
                             " (want token=expansion)");
        }
        var tok = pair.slice(0, ei), exp = pair.slice(ei + 1);
        if (!tok) throw new DogError("empty rep token in " + JSON.stringify(pair));
        pairs.push([tok, exp]);
      });
      pairs.sort(function (a, b) { return b[0].length - a[0].length; }); // longest token wins
      cfg.rep = pairs;
    }
    if (directives.keys !== undefined) {
      var keys = splitWs(directives.keys);
      if (!keys.length) throw new DogError("empty keys: directive");
      cfg.keys = keys;
    }
    if (directives.indent !== undefined) {
      if (!/^[+-]?\d+$/.test(directives.indent)) {
        throw new DogError("indent: must be an integer");
      }
      var n = parseInt(directives.indent, 10);
      if (n < 1) throw new DogError("indent: must be >= 1");
      cfg.indent = n;
    }
    if (!cfg.sep) throw new DogError("sep: must not be empty");
    if (!cfg.quote) throw new DogError("quote: must not be empty");
    return cfg;
  }

  /* ------------------------------------------------------------------ */
  /* expansion                                                           */
  /* ------------------------------------------------------------------ */

  function expandRep(s, cfg) {
    if (!cfg.rep.length) return s;
    var saved = [];
    var out = [];
    var i = 0, n = s.length;
    while (i < n) {
      var c = s.charAt(i);
      if (c === "\\" && i + 1 < n) {
        saved.push(s.charAt(i + 1));
        out.push(PLACEHOLDER);
        i += 2;
      } else {
        out.push(c);
        i += 1;
      }
    }
    s = out.join("");
    for (var r = 0; r < cfg.rep.length; r++) {
      s = s.split(cfg.rep[r][0]).join(cfg.rep[r][1]);
    }
    var res = [];
    var si = 0;
    for (var j = 0; j < s.length; j++) {
      if (s.charAt(j) === PLACEHOLDER) { res.push(saved[si]); si += 1; }
      else res.push(s.charAt(j));
    }
    return res.join("");
  }

  function parseScalar(s, cfg) {
    s = expandRep(s, cfg);
    var q = cfg.quote;
    if (s.length >= 2 && s.charAt(0) === q && s.charAt(s.length - 1) === q) {
      var inner = s.slice(1, -1);
      return inner.split("\\" + q).join(q).split("\\\\").join("\\");
    }
    var low = s.toLowerCase();
    if (low === "null") return null;
    if (low === "true") return true;
    if (low === "false") return false;
    if (/^-?\d+$/.test(s)) return parseInt(s, 10);
    if (/^-?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?$/.test(s)) return parseFloat(s);
    return s;
  }

  function coerceLoose(s) {
    return parseScalar(s, { rep: [], quote: '"' });
  }

  /* ------------------------------------------------------------------ */
  /* native `dog` body: indentation-based maps / lists / scalars / rows  */
  /* ------------------------------------------------------------------ */

  var PAIR_RE = /^([^\s:]+)\s*:(?:[ \t]+(.*))?$/;

  function indentOf(raw) {
    var i = 0;
    while (i < raw.length && raw.charAt(i) === " ") i += 1;
    if (i < raw.length && raw.charAt(i) === "\t") {
      throw new DogError("tabs are not allowed for indentation: " + JSON.stringify(raw));
    }
    return i;
  }

  function bodyItems(bodyLines) {
    var items = [];
    for (var k = 0; k < bodyLines.length; k++) {
      var raw = bodyLines[k];
      var ind = indentOf(raw); // may throw on tabs -- same order as dog.py
      if (raw.trim() === "") continue;
      if (raw.replace(/^ +/, "").charAt(0) === "#") continue;
      items.push({ indent: ind, text: raw.trim(), raw: raw });
    }
    return items;
  }

  function parseBlockScalar(items, i, level) {
    if (i >= items.length || items[i].indent <= level) {
      throw new DogError("expected an indented block after `|`");
    }
    var base = items[i].indent;
    var buf = [];
    while (i < items.length && items[i].indent >= base) {
      var raw = items[i].raw;
      buf.push(raw.length >= base ? raw.slice(base) : "");
      i += 1;
    }
    return [buf.join("\n"), i];
  }

  function dictGet(dict, key, fallback) {
    return hasOwn(dict, key) ? dict[key] : fallback;
  }

  function parseMap(items, i, level, cfg, seed) {
    var d = newObj();
    if (seed) d[seed[0]] = seed[1];
    while (i < items.length) {
      var it = items[i];
      if (it.indent < level) break;
      if (it.indent !== level) {
        throw new DogError("bad indentation in map at " + JSON.stringify(it.text));
      }
      var m = PAIR_RE.exec(it.text);
      if (!m) {
        throw new DogError("expected `key: value` in map, got " + JSON.stringify(it.text));
      }
      var key = expandRep(dictGet(cfg.dict, m[1], m[1]), cfg);
      var val = m[2]; // undefined when the group did not participate
      i += 1;
      var v;
      if (val === undefined || val === "") {
        if (i < items.length && items[i].indent > level) {
          var r = parseValue(items, i, items[i].indent, cfg);
          v = r[0]; i = r[1];
        } else {
          v = null;
        }
      } else if (val === "|") {
        var b = parseBlockScalar(items, i, level);
        v = b[0]; i = b[1];
      } else {
        v = parseScalar(val, cfg);
      }
      d[key] = v; // last duplicate wins
    }
    return [d, i];
  }

  function parseList(items, i, level, cfg) {
    var out = [];
    while (i < items.length) {
      var it = items[i];
      if (it.indent < level) break;
      if (it.indent !== level) {
        throw new DogError("bad indentation in list at " + JSON.stringify(it.text));
      }
      if (!(it.text === "-" || it.text.indexOf("- ") === 0)) break;
      var rest = it.text.slice(1).trim();
      i += 1;
      if (rest === "") {
        var v;
        if (i < items.length && items[i].indent > level) {
          var r0 = parseValue(items, i, items[i].indent, cfg);
          v = r0[0]; i = r0[1];
        } else {
          v = null;
        }
        out.push(v);
      } else {
        var m = PAIR_RE.exec(rest);
        if (m) {
          var key = expandRep(dictGet(cfg.dict, m[1], m[1]), cfg);
          var val = m[2];
          if (val === undefined || val === "") {
            // `- key:` -- deeper block is the VALUE of key (YAML rule)
            var v2;
            if (i < items.length && items[i].indent > level) {
              var r1 = parseValue(items, i, items[i].indent, cfg);
              v2 = r1[0]; i = r1[1];
            } else {
              v2 = null;
            }
            var d1 = newObj(); d1[key] = v2;
            out.push(d1);
          } else if (val === "|") {
            var b = parseBlockScalar(items, i, level);
            var d2 = newObj(); d2[key] = b[0];
            out.push(d2); i = b[1];
          } else {
            var seedKey = key, seedVal = parseScalar(val, cfg);
            if (i < items.length && items[i].indent > level) {
              var r2 = parseMap(items, i, items[i].indent, cfg, [seedKey, seedVal]);
              out.push(r2[0]); i = r2[1];
            } else {
              var d3 = newObj(); d3[seedKey] = seedVal;
              out.push(d3);
            }
          }
        } else {
          out.push(parseScalar(rest, cfg));
        }
      }
    }
    return [out, i];
  }

  function parseValue(items, i, level, cfg) {
    var it = items[i];
    if (it.indent !== level) {
      throw new DogError("bad indentation at " + JSON.stringify(it.text));
    }
    if (it.text === "-" || it.text.indexOf("- ") === 0) {
      return parseList(items, i, level, cfg);
    }
    if (PAIR_RE.test(it.text)) return parseMap(items, i, level, cfg);
    return [parseScalar(it.text, cfg), i + 1];
  }

  function parseRows(items, cfg) {
    // aliases from dict: are allowed in keys: -- dict expands, rep does not
    var keys = cfg.keys.map(function (k) { return dictGet(cfg.dict, k, k); });
    var sep = cfg.sep;
    var out = [];
    for (var n = 0; n < items.length; n++) {
      var it = items[n];
      if (it.indent !== 0) {
        throw new DogError("positional rows must not be indented: " + JSON.stringify(it.text));
      }
      var fields = it.text.split(sep);
      if (fields.length !== keys.length) {
        throw new DogError("row has " + fields.length + " fields but keys: declares " +
                           keys.length + ": " + JSON.stringify(it.text));
      }
      var d = newObj();
      for (var f = 0; f < keys.length; f++) {
        var fv = fields[f].trim();
        d[keys[f]] = fv !== "" ? parseScalar(fv, cfg) : null;
      }
      out.push(d);
    }
    return out;
  }

  function parseDogBody(bodyLines, cfg) {
    var items = bodyItems(bodyLines);
    if (!items.length) return null;
    if (cfg.keys !== null) return parseRows(items, cfg);
    var r = parseValue(items, 0, 0, cfg);
    if (r[1] !== items.length) {
      throw new DogError("unexpected trailing content: " + JSON.stringify(items[r[1]].text));
    }
    if (items[0].indent !== 0) {
      throw new DogError("top-level content must not be indented");
    }
    return r[0];
  }

  /* ------------------------------------------------------------------ */
  /* json / xml / yaml bodies -- byte-identical, header alone makes .dog */
  /* ------------------------------------------------------------------ */

  function parseJsonBody(bodyLines) {
    var text = bodyLines.join("\n").trim();
    if (!text) return null;
    try {
      return JSON.parse(text);
    } catch (e) {
      throw new DogError("bad JSON body: " + e.message);
    }
  }

  // Minimal XML parser: elements, attributes, text, comments, PIs, DOCTYPE
  // skipping, and the five predefined + numeric entities. Enough for the
  // v1 mapping (attributes -> "@name", repeated siblings -> arrays,
  // element text -> "#text" when attributes/children exist).
  function parseXmlBody(bodyLines) {
    var text = bodyLines.join("\n").trim();
    if (!text) return null;

    function fail(msg) { throw new DogError("bad XML body: " + msg); }

    var ENTITIES = { amp: "&", lt: "<", gt: ">", quot: '"', apos: "'" };
    function unescapeXml(s) {
      return s.replace(/&(#\d+|#[xX][0-9a-fA-F]+|[a-zA-Z]+);/g, function (m, e) {
        if (hasOwn(ENTITIES, e)) return ENTITIES[e];
        if (e.charAt(0) === "#") {
          var hex = e.charAt(1) === "x" || e.charAt(1) === "X";
          var code = parseInt(hex ? e.slice(2) : e.slice(1), hex ? 16 : 10);
          if (!isNaN(code)) {
            try { return String.fromCodePoint(code); } catch (err) { /* fall through */ }
          }
          fail("bad character reference " + m);
        }
        fail("unknown entity " + m);
        return m; // unreachable
      });
    }

    var s = text, pos = 0, n = s.length;

    function skipWs() { while (pos < n && /\s/.test(s.charAt(pos))) pos++; }

    function readName() {
      var start = pos;
      while (pos < n && /[^\s\/>=]/.test(s.charAt(pos))) pos++;
      if (pos === start) fail("expected a name at offset " + pos);
      return s.slice(start, pos);
    }

    function skipMisc() {
      // comments, PIs, DOCTYPE/declarations before or after the root
      for (;;) {
        if (s.indexOf("<!--", pos) === pos) {
          var e = s.indexOf("-->", pos + 4);
          if (e < 0) fail("unterminated comment");
          pos = e + 3;
        } else if (s.indexOf("<?", pos) === pos) {
          var p = s.indexOf("?>", pos + 2);
          if (p < 0) fail("unterminated processing instruction");
          pos = p + 2;
        } else if (s.indexOf("<!", pos) === pos) {
          var d = pos + 2, depth = 0;
          while (d < n) {
            if (s.charAt(d) === ">" && depth === 0) break;
            if (s.charAt(d) === "[") depth++;
            if (s.charAt(d) === "]") depth--;
            d++;
          }
          if (d >= n) fail("unterminated declaration");
          pos = d + 1;
        } else {
          return;
        }
      }
    }

    function parseElement() {
      // s[pos] === '<' and not a closing tag
      pos++; // consume '<'
      var name = readName();
      var attrs = newObj();
      for (;;) {
        skipWs();
        if (s.indexOf("/>", pos) === pos) {
          pos += 2;
          return { tag: name, attrs: attrs, children: [], text: "" };
        }
        if (s.charAt(pos) === ">") { pos++; break; }
        if (pos >= n) fail("unterminated start tag <" + name + ">");
        var an = readName();
        skipWs();
        if (s.charAt(pos) !== "=") fail("expected '=' after attribute " + an);
        pos++;
        skipWs();
        var q = s.charAt(pos);
        if (q !== '"' && q !== "'") fail("attribute value must be quoted");
        pos++;
        var v = "";
        while (pos < n && s.charAt(pos) !== q) { v += s.charAt(pos); pos++; }
        if (pos >= n) fail("unterminated attribute value");
        pos++;
        attrs[an] = unescapeXml(v);
      }
      var children = [];
      var textAcc = ""; // leading text before the first child (mirror ET's .text)
      var seenChild = false;
      for (;;) {
        if (pos >= n) fail("unterminated element <" + name + ">");
        if (s.charAt(pos) === "<") {
          if (s.indexOf("</", pos) === pos) {
            pos += 2;
            skipWs();
            var cn = readName();
            skipWs();
            if (s.charAt(pos) !== ">") fail("expected '>' in closing tag");
            pos++;
            if (cn !== name) fail("mismatched close tag </" + cn + "> for <" + name + ">");
            break;
          } else if (s.indexOf("<!--", pos) === pos) {
            var e = s.indexOf("-->", pos + 4);
            if (e < 0) fail("unterminated comment");
            pos = e + 3;
          } else if (s.indexOf("<?", pos) === pos) {
            var p = s.indexOf("?>", pos + 2);
            if (p < 0) fail("unterminated processing instruction");
            pos = p + 2;
          } else if (s.indexOf("<![CDATA[", pos) === pos) {
            var c = s.indexOf("]]>", pos + 9);
            if (c < 0) fail("unterminated CDATA section");
            if (!seenChild) textAcc += s.slice(pos + 9, c);
            pos = c + 3;
          } else if (s.indexOf("<!", pos) === pos) {
            fail("unexpected declaration inside element");
          } else {
            children.push(parseElement());
            seenChild = true;
          }
        } else {
          var e2 = s.indexOf("<", pos);
          var t = e2 < 0 ? s.slice(pos) : s.slice(pos, e2);
          pos = e2 < 0 ? n : e2;
          if (!seenChild) textAcc += t; // tail after children is ignored, like ET
        }
      }
      return { tag: name, attrs: attrs, children: children, text: textAcc };
    }

    function xmlValue(node) {
      var d = newObj();
      var names = Object.keys(node.attrs);
      for (var a = 0; a < names.length; a++) d["@" + names[a]] = node.attrs[names[a]];
      if (node.children.length === 0) {
        var t = unescapeXml(node.text).trim();
        if (names.length) {
          if (t) d["#text"] = t;
          return d;
        }
        return coerceLoose(t);
      }
      for (var c = 0; c < node.children.length; c++) {
        var child = node.children[c];
        var v = xmlValue(child);
        if (hasOwn(d, child.tag)) {
          var cur = d[child.tag];
          if (Array.isArray(cur)) cur.push(v);
          else d[child.tag] = [cur, v];
        } else {
          d[child.tag] = v;
        }
      }
      return d;
    }

    skipWs();
    skipMisc();
    skipWs();
    if (pos >= n || s.charAt(pos) !== "<") fail("no root element");
    var root = parseElement();
    skipWs();
    skipMisc();
    skipWs();
    if (pos !== n) fail("junk after root element");
    var wrapped = newObj();
    wrapped[root.tag] = xmlValue(root);
    return wrapped;
  }

  /* ------------------------------------------------------------------ */
  /* top-level                                                           */
  /* ------------------------------------------------------------------ */

  function parse(text) {
    var hb = splitHeader(text);
    var cfg = buildConfig(hb.directives);
    var lang = cfg.lang;
    if (lang === "json") return parseJsonBody(hb.body);
    if (lang === "xml") return parseXmlBody(hb.body);
    if (lang === "yaml") {
      // header tools are native-dog features; YAML bodies stay byte-identical
      cfg = {
        lang: cfg.lang, dict: newObj(), rep: [], keys: null,
        sep: cfg.sep, indent: cfg.indent, quote: cfg.quote
      };
      return parseDogBody(hb.body, cfg); // common block-mapping subset
    }
    return parseDogBody(hb.body, cfg);
  }

  var api = { parse: parse, DogError: DogError, MAGIC: MAGIC };

  // CommonJS (node). `typeof module` guard keeps this safe in browsers and
  // in ESM contexts, where a bare `module` reference would throw.
  if (typeof module !== "undefined" && module.exports) {
    module.exports = api;
  }
  // Browser global.
  if (typeof globalThis !== "undefined") {
    try { globalThis.dog = api; } catch (e) { /* non-writable global -- ignore */ }
  }
})();
