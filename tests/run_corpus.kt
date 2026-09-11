// run_corpus.kt -- Kotlin conformance runner for the .dog parser fleet.
//
// Mirrors tests/run-corpus.js and tests/run_corpus.rs: for every
// tests/corpus/<name>.dog it parses with dog.kt (this build's `parse`),
// checks the result against <name>.expected.json or the <name>.error
// substring, and -- for cases without a .known-deviation file -- also parses
// with dog.py (the reference implementation) and requires agreement.
//
// Build & run from the repo root:
//
//     kotlinc dog.kt tests/run_corpus.kt -include-runtime -d /tmp/dog_kt.jar
//     java -cp /tmp/dog_kt.jar Run_corpusKt
//
// A parser is not "done" until it passes 100% of the corpus.

import java.io.File
import kotlin.system.exitProcess

fun pyDriver(root: String): String {
    val rootLit = "'" + root.replace("\\", "\\\\").replace("'", "\\'") + "'"
    return """
import json, sys
sys.path.insert(0, $rootLit)
from dog import parse, DogError
path = sys.argv[1]
try:
    with open(path, encoding="utf-8") as f:
        val = parse(f.read())
    print(json.dumps({"ok": True, "value": val}, ensure_ascii=False))
except DogError as e:
    print(json.dumps({"ok": False, "error": str(e)}, ensure_ascii=False))
except Exception as e:  # a non-DogError is itself a divergence
    print(json.dumps({"ok": False, "error": "NON-DOG-ERROR " + repr(e)}, ensure_ascii=False))
""".trimIndent()
}

/// deepEqual with JS/Python-style number semantics: all numbers compare by
/// value across Long/Double (like run_corpus.rs's canonicalization).
fun deepEqual(a: Any?, b: Any?): Boolean {
    if (a is Number && b is Number) {
        if (a is Long && b is Long) return a == b
        if (a is Double && b is Double) return a == b || (a.isNaN() && b.isNaN())
        val x = a.toDouble()
        val y = b.toDouble()
        return x == y || (x.isNaN() && y.isNaN())
    }
    if (a == null || b == null) return a == b
    if (a is List<*> && b is List<*>) {
        if (a.size != b.size) return false
        return a.zip(b).all { (x, y) -> deepEqual(x, y) }
    }
    if (a is Map<*, *> && b is Map<*, *>) {
        if (a.size != b.size) return false
        return a.entries.all { (k, v) -> b.containsKey(k) && deepEqual(v, b[k]) }
    }
    return a == b
}

fun compact(v: Any?): String {
    val sb = StringBuilder()
    jsonDump(v, 0, sb)
    return sb.toString().replace(Regex("\\s+"), " ").take(220)
}

sealed class Outcome {
    data class Ok(val value: Any?) : Outcome()
    data class Err(val error: String) : Outcome()
}

fun runKotlin(dogPath: String): Outcome {
    return try {
        Outcome.Ok(parse(File(dogPath).readText(Charsets.UTF_8)))
    } catch (e: DogError) {
        Outcome.Err(e.message ?: "")
    } catch (e: Exception) {
        Outcome.Err("NON-DOG-ERROR " + e.toString())
    }
}

fun runPython(root: String, dogPath: String): Outcome {
    return try {
        val proc = ProcessBuilder("python3", "-c", pyDriver(root), dogPath)
            .redirectErrorStream(false)
            .start()
        val stdout = proc.inputStream.bufferedReader().readText()
        val code = proc.waitFor()
        if (code != 0) return Outcome.Err("python driver crashed (exit $code)")
        val m = parseJsonValue(stdout.trim()) as? Map<*, *>
            ?: return Outcome.Err("python driver bad output: $stdout")
        if (m["ok"] == true) Outcome.Ok(m["value"]) else Outcome.Err(m["error"].toString())
    } catch (e: Exception) {
        Outcome.Err("python driver crashed: ${e.message}")
    }
}

fun main() {
    val root = File(System.getProperty("user.dir")).canonicalPath
    val corpus = File(root, "tests/corpus")
    if (!corpus.isDirectory) {
        System.err.println("run from the repo root: tests/corpus not found under $root")
        exitProcess(2)
    }
    val cases = corpus.listFiles { f -> f.name.endsWith(".dog") }!!
        .map { it.name.dropLast(4) }
        .sorted()

    var passed = 0
    var failed = 0
    val failures = mutableListOf<String>()
    val deviations = mutableListOf<String>()

    for (name in cases) {
        val dogPath = File(corpus, "$name.dog").path
        val expFile = File(corpus, "$name.expected.json")
        val errFile = File(corpus, "$name.error")
        val devFile = File(corpus, "$name.known-deviation")
        val hasExpected = expFile.exists()
        val hasError = errFile.exists()
        val hasDeviation = devFile.exists()
        val problems = mutableListOf<String>()

        if (!hasExpected && !hasError) {
            problems.add("case has neither .expected.json nor .error")
        }

        val kt = runKotlin(dogPath)

        if (hasExpected) {
            val expected = parseJsonValue(expFile.readText(Charsets.UTF_8))
            when (kt) {
                is Outcome.Err -> problems.add("dog.kt threw: ${kt.error}")
                is Outcome.Ok -> if (!deepEqual(kt.value, expected)) {
                    problems.add("dog.kt value != expected: ${compact(kt.value)}")
                }
            }
        }
        if (hasError) {
            val want = errFile.readText(Charsets.UTF_8).trim()
            when (kt) {
                is Outcome.Ok -> problems.add("dog.kt should have thrown (want: $want)")
                is Outcome.Err -> if (!kt.error.contains(want)) {
                    problems.add("dog.kt error missing \"$want\": ${kt.error}")
                }
            }
        }

        // Reference agreement -- skipped only with a documented known-deviation.
        if (hasDeviation) {
            deviations.add("$name: ${devFile.readText(Charsets.UTF_8).trim()}")
        } else {
            val py = runPython(root, dogPath)
            if (hasExpected) {
                val expected = parseJsonValue(expFile.readText(Charsets.UTF_8))
                when (py) {
                    is Outcome.Err -> problems.add("dog.py threw: ${py.error}")
                    is Outcome.Ok -> if (!deepEqual(py.value, expected)) {
                        problems.add("dog.py value != expected: ${compact(py.value)}")
                    }
                }
            }
            if (hasError) {
                val want = errFile.readText(Charsets.UTF_8).trim()
                when (py) {
                    is Outcome.Ok -> problems.add("dog.py should have thrown (want: $want)")
                    is Outcome.Err -> if (!py.error.contains(want)) {
                        problems.add("dog.py error missing \"$want\": ${py.error}")
                    }
                }
            }
            // Cross-engine agreement on valid values (both already checked vs expected).
            if (hasExpected && kt is Outcome.Ok && py is Outcome.Ok && !deepEqual(kt.value, py.value)) {
                problems.add("dog.kt and dog.py disagree: ${compact(kt.value)} vs ${compact(py.value)}")
            }
        }

        if (problems.isNotEmpty()) {
            failed++
            failures.add(name + "\n    " + problems.joinToString("\n    "))
            println("FAIL $name")
        } else {
            passed++
            println("ok   $name")
        }
    }

    println("\ncorpus: ${cases.size} cases, $passed passed, $failed failed")
    if (deviations.isNotEmpty()) {
        println("\nknown deviations (documented, dog.py agreement skipped):")
        for (d in deviations) println("  - $d")
    }
    if (failures.isNotEmpty()) {
        println("\nfailures:")
        for (f in failures) println("  $f")
    }
    exitProcess(if (failed > 0) 1 else 0)
}
