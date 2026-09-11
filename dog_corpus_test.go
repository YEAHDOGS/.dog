// dog_corpus_test.go -- Go conformance runner for the .dog parser fleet.
//
// Mirrors tests/run-corpus.js: for every tests/corpus/<name>.dog it parses
// with dog.go (this package's Parse), checks the result against
// <name>.expected.json or the <name>.error substring, and -- for cases
// without a .known-deviation file -- also parses with dog.py (the reference
// implementation) and requires agreement. Run with: go test -run TestCorpus
package main

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

var pyDriver = `
import json, sys
sys.path.insert(0, ROOT)
from dog import parse, DogError
path = sys.argv[1]
try:
    with open(path, encoding="utf-8") as f:
        val = parse(f.read())
    print(json.dumps({"ok": True, "value": val}, ensure_ascii=False))
except DogError as e:
    print(json.dumps({"ok": False, "error": str(e)}, ensure_ascii=False))
except Exception as e:
    print(json.dumps({"ok": False, "error": "NON-DOG-ERROR " + repr(e)}, ensure_ascii=False))
`

func runPython(t *testing.T, root, dogPath string) (bool, interface{}, string) {
	t.Helper()
	drv := strings.Replace(pyDriver, "ROOT", strconv_ql(root), 1)
	cmd := exec.Command("python3", "-c", drv, dogPath)
	out, err := cmd.Output()
	if err != nil {
		return false, nil, "python driver crashed: " + err.Error()
	}
	var res struct {
		Ok    bool        `json:"ok"`
		Value interface{} `json:"value"`
		Error string      `json:"error"`
	}
	if jerr := json.Unmarshal(out, &res); jerr != nil {
		return false, nil, "python driver bad output: " + string(out)
	}
	return res.Ok, res.Value, res.Error
}

// strconv_ql quotes a string for embedding in the python driver.
func strconv_ql(s string) string {
	b, _ := json.Marshal(s)
	return string(b)
}

// canonical normalizes parsed values for cross-engine comparison:
// all numbers become float64 (JS/Python have a single number type).
func canonical(v interface{}) interface{} {
	switch x := v.(type) {
	case map[string]interface{}:
		m := make(map[string]interface{}, len(x))
		for k, vv := range x {
			m[k] = canonical(vv)
		}
		return m
	case []interface{}:
		a := make([]interface{}, len(x))
		for i, vv := range x {
			a[i] = canonical(vv)
		}
		return a
	case int64:
		return float64(x)
	case int:
		return float64(x)
	case float32:
		return float64(x)
	default:
		return v
	}
}

func deepEqual(a, b interface{}) bool {
	a, b = canonical(a), canonical(b)
	if a == nil || b == nil {
		return a == nil && b == nil
	}
	switch x := a.(type) {
	case map[string]interface{}:
		y, ok := b.(map[string]interface{})
		if !ok || len(x) != len(y) {
			return false
		}
		for k, v := range x {
			yv, ok := y[k]
			if !ok || !deepEqual(v, yv) {
				return false
			}
		}
		return true
	case []interface{}:
		y, ok := b.([]interface{})
		if !ok || len(x) != len(y) {
			return false
		}
		for i, v := range x {
			if !deepEqual(v, y[i]) {
				return false
			}
		}
		return true
	case float64:
		y, ok := b.(float64)
		return ok && x == y
	case string:
		y, ok := b.(string)
		return ok && x == y
	case bool:
		y, ok := b.(bool)
		return ok && x == y
	default:
		return false
	}
}

func TestCorpus(t *testing.T) {
	root, err := filepath.Abs(".")
	if err != nil {
		t.Fatal(err)
	}
	corpus := filepath.Join(root, "tests", "corpus")
	entries, err := os.ReadDir(corpus)
	if err != nil {
		t.Fatal(err)
	}
	var cases []string
	for _, e := range entries {
		if strings.HasSuffix(e.Name(), ".dog") {
			cases = append(cases, strings.TrimSuffix(e.Name(), ".dog"))
		}
	}
	sort.Strings(cases)
	if len(cases) != 39 {
		t.Errorf("corpus census: want 39 cases, found %d (pin the corpus, don't drift)", len(cases))
	}

	passed, failed := 0, 0
	for _, name := range cases {
		dogPath := filepath.Join(corpus, name+".dog")
		expPath := filepath.Join(corpus, name+".expected.json")
		errPath := filepath.Join(corpus, name+".error")
		devPath := filepath.Join(corpus, name+".known-deviation")
		_, hasExpected := fileExists(expPath)
		_, hasError := fileExists(errPath)
		devBytes, hasDeviation := fileExists(devPath)

		var problems []string
		if !hasExpected && !hasError {
			problems = append(problems, "case has neither .expected.json nor .error")
		}

		raw, rerr := os.ReadFile(dogPath)
		if rerr != nil {
			t.Fatalf("%s: cannot read input: %s", name, rerr)
		}
		val, gerr := Parse(string(raw))

		if hasExpected {
			expRaw, _ := os.ReadFile(expPath)
			var expected interface{}
			if jerr := json.Unmarshal(expRaw, &expected); jerr != nil {
				t.Fatalf("%s: bad expected.json: %s", name, jerr)
			}
			if gerr != nil {
				problems = append(problems, "dog.go threw: "+gerr.Error())
			} else if !deepEqual(val, expected) {
				problems = append(problems, "dog.go value != expected: "+jsonOneLine(val))
			}
		}
		if hasError {
			wantRaw, _ := os.ReadFile(errPath)
			want := strings.TrimSpace(string(wantRaw))
			if gerr == nil {
				problems = append(problems, "dog.go should have thrown (want: "+want+")")
			} else if !strings.Contains(gerr.Error(), want) {
				problems = append(problems, "dog.go error missing "+strconv_ql(want)+": "+gerr.Error())
			}
		}

		// Reference agreement -- skipped only with a documented known-deviation.
		if hasDeviation {
			t.Logf("known deviation %s: %s", name, strings.TrimSpace(string(devBytes)))
		} else {
			pyOk, pyVal, pyErr := runPython(t, root, dogPath)
			if hasExpected {
				expRaw, _ := os.ReadFile(expPath)
				var expected interface{}
				_ = json.Unmarshal(expRaw, &expected)
				if !pyOk {
					problems = append(problems, "dog.py threw: "+pyErr)
				} else if !deepEqual(pyVal, expected) {
					problems = append(problems, "dog.py value != expected: "+jsonOneLine(pyVal))
				}
			}
			if hasError {
				wantRaw, _ := os.ReadFile(errPath)
				want := strings.TrimSpace(string(wantRaw))
				if pyOk {
					problems = append(problems, "dog.py should have thrown (want: "+want+")")
				} else if !strings.Contains(pyErr, want) {
					problems = append(problems, "dog.py error missing "+strconv_ql(want)+": "+pyErr)
				}
			}
			if hasExpected && gerr == nil && pyOk && !deepEqual(val, pyVal) {
				problems = append(problems, "dog.go and dog.py disagree: "+
					jsonOneLine(val)+" vs "+jsonOneLine(pyVal))
			}
		}

		if len(problems) > 0 {
			failed++
			t.Errorf("FAIL %s\n    %s", name, strings.Join(problems, "\n    "))
		} else {
			passed++
			t.Logf("ok   %s", name)
		}
	}
	t.Logf("corpus: %d cases, %d passed, %d failed", len(cases), passed, failed)
}

func fileExists(p string) ([]byte, bool) {
	b, err := os.ReadFile(p)
	return b, err == nil
}

func jsonOneLine(v interface{}) string {
	b, _ := json.Marshal(v)
	return string(b)
}
