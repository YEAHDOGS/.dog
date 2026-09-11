// run_corpus.rs -- Rust conformance runner for the .dog parser fleet.
//
// Mirrors tests/run-corpus.js and dog_corpus_test.go: for every
// tests/corpus/<name>.dog it parses with dog.rs (this crate's `parse`),
// checks the result against <name>.expected.json or the <name>.error
// substring, and -- for cases without a .known-deviation file -- also parses
// with dog.py (the reference implementation) and requires agreement.
//
// Build & run from the repo root:
//
//     rustc --edition 2021 --test -O tests/run_corpus.rs -o /tmp/dog_rs_test
//     /tmp/dog_rs_test --nocapture
//
// A parser is not "done" until it passes 100% of the corpus.

#[path = "../dog.rs"]
mod dog;

use dog::{parse, parse_json, DogError, Value};
use std::process::Command;

const PY_DRIVER: &str = r#"
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
except Exception as e:  # a non-DogError is itself a divergence
    print(json.dumps({"ok": False, "error": "NON-DOG-ERROR " + repr(e)}, ensure_ascii=False))
"#;

/// canonical normalizes parsed values for cross-engine comparison:
/// all numbers become Float (JS/Python have a single number type).
fn canonical(v: &Value) -> Value {
    match v {
        Value::Int(n) => Value::Float(*n as f64),
        Value::Array(a) => Value::Array(a.iter().map(canonical).collect()),
        Value::Object(o) => Value::Object(
            o.iter()
                .map(|(k, vv)| (k.clone(), canonical(vv)))
                .collect(),
        ),
        _ => v.clone(),
    }
}

fn deep_equal(a: &Value, b: &Value) -> bool {
    let a = canonical(a);
    let b = canonical(b);
    match (&a, &b) {
        (Value::Null, Value::Null) => true,
        (Value::Bool(x), Value::Bool(y)) => x == y,
        (Value::Float(x), Value::Float(y)) => x == y || (x.is_nan() && y.is_nan()),
        (Value::Str(x), Value::Str(y)) => x == y,
        (Value::Array(x), Value::Array(y)) => {
            x.len() == y.len() && x.iter().zip(y.iter()).all(|(p, q)| deep_equal(p, q))
        }
        (Value::Object(x), Value::Object(y)) => {
            x.len() == y.len()
                && x.iter().all(|(k, v)| {
                    y.iter()
                        .find(|(ek, _)| ek == k)
                        .map_or(false, |(_, ev)| deep_equal(v, ev))
                })
        }
        _ => false,
    }
}

fn json_one_line(v: &Value) -> String {
    // compact debug rendering for failure messages
    match v {
        Value::Null => "null".to_string(),
        Value::Bool(b) => b.to_string(),
        Value::Int(n) => n.to_string(),
        Value::Float(f) => f.to_string(),
        Value::Str(s) => format!("{:?}", s),
        Value::Array(a) => {
            format!(
                "[{}]",
                a.iter().map(json_one_line).collect::<Vec<_>>().join(",")
            )
        }
        Value::Object(o) => format!(
            "{{{}}}",
            o.iter()
                .map(|(k, vv)| format!("{:?}:{}", k, json_one_line(vv)))
                .collect::<Vec<_>>()
                .join(",")
        ),
    }
}

fn obj_get<'a>(o: &'a [(String, Value)], k: &str) -> Option<&'a Value> {
    o.iter().find(|(ek, _)| ek == k).map(|(_, v)| v)
}

fn run_python(root: &str, dog_path: &str) -> (bool, Option<Value>, String) {
    let drv = PY_DRIVER.replacen("ROOT", &format!("{:?}", root), 1);
    let out = Command::new("python3")
        .arg("-c")
        .arg(&drv)
        .arg(dog_path)
        .output();
    let out = match out {
        Ok(o) if o.status.success() => o,
        _ => return (false, None, "python driver crashed".to_string()),
    };
    let stdout = String::from_utf8_lossy(&out.stdout);
    let res = match parse_json(stdout.trim()) {
        Ok(v) => v,
        Err(e) => return (false, None, format!("python driver bad output: {}", e)),
    };
    let o = match res {
        Value::Object(o) => o,
        _ => return (false, None, "python driver bad output shape".to_string()),
    };
    let ok = matches!(obj_get(&o, "ok"), Some(Value::Bool(true)));
    let val = obj_get(&o, "value").cloned();
    let err = match obj_get(&o, "error") {
        Some(Value::Str(s)) => s.clone(),
        _ => String::new(),
    };
    (ok, val, err)
}

#[test]
fn corpus() {
    let root = std::env::current_dir()
        .unwrap()
        .to_string_lossy()
        .to_string();
    let corpus = format!("{}/tests/corpus", root);
    let mut cases: Vec<String> = std::fs::read_dir(&corpus)
        .expect("cannot read tests/corpus (run from the repo root)")
        .filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().to_string())
        .filter(|n| n.ends_with(".dog"))
        .map(|n| n.trim_end_matches(".dog").to_string())
        .collect();
    cases.sort();

    let mut census_problems: Vec<String> = Vec::new();
    if cases.len() != 39 {
        census_problems.push(format!(
            "corpus census: want 39 cases, found {} (pin the corpus, don't drift)",
            cases.len()
        ));
    }

    let mut passed = 0;
    let mut failed = 0;
    let mut failures: Vec<String> = Vec::new();
    let mut deviations: Vec<String> = Vec::new();

    for name in &cases {
        let dog_path = format!("{}/{}.dog", corpus, name);
        let exp_path = format!("{}/{}.expected.json", corpus, name);
        let err_path = format!("{}/{}.error", corpus, name);
        let dev_path = format!("{}/{}.known-deviation", corpus, name);
        let has_expected = std::path::Path::new(&exp_path).exists();
        let has_error = std::path::Path::new(&err_path).exists();
        let has_deviation = std::path::Path::new(&dev_path).exists();

        let mut problems: Vec<String> = Vec::new();
        if !has_expected && !has_error {
            problems.push("case has neither .expected.json nor .error".to_string());
        }

        let raw = std::fs::read(&dog_path).expect("cannot read corpus input");
        let text = String::from_utf8_lossy(&raw);
        let res: Result<Value, DogError> = parse(&text);

        if has_expected {
            let exp_raw = std::fs::read(&exp_path).expect("cannot read expected.json");
            let expected =
                parse_json(String::from_utf8_lossy(&exp_raw).trim()).expect("bad expected.json");
            match &res {
                Err(e) => problems.push(format!("dog.rs threw: {}", e)),
                Ok(v) if !deep_equal(v, &expected) => problems.push(format!(
                    "dog.rs value != expected: {}",
                    json_one_line(v)
                )),
                _ => {}
            }
        }
        if has_error {
            let want = std::fs::read_to_string(&err_path)
                .expect("cannot read .error")
                .trim()
                .to_string();
            match &res {
                Ok(_) => problems.push(format!("dog.rs should have thrown (want: {})", want)),
                Err(e) if !e.to_string().contains(&want) => problems.push(format!(
                    "dog.rs error missing {:?}: {}",
                    want,
                    e
                )),
                _ => {}
            }
        }

        // Reference agreement -- skipped only with a documented known-deviation.
        if has_deviation {
            let note = std::fs::read_to_string(&dev_path)
                .unwrap_or_default()
                .trim()
                .to_string();
            deviations.push(format!("{}: {}", name, note));
        } else {
            let (py_ok, py_val, py_err) = run_python(&root, &dog_path);
            if has_expected {
                let exp_raw = std::fs::read(&exp_path).expect("cannot read expected.json");
                let expected = parse_json(String::from_utf8_lossy(&exp_raw).trim())
                    .expect("bad expected.json");
                if !py_ok {
                    problems.push(format!("dog.py threw: {}", py_err));
                } else if let Some(pv) = &py_val {
                    if !deep_equal(pv, &expected) {
                        problems.push(format!(
                            "dog.py value != expected: {}",
                            json_one_line(pv)
                        ));
                    }
                }
            }
            if has_error {
                let want = std::fs::read_to_string(&err_path)
                    .expect("cannot read .error")
                    .trim()
                    .to_string();
                if py_ok {
                    problems.push(format!("dog.py should have thrown (want: {})", want));
                } else if !py_err.contains(&want) {
                    problems.push(format!(
                        "dog.py error missing {:?}: {}",
                        want, py_err
                    ));
                }
            }
            if has_expected {
                if let (Ok(v), Some(pv)) = (&res, &py_val) {
                    if py_ok && !deep_equal(v, pv) {
                        problems.push(format!(
                            "dog.rs and dog.py disagree: {} vs {}",
                            json_one_line(v),
                            json_one_line(pv)
                        ));
                    }
                }
            }
        }

        if problems.is_empty() {
            passed += 1;
            println!("ok   {}", name);
        } else {
            failed += 1;
            failures.push(format!("FAIL {}\n    {}", name, problems.join("\n    ")));
            println!("FAIL {}", name);
        }
    }

    println!(
        "\ncorpus: {} cases, {} passed, {} failed",
        cases.len(),
        passed,
        failed
    );
    if !deviations.is_empty() {
        println!("\nknown deviations (documented, dog.py agreement skipped):");
        for d in &deviations {
            println!("  - {}", d.lines().next().unwrap_or(""));
        }
    }

    let mut all: Vec<String> = census_problems;
    all.extend(failures);
    assert!(
        all.is_empty(),
        "\nconformance failures:\n  {}",
        all.join("\n  ")
    );
}
