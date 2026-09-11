#!/usr/bin/env python3
"""Tests for dog.py -- run with:  python3 test_dog.py"""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dog import parse, DogError  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
EX = os.path.join(HERE, "examples")

passed = failed = 0


def read(name):
    with open(os.path.join(EX, name), encoding="utf-8") as f:
        return f.read()


def check(name, fn):
    global passed, failed
    try:
        fn()
    except AssertionError as e:
        failed += 1
        print("FAIL %s: %s" % (name, e))
    except Exception as e:  # noqa: BLE001
        failed += 1
        print("ERROR %s: %r" % (name, e))
    else:
        passed += 1
        print("ok   %s" % name)


USERS = [
    {"name": "Ava Reyes", "email": "ava.reyes@example.com",
     "role": "admin", "active": True},
    {"name": "Ben Okafor", "email": "ben.okafor@example.com",
     "role": "member", "active": True},
    {"name": "Cy Lindqvist", "email": "cy.lindqvist@example.com",
     "role": "member", "active": True},
    {"name": "Dee Park", "email": "dee.park@example.com",
     "role": "member", "active": False},
    {"name": "Eli Vance", "email": "eli.vance@example.com",
     "role": "moderator", "active": True},
    {"name": "Fay Hassan", "email": "fay.hassan@example.com",
     "role": "member", "active": True},
]


def t_json_variation():
    data = parse(read("json-variation.dog"))
    assert data == USERS, "json body must parse to the sample data"


def t_json_body_byte_identical():
    dog_text = read("json-variation.dog")
    body = dog_text.split("\n\n", 1)[1]
    assert body.rstrip("\n") == read("minimal.json").rstrip("\n"), \
        "json-variation body must be byte-identical to minimal.json"


def t_minimal_rows_roundtrip():
    assert parse(read("minimal.dog")) == USERS


def t_yaml_variation():
    assert parse(read("yaml-variation.dog")) == USERS


def t_xml_variation():
    data = parse(read("xml-variation.dog"))
    users = data["users"]["user"]
    assert len(users) == 6
    assert users[0]["name"] == "Ava Reyes"
    assert users[0]["@active"] == "true"  # XML attributes stay strings
    assert users[3]["@active"] == "false"
    assert users[4]["role"] == "moderator"


def t_nested_native():
    data = parse(read("nested.dog"))
    srv = data["server"]
    assert srv["name"] == "castle"                      # dict: expansion
    assert srv["endpoints"] == [                        # rep: in list items
        "https://dogs.example/health",
        "https://dogs.example/api/v1",
    ]
    assert srv["limits"] == {"cpu": 4, "mem": "16GB"}   # int vs string
    assert srv["note"] == "Runs behind the modem.\nNo cookies. No banners."


def t_rep_escape():
    doc = ".dog/1.0\nrep: ~=https://x\n\nk: \\~literal and ~expanded\n"
    data = parse(doc)
    assert data == {"k": "~literal and https://xexpanded"}, data
    doc2 = ".dog/1.0\nrep: ~=https://x\n\nk: \\\\~x\n"
    assert parse(doc2) == {"k": "\\https://xx"}, parse(doc2)


def t_dict_unknown_alias_kept():
    doc = ".dog/1.0\ndict: n=name\n\nn: 1\nzzz: 2\n"
    assert parse(doc) == {"name": 1, "zzz": 2}


def t_keys_with_dict_aliases():
    doc = (".dog/1.0\nlang: dog\ndict: n=name\nkeys: n\n\n"
           "Ava\nBen\n")
    assert parse(doc) == [{"name": "Ava"}, {"name": "Ben"}]


def t_scalar_coercions():
    doc = ".dog/1.0\n\ns1: 42\ns2: 3.5\ns3: true\ns4: null\ns5: \"42\"\n"
    d = parse(doc)
    assert d == {"s1": 42, "s2": 3.5, "s3": True, "s4": None, "s5": "42"}, d


def t_missing_magic():
    try:
        parse('{"a": 1}\n')
    except DogError:
        return
    raise AssertionError("missing magic must raise DogError")


def t_row_field_mismatch():
    try:
        parse(".dog/1.0\nkeys: a b\n\n1|2|3\n")
    except DogError:
        return
    raise AssertionError("field-count mismatch must raise DogError")


def t_bad_indent():
    try:
        parse(".dog/1.0\n\nkey:\n\tbad: 1\n")
    except DogError:
        return
    raise AssertionError("tab indentation must raise DogError")


def t_byte_savings():
    j = os.path.getsize(os.path.join(EX, "minimal.json"))
    d = os.path.getsize(os.path.join(EX, "minimal.dog"))
    assert d < j * 0.7, "dog file should be <70%% of pretty JSON (%d vs %d)" % (d, j)
    m = len(json.dumps(USERS, separators=(",", ":")).encode())
    assert d < m, "dog file should beat even minified JSON (%d vs %d)" % (d, m)


def t_cli_emits_json():
    p = subprocess.run([sys.executable, os.path.join(HERE, "dog.py"),
                        os.path.join(EX, "minimal.dog")],
                       capture_output=True, text=True)
    assert p.returncode == 0, p.stderr
    assert json.loads(p.stdout) == USERS


def t_cli_bad_file_fails():
    p = subprocess.run([sys.executable, os.path.join(HERE, "dog.py"),
                        os.path.join(EX, "minimal.json")],
                       capture_output=True, text=True)
    assert p.returncode != 0  # no magic line -> DogError -> exit 1


for name, fn in sorted([(k[2:], v) for k, v in list(globals().items())
                        if k.startswith("t_") and callable(v)]):
    check(name, fn)

print("\n%d passed, %d failed" % (passed, failed))
sys.exit(1 if failed else 0)
