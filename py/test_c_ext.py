"""Parity tests — C extension vs pure-Python reader vs JSON.

Run: python3 test_c_ext.py [fixtures_dir]
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import _nxs
from nxs import NxsReader


def main() -> int:
    fixture_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "../js/fixtures")
    nxb = (fixture_dir / "records_1000.nxb").read_bytes()
    js = json.loads((fixture_dir / "records_1000.json").read_text())

    passed = failed = 0

    def case(name, fn):
        nonlocal passed, failed
        try:
            fn()
            print(f"  ✓ {name}")
            passed += 1
        except Exception as e:
            print(f"  ✗ {name}\n      {e}")
            failed += 1

    print("\nC extension vs pure-Python parity\n")

    py_reader = NxsReader(nxb)
    c_reader = _nxs.Reader(nxb)

    def same_count():
        assert py_reader.record_count == c_reader.record_count == 1000

    def same_keys():
        assert list(py_reader.keys) == list(c_reader.keys)

    def all_i64_match():
        for i in (0, 1, 42, 500, 999):
            py_val = py_reader.record(i).get_i64("id")
            c_val = c_reader.record(i).get_i64("id")
            js_val = js[i]["id"]
            assert py_val == c_val == js_val, (i, py_val, c_val, js_val)

    def all_str_match():
        for i in (0, 7, 42, 500, 999):
            py_val = py_reader.record(i).get_str("username")
            c_val = c_reader.record(i).get_str("username")
            js_val = js[i]["username"]
            assert py_val == c_val == js_val, (i, py_val, c_val, js_val)

    def all_f64_match():
        for i in (0, 42, 500, 999):
            py_val = py_reader.record(i).get_f64("score")
            c_val = c_reader.record(i).get_f64("score")
            js_val = js[i]["score"]
            assert math.isclose(py_val, c_val), (py_val, c_val)
            assert math.isclose(c_val, js_val, rel_tol=1e-6), (c_val, js_val)

    def all_bool_match():
        for i in (0, 1, 2, 42, 500, 999):
            py_val = py_reader.record(i).get_bool("active")
            c_val = c_reader.record(i).get_bool("active")
            js_val = js[i]["active"]
            assert py_val == c_val == js_val, (i, py_val, c_val, js_val)

    def missing_key_is_none():
        o = c_reader.record(0)
        assert o.get_str("nonexistent") is None

    def oob_raises_indexerror():
        try:
            c_reader.record(1_000_000)
        except IndexError:
            return
        raise AssertionError("expected IndexError")

    def full_scan_sums_equal():
        py_sum = sum(py_reader.record(i).get_f64("score") for i in range(1000))
        c_sum = sum(c_reader.record(i).get_f64("score") for i in range(1000))
        json_sum = sum(r["score"] for r in js)
        assert math.isclose(py_sum, c_sum), (py_sum, c_sum)
        assert math.isclose(c_sum, json_sum, rel_tol=1e-6), (c_sum, json_sum)

    case("record_count matches", same_count)
    case("schema keys match", same_keys)
    case("i64 field reads match", all_i64_match)
    case("str field reads match", all_str_match)
    case("f64 field reads match", all_f64_match)
    case("bool field reads match", all_bool_match)
    case("missing key returns None", missing_key_is_none)
    case("out-of-bounds raises IndexError", oob_raises_indexerror)
    case("full-scan sums match", full_scan_sums_equal)

    def col_buffer_columnar():
        candidates = [
            fixture_dir.parent.parent / "conformance/columnar_flat8_dense_100.nxb",
            Path(__file__).resolve().parent / "../../nyxis/conformance/columnar_flat8_dense_100.nxb",
            fixture_dir / "../../conformance/columnar_flat8_dense_100.nxb",
        ]
        path = next((p for p in candidates if p.is_file()), None)
        if path is None:
            print("      (skip: columnar_flat8_dense_100.nxb not found)")
            return
        data = path.read_bytes()
        r = _nxs.Reader(data)
        assert r.layout == "columnar"
        buf = r.col_buffer("score")
        assert buf["count"] == 100
        assert len(buf["values"]) == 100 * 8
        assert len(buf["bitmap"]) >= 13
        s = r.col_sum_f64("score")
        assert math.isclose(s, 2475.0, rel_tol=1e-6)

    def col_numpy_columnar():
        try:
            import numpy as np  # noqa: F401
        except ImportError:
            print("      (skip: numpy not installed)")
            return
        candidates = [
            fixture_dir.parent.parent / "conformance/columnar_flat8_dense_100.nxb",
            Path(__file__).resolve().parent / "../../nyxis/conformance/columnar_flat8_dense_100.nxb",
        ]
        path = next((p for p in candidates if p.is_file()), None)
        if path is None:
            return
        r = _nxs.Reader(path.read_bytes())
        arr = r.col_numpy_f64("score")
        assert arr.shape == (100,)
        assert math.isclose(float(arr.sum()), 2475.0, rel_tol=1e-6)

    case("columnar col_buffer + col_sum_f64", col_buffer_columnar)
    case("columnar col_numpy_f64", col_numpy_columnar)

    print(f"\n{passed} passed, {failed} failed\n")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
