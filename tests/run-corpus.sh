#!/usr/bin/env bash
# tests/run-corpus.sh -- conformance runner for dog.c (the C parser).
#
# For every tests/corpus/<name>.dog:
#   - parses it with the compiled dog binary
#   - parses it with dog.py (the reference implementation)
#   - checks the result against <name>.expected.json (semantic JSON
#     comparison), or against <name>.error (stderr must contain the
#     expected substring) for error cases
#   - <name>.known-deviation skips the dog.py agreement check and prints
#     the documented reason (the two truncated-directive cases: dog.c
#     rejects them as hard errors per the fleet-wide hard rule, while
#     dog.py silently ignores them pending the founder's D1 ruling)
#
# A parser is not "done" until it passes 100% of the corpus.
#
# Usage: bash tests/run-corpus.sh   (run from the repo root)

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
BIN="$TMPDIR/dogc"

echo "== compiling dog.c (gcc -Wall -Wextra, zero warnings tolerated)"
if ! gcc -Wall -Wextra -O2 -std=c11 dog.c -o "$BIN" 2> "$TMPDIR/warnings.txt"; then
    echo "COMPILATION FAILED"
    cat "$TMPDIR/warnings.txt"
    exit 1
fi
if [ -s "$TMPDIR/warnings.txt" ]; then
    echo "COMPILER WARNINGS (must be zero):"
    cat "$TMPDIR/warnings.txt"
    exit 1
fi
echo "== compiled clean, zero warnings"

passed=0
failed=0
deviations=()

for dogfile in tests/corpus/*.dog; do
    name="$(basename "$dogfile" .dog)"
    "$BIN" "$dogfile" > "$TMPDIR/c.out" 2> "$TMPDIR/c.err"
    cst=$?
    if python3 - "$ROOT" "$name" "$dogfile" "$TMPDIR/c.out" "$TMPDIR/c.err" "$cst" <<'PYEOF'; then
import json, os, sys
root, name, dogfile, cout_path, cerr_path, cst = sys.argv[1:7]
cst = int(cst)
sys.path.insert(0, root)
from dog import parse, DogError
corpus = os.path.join(root, "tests", "corpus")
exp_path = os.path.join(corpus, name + ".expected.json")
err_path = os.path.join(corpus, name + ".error")
dev_path = os.path.join(corpus, name + ".known-deviation")
problems = []
c_out = open(cout_path, encoding="utf-8").read()
c_err = open(cerr_path, encoding="utf-8").read()
UNSET = ("unset",)
c_val = UNSET

if os.path.exists(exp_path):
    expected = json.load(open(exp_path, encoding="utf-8"))
    if cst != 0:
        problems.append("dog.c exited %d: %s" % (cst, c_err.strip()[:200]))
    else:
        try:
            c_val = json.loads(c_out)
        except Exception as e:
            problems.append("dog.c stdout is not JSON: %s" % e)
        else:
            if c_val != expected:
                problems.append("dog.c value != expected: %r" % (c_val,))
    if not os.path.exists(dev_path):
        try:
            py_val = parse(open(dogfile, encoding="utf-8").read())
            py_ok, py_err = True, ""
        except DogError as e:
            py_ok, py_err = False, str(e)
        except Exception as e:
            py_ok, py_err = False, "NON-DOG-ERROR " + repr(e)
        if not py_ok:
            problems.append("dog.py threw: " + py_err)
        elif py_val != expected:
            problems.append("dog.py value != expected: %r" % (py_val,))
        elif c_val is not UNSET and py_ok and c_val != py_val:
            problems.append("dog.c and dog.py disagree: %r vs %r" % (c_val, py_val))
else:
    want = open(err_path, encoding="utf-8").read().strip()
    if cst == 0:
        problems.append("dog.c should have errored (want: %s)" % want)
    elif want not in c_err:
        problems.append("dog.c stderr missing %r: %s" % (want, c_err.strip()[:200]))
    if not os.path.exists(dev_path):
        try:
            parse(open(dogfile, encoding="utf-8").read())
            py_ok, py_err = True, ""
        except DogError as e:
            py_ok, py_err = False, str(e)
        except Exception as e:
            py_ok, py_err = False, "NON-DOG-ERROR " + repr(e)
        if py_ok:
            problems.append("dog.py should have thrown (want: %s)" % want)
        elif want not in py_err:
            problems.append("dog.py error missing %r: %s" % (want, py_err))

if problems:
    print("FAIL " + name)
    for pr in problems:
        print("    " + pr)
    sys.exit(1)
print("ok   " + name)
PYEOF
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
    fi

    if [ -f "tests/corpus/$name.known-deviation" ]; then
        deviations+=("$name: $(tr '\n' ' ' < "tests/corpus/$name.known-deviation")")
    fi
done

total=$((passed + failed))
echo ""
echo "corpus: $total cases, $passed passed, $failed failed"
if [ "${#deviations[@]}" -gt 0 ]; then
    echo ""
    echo "known deviations (documented, dog.py agreement skipped):"
    for d in "${deviations[@]}"; do
        echo "  - $d"
    done
fi
[ "$failed" -eq 0 ]
