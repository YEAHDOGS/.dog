/*
 * dog.kt -- parser for the .dog text data format (DOG/1, magic `.dog/1.0`).
 *
 * Single-file, zero dependencies (Kotlin stdlib only). Mirrors the dog.js /
 * dog.go / dog.rs behavior: in particular it REJECTS truncated directive
 * names (l:, di:, ...) as hard errors, because abbreviated directives are
 * not in the spec (bag log D1). It does NOT copy dog.py's silent leniency.
 *
 * Library use:
 *
 *     val v: Any? = parse(text)
 *
 * Values are JSON-equivalent: null, Boolean, Long, Double, String,
 * List<Any?>, or LinkedHashMap<String, Any?> (insertion-ordered, duplicate
 * keys keep first position, last wins).
 *
 * CLI use (mirrors dog.py):
 *
 *     kotlinc dog.kt -include-runtime -d dog.jar && java -jar dog.jar file.dog
 *
 * Errors throw DogError.
 *
 * This product was made by DOGS -- https://wearedogs.net
 */

import java.util.Locale
import kotlin.system.exitProcess

const val MAGIC = ".dog/1.0"
val KNOWN_DIRECTIVES = listOf("lang", "dict", "rep", "keys", "sep", "indent", "quote")
const val PLACEHOLDER = '' // U+E000 -- must match dog.py's escape placeholder

/** Raised for any malformed .dog document. */
class DogError(message: String) : Exception(message)

/* ------------------------------------------------------------------ */
/* value model + JSON text output                                       */
/* ------------------------------------------------------------------ */

fun jsonQuote(s: String): String {
    val sb = StringBuilder("\"")
    for (c in s) {
        when (c) {
            '"' -> sb.append("\\\"")
            '\\' -> sb.append("\\\\")
            '\b' -> sb.append("\\b")
            '\u000C' -> sb.append("\\f")
            '\n' -> sb.append("\\n")
            '\r' -> sb.append("\\r")
            '\t' -> sb.append("\\t")
            else -> if (c < ' ') sb.append(String.format("\\u%04x", c.code)) else sb.append(c)
        }
    }
    return sb.append('"').toString()
}

fun jsonDump(v: Any?, level: Int, sb: StringBuilder) {
    when (v) {
        null -> sb.append("null")
        is Boolean -> sb.append(if (v) "true" else "false")
        is Long -> sb.append(v.toString())
        is Int -> sb.append(v.toString())
        is Double -> sb.append(v.toString())
        is String -> sb.append(jsonQuote(v))
        is List<*> -> {
            if (v.isEmpty()) { sb.append("[]"); return }
            sb.append("[\n")
            v.forEachIndexed { idx, e ->
                sb.append("  ".repeat(level + 1))
                jsonDump(e, level + 1, sb)
                if (idx < v.size - 1) sb.append(",")
                sb.append("\n")
            }
            sb.append("  ".repeat(level)).append("]")
        }
        is Map<*, *> -> {
            if (v.isEmpty()) { sb.append("{}"); return }
            sb.append("{\n")
            val entries = v.entries.toList()
            entries.forEachIndexed { idx, (k, e) ->
                sb.append("  ".repeat(level + 1))
                sb.append(jsonQuote(k.toString())).append(": ")
                jsonDump(e, level + 1, sb)
                if (idx < entries.size - 1) sb.append(",")
                sb.append("\n")
            }
            sb.append("  ".repeat(level)).append("}")
        }
        else -> sb.append(jsonQuote(v.toString()))
    }
}

/* ------------------------------------------------------------------ */
/* header                                                               */
/* ------------------------------------------------------------------ */

data class Config(
    val lang: String = "dog",
    val dict: Map<String, String> = emptyMap(),
    val rep: List<Pair<String, String>> = emptyList(),
    val keys: List<String>? = null,
    val sep: String = "|",
    val indent: Int = 2,
    val quote: String = "\""
)

/* Split a header line into whitespace-separated tokens. Double-quoted
   spans may contain spaces (`keys:"a b"`); inside quotes a backslash
   escapes the next character (`\"` , `\\`). */
fun tokenizeHeaderLine(line: String): List<String> {
    val tokens = mutableListOf<String>()
    val cur = StringBuilder()
    var inQuotes = false
    var escaped = false
    for (ch in line) {
        if (inQuotes) {
            when {
                escaped -> { cur.append(ch); escaped = false }
                ch == '\\' -> escaped = true
                ch == '"' -> inQuotes = false
                else -> cur.append(ch)
            }
        } else when (ch) {
            '"' -> inQuotes = true
            ' ', '\t', '\r', '\u000B', '\u000C' -> {
                if (cur.isNotEmpty()) { tokens.add(cur.toString()); cur.clear() }
            }
            else -> cur.append(ch)
        }
    }
    if (inQuotes || escaped) throw DogError("unterminated quote in header line: ${jsonQuote(line)}")
    if (cur.isNotEmpty()) tokens.add(cur.toString())
    return tokens
}

/* The header is exactly ONE line: line 1, starting with the magic
   `.dog/1.0` followed by space-separated `name:value` params. A reader
   never has to guess where the header ends. The body is every line
   after line 1; one optional blank line right after the header is skipped. */
fun splitHeader(text: String): Pair<Map<String, String>, List<String>> {
    val stripped = text.removePrefix("﻿")
    val lines = stripped.split(Regex("\r\n|[\n\r  ]"))
    if (lines.isEmpty()) {
        throw DogError("first line must be the magic '.dog/1.0'")
    }
    val tokens = tokenizeHeaderLine(lines[0])
    if (tokens.isEmpty() || tokens[0] != MAGIC) {
        throw DogError("first line must be the magic '.dog/1.0'")
    }
    val directives = LinkedHashMap<String, String>()
    for (tok in tokens.drop(1)) {
        val ci = tok.indexOf(':')
        if (ci < 0) {
            throw DogError("bad header directive ${jsonQuote(tok)} (want name:value)")
        }
        val name = tok.substring(0, ci).lowercase(Locale.ROOT)
        if (name.isEmpty() || name.any { it.isWhitespace() }) {
            throw DogError("bad directive name in ${jsonQuote(tok)}")
        }
        // Abbreviated directives are NOT in the spec (needs founder sign-off,
        // bag log D1). A truncated known-directive name is a hard error here,
        // never silently resolved -- and never confused with the unknown-
        // directive ignore rule, which only covers names that are not
        // truncations of known directives.
        for (full in KNOWN_DIRECTIVES) {
            if (name != full && full.startsWith(name)) {
                throw DogError("truncated directive '$name:' is not valid -- abbreviations are not in the spec (want '$full:')")
            }
        }
        directives[name] = tok.substring(ci + 1) // last one wins
    }
    var body = lines.drop(1)
    if (body.isNotEmpty() && body[0].trim().isEmpty()) body = body.drop(1)
    return Pair(directives, body)
}

fun splitWs(s: String): List<String> = s.split(Regex("\\s+")).filter { it.isNotEmpty() }

fun buildConfig(directives: Map<String, String>): Config {
    val lang = (directives["lang"] ?: "dog").trim().lowercase(Locale.ROOT)
    if (lang !in listOf("dog", "json", "xml", "yaml")) {
        throw DogError("unsupported lang: ${jsonQuote(directives["lang"] ?: "")} (want dog|json|xml|yaml)")
    }
    val dict = LinkedHashMap<String, String>()
    directives["dict"]?.let { d ->
        for (pair in splitWs(d)) {
            val ei = pair.indexOf('=')
            if (ei < 0) throw DogError("bad dict pair ${jsonQuote(pair)} (want alias=key)")
            val alias = pair.substring(0, ei)
            val real = pair.substring(ei + 1)
            if (alias.isEmpty() || real.isEmpty()) throw DogError("bad dict pair ${jsonQuote(pair)}")
            dict[alias] = real
        }
    }
    val repPairs = mutableListOf<Pair<String, String>>()
    directives["rep"]?.let { r ->
        for (pair in splitWs(r)) {
            val ei = pair.indexOf('=')
            if (ei < 0) throw DogError("bad rep pair ${jsonQuote(pair)} (want token=expansion)")
            val tok = pair.substring(0, ei)
            val exp = pair.substring(ei + 1)
            if (tok.isEmpty()) throw DogError("empty rep token in ${jsonQuote(pair)}")
            repPairs.add(Pair(tok, exp))
        }
    }
    repPairs.sortByDescending { it.first.length } // longest token wins
    var keys: List<String>? = null
    directives["keys"]?.let { k ->
        val ks = splitWs(k)
        if (ks.isEmpty()) throw DogError("empty keys: directive")
        keys = ks
    }
    var indent = 2
    directives["indent"]?.let { ind ->
        if (!Regex("^[+-]?\\d+$").matches(ind)) throw DogError("indent: must be an integer")
        val n = ind.toInt()
        if (n < 1) throw DogError("indent: must be >= 1")
        indent = n
    }
    val sep = directives["sep"] ?: "|"
    val quote = directives["quote"] ?: "\""
    if (sep.isEmpty()) throw DogError("sep: must not be empty")
    if (quote.isEmpty()) throw DogError("quote: must not be empty")
    return Config(lang, dict, repPairs, keys, sep, indent, quote)
}

/* ------------------------------------------------------------------ */
/* expansion                                                            */
/* ------------------------------------------------------------------ */

fun expandRep(s0: String, cfg: Config): String {
    if (cfg.rep.isEmpty()) return s0
    val saved = StringBuilder()
    val out = StringBuilder()
    var i = 0
    val n = s0.length
    while (i < n) {
        val c = s0[i]
        if (c == '\\' && i + 1 < n) {
            saved.append(s0[i + 1])
            out.append(PLACEHOLDER)
            i += 2
        } else {
            out.append(c)
            i += 1
        }
    }
    var s = out.toString()
    for ((tok, exp) in cfg.rep) s = s.replace(tok, exp)
    val res = StringBuilder()
    var si = 0
    for (ch in s) {
        if (ch == PLACEHOLDER) { res.append(saved[si]); si += 1 } else res.append(ch)
    }
    return res.toString()
}

val INT_RE = Regex("^-?\\d+$")
val FLOAT_RE = Regex("^-?(?:\\d+\\.\\d*|\\.\\d+|\\d+)(?:[eE][-+]?\\d+)?$")

fun parseScalar(s0: String, cfg: Config): Any? {
    val s = expandRep(s0, cfg)
    val q = cfg.quote
    if (s.length >= 2 && s[0].toString() == q && s[s.length - 1].toString() == q) {
        val inner = s.substring(1, s.length - 1)
        return inner.replace("\\$q", q).replace("\\\\", "\\")
    }
    val low = s.lowercase(Locale.ROOT)
    if (low == "null") return null
    if (low == "true") return true
    if (low == "false") return false
    if (INT_RE.matches(s)) return s.toLongOrNull() ?: s.toDouble()
    if (FLOAT_RE.matches(s)) return s.toDouble()
    return s
}

fun coerceLoose(s: String): Any? = parseScalar(s, Config(rep = emptyList(), quote = "\""))

/* ------------------------------------------------------------------ */
/* native `dog` body: indentation-based maps / lists / scalars / rows   */
/* ------------------------------------------------------------------ */

val PAIR_RE = Regex("""^([^\s:]+)\s*:(?:[ \t]+(.*))?$""")

data class Item(val indent: Int, val text: String, val raw: String)

fun indentOf(raw: String): Int {
    var i = 0
    while (i < raw.length && raw[i] == ' ') i += 1
    if (i < raw.length && raw[i] == '\t') {
        throw DogError("tabs are not allowed for indentation: ${jsonQuote(raw)}")
    }
    return i
}

fun bodyItems(bodyLines: List<String>): List<Item> {
    val items = mutableListOf<Item>()
    for (raw in bodyLines) {
        val ind = indentOf(raw) // may throw on tabs -- same order as dog.py
        if (raw.trim().isEmpty()) continue
        if (raw.dropWhile { it == ' ' }.firstOrNull() == '#') continue
        items.add(Item(ind, raw.trim(), raw))
    }
    return items
}

fun parseBlockScalar(items: List<Item>, i0: Int, level: Int): Pair<String, Int> {
    var i = i0
    if (i >= items.size || items[i].indent <= level) {
        throw DogError("expected an indented block after `|`")
    }
    val base = items[i].indent
    val buf = mutableListOf<String>()
    while (i < items.size && items[i].indent >= base) {
        val raw = items[i].raw
        buf.add(if (raw.length >= base) raw.substring(base) else "")
        i += 1
    }
    return Pair(buf.joinToString("\n"), i)
}

fun parseMap(items: List<Item>, i0: Int, level: Int, cfg: Config, seed: Pair<String, Any?>? = null): Pair<LinkedHashMap<String, Any?>, Int> {
    val d = LinkedHashMap<String, Any?>()
    if (seed != null) d[seed.first] = seed.second
    var i = i0
    while (i < items.size) {
        val it = items[i]
        if (it.indent < level) break
        if (it.indent != level) {
            throw DogError("bad indentation in map at ${jsonQuote(it.text)}")
        }
        val m = PAIR_RE.matchEntire(it.text)
            ?: throw DogError("expected `key: value` in map, got ${jsonQuote(it.text)}")
        val key = expandRep(cfg.dict[m.groupValues[1]] ?: m.groupValues[1], cfg)
        val vstr = m.groups[2]?.value // null when the group did not participate
        i += 1
        val v: Any?
        if (vstr == null || vstr == "") {
            if (i < items.size && items[i].indent > level) {
                val r = parseValue(items, i, items[i].indent, cfg)
                v = r.first; i = r.second
            } else {
                v = null
            }
        } else if (vstr == "|") {
            val b = parseBlockScalar(items, i, level)
            v = b.first; i = b.second
        } else {
            v = parseScalar(vstr, cfg)
        }
        d[key] = v // last duplicate wins
    }
    return Pair(d, i)
}

fun parseList(items: List<Item>, i0: Int, level: Int, cfg: Config): Pair<MutableList<Any?>, Int> {
    val out = mutableListOf<Any?>()
    var i = i0
    while (i < items.size) {
        val it = items[i]
        if (it.indent < level) break
        if (it.indent != level) {
            throw DogError("bad indentation in list at ${jsonQuote(it.text)}")
        }
        if (!(it.text == "-" || it.text.startsWith("- "))) break
        val rest = it.text.substring(1).trim()
        i += 1
        if (rest == "") {
            val v: Any?
            if (i < items.size && items[i].indent > level) {
                val r0 = parseValue(items, i, items[i].indent, cfg)
                v = r0.first; i = r0.second
            } else {
                v = null
            }
            out.add(v)
        } else {
            val m = PAIR_RE.matchEntire(rest)
            if (m != null) {
                val key = expandRep(cfg.dict[m.groupValues[1]] ?: m.groupValues[1], cfg)
                val vstr = m.groups[2]?.value
                if (vstr == null || vstr == "") {
                    // `- key:` -- deeper block is the VALUE of key (YAML rule)
                    val v2: Any?
                    if (i < items.size && items[i].indent > level) {
                        val r1 = parseValue(items, i, items[i].indent, cfg)
                        v2 = r1.first; i = r1.second
                    } else {
                        v2 = null
                    }
                    val d1 = LinkedHashMap<String, Any?>(); d1[key] = v2
                    out.add(d1)
                } else if (vstr == "|") {
                    val b = parseBlockScalar(items, i, level)
                    val d2 = LinkedHashMap<String, Any?>(); d2[key] = b.first
                    out.add(d2); i = b.second
                } else {
                    val seedKey = key
                    val seedVal = parseScalar(vstr, cfg)
                    if (i < items.size && items[i].indent > level) {
                        val r2 = parseMap(items, i, items[i].indent, cfg, Pair(seedKey, seedVal))
                        out.add(r2.first); i = r2.second
                    } else {
                        val d3 = LinkedHashMap<String, Any?>(); d3[seedKey] = seedVal
                        out.add(d3)
                    }
                }
            } else {
                out.add(parseScalar(rest, cfg))
            }
        }
    }
    return Pair(out, i)
}

fun parseValue(items: List<Item>, i: Int, level: Int, cfg: Config): Pair<Any?, Int> {
    val it = items[i]
    if (it.indent != level) {
        throw DogError("bad indentation at ${jsonQuote(it.text)}")
    }
    if (it.text == "-" || it.text.startsWith("- ")) {
        return parseList(items, i, level, cfg)
    }
    if (PAIR_RE.matches(it.text)) return parseMap(items, i, level, cfg)
    return Pair(parseScalar(it.text, cfg), i + 1)
}

fun parseRows(items: List<Item>, cfg: Config): List<LinkedHashMap<String, Any?>> {
    // aliases from dict: are allowed in keys: -- dict expands, rep does not
    val keys = cfg.keys!!.map { k -> cfg.dict[k] ?: k }
    val sep = cfg.sep
    val out = mutableListOf<LinkedHashMap<String, Any?>>()
    for (it in items) {
        if (it.indent != 0) {
            throw DogError("positional rows must not be indented: ${jsonQuote(it.text)}")
        }
        val fields = it.text.split(sep)
        if (fields.size != keys.size) {
            throw DogError("row has ${fields.size} fields but keys: declares ${keys.size}: ${jsonQuote(it.text)}")
        }
        val d = LinkedHashMap<String, Any?>()
        for (f in keys.indices) {
            val fv = fields[f].trim()
            d[keys[f]] = if (fv != "") parseScalar(fv, cfg) else null
        }
        out.add(d)
    }
    return out
}

fun parseDogBody(bodyLines: List<String>, cfg: Config): Any? {
    val items = bodyItems(bodyLines)
    if (items.isEmpty()) return null
    if (cfg.keys != null) return parseRows(items, cfg)
    val r = parseValue(items, 0, 0, cfg)
    if (r.second != items.size) {
        throw DogError("unexpected trailing content: ${jsonQuote(items[r.second].text)}")
    }
    if (items[0].indent != 0) {
        throw DogError("top-level content must not be indented")
    }
    return r.first
}

/* ------------------------------------------------------------------ */
/* json bodies -- byte-identical; also parses .expected.json in tests   */
/* ------------------------------------------------------------------ */

private class JsonParser(val s: String) {
    var pos = 0
    val n = s.length

    fun fail(msg: String): Nothing = throw DogError(msg)

    fun skipWs() {
        while (pos < n && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) pos++
    }

    fun parse(): Any? {
        skipWs()
        val v = parseValue()
        skipWs()
        if (pos != n) fail("unexpected trailing characters at offset $pos")
        return v
    }

    fun parseValue(): Any? {
        if (pos >= n) fail("unexpected end of JSON")
        return when (val c = s[pos]) {
            '{' -> parseObject()
            '[' -> parseArray()
            '"' -> parseString()
            't' -> { expect("true"); true }
            'f' -> { expect("false"); false }
            'n' -> { expect("null"); null }
            '-', in '0'..'9' -> parseNumber()
            else -> fail("unexpected character '$c' at offset $pos")
        }
    }

    fun expect(word: String) {
        if (!s.startsWith(word, pos)) fail("invalid literal at offset $pos")
        pos += word.length
    }

    fun parseObject(): LinkedHashMap<String, Any?> {
        pos++ // '{'
        val d = LinkedHashMap<String, Any?>()
        skipWs()
        if (pos < n && s[pos] == '}') { pos++; return d }
        while (true) {
            skipWs()
            if (pos >= n || s[pos] != '"') fail("expected string key at offset $pos")
            val k = parseString()
            skipWs()
            if (pos >= n || s[pos] != ':') fail("expected ':' at offset $pos")
            pos++
            skipWs()
            d[k] = parseValue()
            skipWs()
            if (pos >= n) fail("unterminated object")
            when (s[pos]) {
                ',' -> { pos++; continue }
                '}' -> { pos++; return d }
                else -> fail("expected ',' or '}' at offset $pos")
            }
        }
    }

    fun parseArray(): MutableList<Any?> {
        pos++ // '['
        val out = mutableListOf<Any?>()
        skipWs()
        if (pos < n && s[pos] == ']') { pos++; return out }
        while (true) {
            skipWs()
            out.add(parseValue())
            skipWs()
            if (pos >= n) fail("unterminated array")
            when (s[pos]) {
                ',' -> { pos++; continue }
                ']' -> { pos++; return out }
                else -> fail("expected ',' or ']' at offset $pos")
            }
        }
    }

    fun parseString(): String {
        pos++ // '"'
        val sb = StringBuilder()
        while (true) {
            if (pos >= n) fail("unterminated string")
            val c = s[pos++]
            when (c) {
                '"' -> return sb.toString()
                '\\' -> {
                    if (pos >= n) fail("unterminated escape")
                    when (val e = s[pos++]) {
                        '"' -> sb.append('"')
                        '\\' -> sb.append('\\')
                        '/' -> sb.append('/')
                        'b' -> sb.append('\b')
                        'f' -> sb.append('\u000C')
                        'n' -> sb.append('\n')
                        'r' -> sb.append('\r')
                        't' -> sb.append('\t')
                        'u' -> {
                            if (pos + 4 > n) fail("bad unicode escape")
                            val h = s.substring(pos, pos + 4)
                            val code = h.toIntOrNull(16) ?: fail("bad unicode escape \\u$h")
                            pos += 4
                            if (code in 0xD800..0xDBFF && pos + 6 <= n && s[pos] == '\\' && s[pos + 1] == 'u') {
                                val l = s.substring(pos + 2, pos + 6).toIntOrNull(16)
                                if (l != null && l in 0xDC00..0xDFFF) {
                                    sb.append(Character.toChars(0x10000 + ((code - 0xD800) shl 10) + (l - 0xDC00)))
                                    pos += 6
                                } else {
                                    sb.append(code.toChar())
                                }
                            } else {
                                sb.append(code.toChar())
                            }
                        }
                        else -> fail("bad escape '\\$e'")
                    }
                }
                else -> {
                    if (c < ' ') fail("unescaped control character")
                    sb.append(c)
                }
            }
        }
    }

    fun parseNumber(): Any {
        val start = pos
        if (pos < n && s[pos] == '-') pos++
        while (pos < n && s[pos] in '0'..'9') pos++
        val isFloat = (pos < n && s[pos] == '.') || (pos < n && (s[pos] == 'e' || s[pos] == 'E'))
        if (pos < n && s[pos] == '.') {
            pos++
            while (pos < n && s[pos] in '0'..'9') pos++
        }
        if (pos < n && (s[pos] == 'e' || s[pos] == 'E')) {
            pos++
            if (pos < n && (s[pos] == '+' || s[pos] == '-')) pos++
            val dstart = pos
            while (pos < n && s[pos] in '0'..'9') pos++
            if (pos == dstart) fail("bad number at offset $start")
        }
        val tok = s.substring(start, pos)
        if (tok == "-" || tok.isEmpty()) fail("bad number at offset $start")
        // JSON.parse rejects leading zeros ("01"); mirror that strictness.
        if (Regex("^-?0\\d").containsMatchIn(tok)) fail("bad number at offset $start")
        return if (!isFloat) tok.toLongOrNull() ?: (tok.toDoubleOrNull() ?: fail("bad number at offset $start"))
        else tok.toDoubleOrNull() ?: fail("bad number at offset $start")
    }
}

/** Parse a JSON text into the dog value model (used for `lang: json` bodies
 *  and for reading .expected.json in the conformance runner). */
fun parseJsonValue(text: String): Any? = JsonParser(text).parse()

fun parseJsonBody(bodyLines: List<String>): Any? {
    val text = bodyLines.joinToString("\n").trim()
    if (text.isEmpty()) return null
    try {
        return parseJsonValue(text)
    } catch (e: DogError) {
        throw DogError("bad JSON body: ${e.message}")
    }
}

/* ------------------------------------------------------------------ */
/* xml bodies -- minimal parser, same v1 mapping as dog.js              */
/* ------------------------------------------------------------------ */

val XML_ENTITIES = mapOf("amp" to "&", "lt" to "<", "gt" to ">", "quot" to "\"", "apos" to "'")
val XML_ENTITY_RE = Regex("&(#[0-9]+|#[xX][0-9a-fA-F]+|[a-zA-Z]+);")

fun unescapeXml(s: String): String = XML_ENTITY_RE.replace(s) { m ->
    val e = m.groupValues[1]
    XML_ENTITIES[e] ?: run {
        if (e.startsWith("#")) {
            val hex = e[1] == 'x' || e[1] == 'X'
            val code = (if (hex) e.substring(2) else e.substring(1)).toLong(if (hex) 16 else 10)
            if (code < 0 || code > 0x10FFFF || code in 0xD800..0xDFFF) {
                throw DogError("bad XML body: bad character reference ${m.value}")
            }
            String(Character.toChars(code.toInt()))
        } else {
            throw DogError("bad XML body: unknown entity ${m.value}")
        }
    }
}

data class XmlNode(
    val tag: String,
    val attrs: LinkedHashMap<String, String>,
    val children: MutableList<XmlNode>,
    val text: StringBuilder // leading text before the first child (ET's .text)
)

private class XmlParser(val s: String) {
    var pos = 0
    val n = s.length

    fun fail(msg: String): Nothing = throw DogError("bad XML body: $msg")

    fun skipWs() {
        while (pos < n && s[pos].isWhitespace()) pos++
    }

    fun readName(): String {
        val start = pos
        while (pos < n && s[pos] != '/' && s[pos] != '>' && s[pos] != '=' && !s[pos].isWhitespace()) pos++
        if (pos == start) fail("expected a name at offset $pos")
        return s.substring(start, pos)
    }

    fun skipMisc() {
        // comments, PIs, DOCTYPE/declarations before or after the root
        while (true) {
            when {
                s.startsWith("<!--", pos) -> {
                    val e = s.indexOf("-->", pos + 4)
                    if (e < 0) fail("unterminated comment")
                    pos = e + 3
                }
                s.startsWith("<?", pos) -> {
                    val p = s.indexOf("?>", pos + 2)
                    if (p < 0) fail("unterminated processing instruction")
                    pos = p + 2
                }
                s.startsWith("<!", pos) -> {
                    var d = pos + 2
                    var depth = 0
                    while (d < n) {
                        if (s[d] == '>' && depth == 0) break
                        if (s[d] == '[') depth++
                        if (s[d] == ']') depth--
                        d++
                    }
                    if (d >= n) fail("unterminated declaration")
                    pos = d + 1
                }
                else -> return
            }
        }
    }

    fun parseElement(): XmlNode {
        // s[pos] == '<' and not a closing tag
        pos++ // consume '<'
        val name = readName()
        val attrs = LinkedHashMap<String, String>()
        while (true) {
            skipWs()
            if (s.startsWith("/>", pos)) {
                pos += 2
                return XmlNode(name, attrs, mutableListOf(), StringBuilder())
            }
            if (pos < n && s[pos] == '>') { pos++; break }
            if (pos >= n) fail("unterminated start tag <$name>")
            val an = readName()
            skipWs()
            if (pos >= n || s[pos] != '=') fail("expected '=' after attribute $an")
            pos++
            skipWs()
            if (pos >= n) fail("attribute value must be quoted")
            val q = s[pos]
            if (q != '"' && q != '\'') fail("attribute value must be quoted")
            pos++
            val v = StringBuilder()
            while (pos < n && s[pos] != q) { v.append(s[pos]); pos++ }
            if (pos >= n) fail("unterminated attribute value")
            pos++
            attrs[an] = unescapeXml(v.toString())
        }
        val children = mutableListOf<XmlNode>()
        val textAcc = StringBuilder()
        var seenChild = false
        while (true) {
            if (pos >= n) fail("unterminated element <$name>")
            if (s[pos] == '<') {
                when {
                    s.startsWith("</", pos) -> {
                        pos += 2
                        skipWs()
                        val cn = readName()
                        skipWs()
                        if (pos >= n || s[pos] != '>') fail("expected '>' in closing tag")
                        pos++
                        if (cn != name) fail("mismatched close tag </$cn> for <$name>")
                        return XmlNode(name, attrs, children, textAcc)
                    }
                    s.startsWith("<!--", pos) -> {
                        val e = s.indexOf("-->", pos + 4)
                        if (e < 0) fail("unterminated comment")
                        pos = e + 3
                    }
                    s.startsWith("<?", pos) -> {
                        val p = s.indexOf("?>", pos + 2)
                        if (p < 0) fail("unterminated processing instruction")
                        pos = p + 2
                    }
                    s.startsWith("<![CDATA[", pos) -> {
                        val c = s.indexOf("]]>", pos + 9)
                        if (c < 0) fail("unterminated CDATA section")
                        if (!seenChild) textAcc.append(s, pos + 9, c)
                        pos = c + 3
                    }
                    s.startsWith("<!", pos) -> fail("unexpected declaration inside element")
                    else -> {
                        children.add(parseElement())
                        seenChild = true
                    }
                }
            } else {
                val e2 = s.indexOf('<', pos)
                val t = if (e2 < 0) s.substring(pos) else s.substring(pos, e2)
                pos = if (e2 < 0) n else e2
                if (!seenChild) textAcc.append(t) // tail after children is ignored, like ET
            }
        }
    }
}

fun xmlValue(node: XmlNode): Any? {
    val d = LinkedHashMap<String, Any?>()
    for ((an, av) in node.attrs) d["@$an"] = av
    if (node.children.isEmpty()) {
        val t = unescapeXml(node.text.toString()).trim()
        if (node.attrs.isNotEmpty()) {
            if (t.isNotEmpty()) d["#text"] = t
            return d
        }
        return coerceLoose(t)
    }
    for (child in node.children) {
        val v = xmlValue(child)
        if (!d.containsKey(child.tag)) {
            d[child.tag] = v
        } else {
            val cur = d[child.tag]
            if (cur is MutableList<*>) {
                @Suppress("UNCHECKED_CAST")
                (cur as MutableList<Any?>).add(v)
            } else {
                d[child.tag] = mutableListOf(cur, v)
            }
        }
    }
    return d
}

fun parseXmlBody(bodyLines: List<String>): Any? {
    val text = bodyLines.joinToString("\n").trim()
    if (text.isEmpty()) return null
    val p = XmlParser(text)
    p.skipWs()
    p.skipMisc()
    p.skipWs()
    if (p.pos >= p.n || p.s[p.pos] != '<') p.fail("no root element")
    val root = p.parseElement()
    p.skipWs()
    p.skipMisc()
    p.skipWs()
    if (p.pos != p.n) p.fail("junk after root element")
    val wrapped = LinkedHashMap<String, Any?>()
    wrapped[root.tag] = xmlValue(root)
    return wrapped
}

/* ------------------------------------------------------------------ */
/* top-level                                                            */
/* ------------------------------------------------------------------ */

/** Parse a .dog document into its JSON-equivalent value. */
fun parse(text: String): Any? {
    val (directives, body) = splitHeader(text)
    val cfg = buildConfig(directives)
    return when (cfg.lang) {
        "json" -> parseJsonBody(body)
        "xml" -> parseXmlBody(body)
        "yaml" -> {
            // header tools are native-dog features; YAML bodies stay byte-identical
            val ycfg = cfg.copy(dict = emptyMap(), rep = emptyList(), keys = null)
            parseDogBody(body, ycfg) // common block-mapping subset
        }
        else -> parseDogBody(body, cfg)
    }
}

/* ------------------------------------------------------------------ */
/* CLI (mirrors dog.py)                                                 */
/* ------------------------------------------------------------------ */

fun main(args: Array<String>) {
    if (args.size != 1 || args[0] == "-h" || args[0] == "--help") {
        System.err.println("usage: dog file.dog")
        exitProcess(2)
    }
    val text = try {
        java.nio.file.Files.readString(java.nio.file.Path.of(args[0]), Charsets.UTF_8)
    } catch (e: Exception) {
        System.err.println("dog: cannot read ${args[0]}: ${e.message}")
        exitProcess(2)
    }
    val data = try {
        parse(text)
    } catch (e: DogError) {
        System.err.println("dog error: ${e.message}")
        exitProcess(1)
    }
    val sb = StringBuilder()
    jsonDump(data, 0, sb)
    println(sb.toString())
}
