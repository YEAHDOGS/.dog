#!/usr/bin/env node
/*
 * run-corpus.js -- conformance runner for the .dog parser fleet.
 *
 * For every tests/corpus/<name>.dog:
 *   - parses it with dog.js (the implementation under test)
 *   - parses it with dog.py (the reference implementation)
 *   - checks the result against <name>.expected.json, or against
 *     <name>.error (expected-error substring) for error cases
 *   - <name>.known-deviation skips the dog.py agreement check and prints
 *     the documented reason (used while implementations intentionally differ)
 *
 * A parser is not "done" until it passes 100% of the corpus.
 *
 * Usage: node tests/run-corpus.js   (run from the repo root)
 */
"use strict";

const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");

const ROOT = path.resolve(__dirname, "..");
const CORPUS = path.join(ROOT, "tests", "corpus");
const dog = require(path.join(ROOT, "dog.js"));

function deepEqual(a, b) {
  if (a === b) return true;
  if (typeof a !== typeof b) return false;
  if (a === null || b === null) return a === b;
  if (Array.isArray(a) || Array.isArray(b)) {
    if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length) return false;
    return a.every((v, i) => deepEqual(v, b[i]));
  }
  if (typeof a === "object") {
    const ka = Object.keys(a), kb = Object.keys(b);
    if (ka.length !== kb.length) return false;
    return ka.every((k) =>
      Object.prototype.hasOwnProperty.call(b, k) && deepEqual(a[k], b[k]));
  }
  // NaN: JSON has no NaN; treat as unequal unless both NaN
  return a !== a && b !== b;
}

const PY_DRIVER = `
import json, sys
sys.path.insert(0, ${JSON.stringify(ROOT)})
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
`;

function runPython(dogPath) {
  const r = spawnSync("python3", ["-c", PY_DRIVER, dogPath], { encoding: "utf8" });
  if (r.status !== 0) {
    return { ok: false, error: "python driver crashed: " + (r.stderr || "").trim() };
  }
  try {
    return JSON.parse(r.stdout);
  } catch (e) {
    return { ok: false, error: "python driver bad output: " + r.stdout };
  }
}

function runJs(dogPath) {
  try {
    return { ok: true, value: dog.parse(fs.readFileSync(dogPath, "utf8")) };
  } catch (e) {
    return { ok: false, error: String((e && e.message) || e) };
  }
}

const cases = fs.readdirSync(CORPUS)
  .filter((f) => f.endsWith(".dog"))
  .map((f) => f.slice(0, -4))
  .sort();

let passed = 0, failed = 0;
const failures = [];
const deviations = [];

for (const name of cases) {
  const dogPath = path.join(CORPUS, name + ".dog");
  const expPath = path.join(CORPUS, name + ".expected.json");
  const errPath = path.join(CORPUS, name + ".error");
  const devPath = path.join(CORPUS, name + ".known-deviation");
  const hasExpected = fs.existsSync(expPath);
  const hasError = fs.existsSync(errPath);
  const hasDeviation = fs.existsSync(devPath);
  const problems = [];

  if (!hasExpected && !hasError) {
    problems.push("case has neither .expected.json nor .error");
  }

  const js = runJs(dogPath);

  if (hasExpected) {
    const expected = JSON.parse(fs.readFileSync(expPath, "utf8"));
    if (!js.ok) problems.push("dog.js threw: " + js.error);
    else if (!deepEqual(js.value, expected)) {
      problems.push("dog.js value != expected: " + JSON.stringify(js.value));
    }
  }
  if (hasError) {
    const want = fs.readFileSync(errPath, "utf8").trim();
    if (js.ok) problems.push("dog.js should have thrown (want: " + want + ")");
    else if (!js.error.includes(want)) {
      problems.push("dog.js error missing " + JSON.stringify(want) + ": " + js.error);
    }
  }

  // Reference agreement -- skipped only with a documented known-deviation.
  if (hasDeviation) {
    deviations.push(name + ": " + fs.readFileSync(devPath, "utf8").trim());
  } else {
    const py = runPython(dogPath);
    if (hasExpected) {
      const expected = JSON.parse(fs.readFileSync(expPath, "utf8"));
      if (!py.ok) problems.push("dog.py threw: " + py.error);
      else if (!deepEqual(py.value, expected)) {
        problems.push("dog.py value != expected: " + JSON.stringify(py.value));
      }
    }
    if (hasError) {
      const want = fs.readFileSync(errPath, "utf8").trim();
      if (py.ok) problems.push("dog.py should have thrown (want: " + want + ")");
      else if (!py.error.includes(want)) {
        problems.push("dog.py error missing " + JSON.stringify(want) + ": " + py.error);
      }
    }
    // Cross-engine agreement on valid values (both already checked vs expected).
    if (hasExpected && js.ok && py.ok && !deepEqual(js.value, py.value)) {
      problems.push("dog.js and dog.py disagree: " +
        JSON.stringify(js.value) + " vs " + JSON.stringify(py.value));
    }
  }

  if (problems.length) {
    failed++;
    failures.push(name + "\n    " + problems.join("\n    "));
    console.log("FAIL " + name);
  } else {
    passed++;
    console.log("ok   " + name);
  }
}

console.log("\ncorpus: " + cases.length + " cases, " + passed + " passed, " + failed + " failed");
if (deviations.length) {
  console.log("\nknown deviations (documented, dog.py agreement skipped):");
  for (const d of deviations) console.log("  - " + d);
}
if (failures.length) {
  console.log("\nfailures:");
  for (const f of failures) console.log("  " + f);
}
process.exit(failed ? 1 : 0);
