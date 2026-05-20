"""Smoke tests for the Python NXS reader and writer.

Run: python3 test_nxs.py [fixtures_dir]
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path

from nxs import NxsReader, NxsError, Eq, Gt, Query
from nxs_writer import NxsSchema, NxsWriter


def main() -> int:
    fixture_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "../js/fixtures")
    nxb_path = fixture_dir / "records_1000.nxb"
    json_path = fixture_dir / "records_1000.json"

    if not nxb_path.exists():
        print(f"fixtures not found at {fixture_dir}")
        print("generate them first:  cargo run --release --bin gen_fixtures -- js/fixtures")
        return 1

    buf = nxb_path.read_bytes()
    js = json.loads(json_path.read_text())

    passed = failed = 0
    print("\nNXS Python Reader — Tests\n")

    def case(name, fn):
        nonlocal passed, failed
        try:
            fn()
            print(f"  ✓ {name}")
            passed += 1
        except Exception as e:
            print(f"  ✗ {name}\n      {e}")
            failed += 1

    # ── Tests ──────────────────────────────────────────────────────────────
    def opens():
        NxsReader(buf)

    def count():
        r = NxsReader(buf)
        assert r.record_count == 1000, r.record_count

    def keys():
        r = NxsReader(buf)
        for k in ("id", "username", "email", "score", "active"):
            assert k in r.keys, f"missing key {k}"

    def record_0_id():
        r = NxsReader(buf)
        assert r.record(0).get_i64("id") == js[0]["id"]

    def record_42_username():
        r = NxsReader(buf)
        assert r.record(42).get_str("username") == js[42]["username"]

    def record_500_score():
        r = NxsReader(buf)
        got = r.record(500).get_f64("score")
        assert math.isclose(got, js[500]["score"], rel_tol=1e-6), (got, js[500]["score"])

    def record_999_active():
        r = NxsReader(buf)
        assert r.record(999).get_bool("active") == js[999]["active"]

    def oob_raises():
        r = NxsReader(buf)
        try:
            r.record(10_000)
        except NxsError:
            return
        raise AssertionError("expected NxsError")

    def iter_count():
        r = NxsReader(buf)
        assert sum(1 for _ in r.records()) == 1000

    def sum_matches():
        r = NxsReader(buf)
        nxs_sum = sum(rec.get_f64("score") for rec in r.records())
        json_sum = sum(rec["score"] for rec in js)
        assert math.isclose(nxs_sum, json_sum, rel_tol=1e-6), (nxs_sum, json_sum)

    case("opens without error", opens)
    case("reads correct record count", count)
    case("reads schema keys", keys)
    case("record(0) matches JSON[0].id", record_0_id)
    case("record(42) matches JSON[42].username", record_42_username)
    case("record(500) matches JSON[500].score", record_500_score)
    case("record(999) matches JSON[999].active", record_999_active)
    case("out-of-bounds raises NxsError", oob_raises)
    case("iteration visits every record", iter_count)
    case("iteration sum matches JSON sum", sum_matches)

    # ── Security tests ───────────────────────────────────────────────────────
    def bad_magic():
        bad = bytearray(buf); bad[0] = 0x00
        try: NxsReader(bytes(bad))
        except NxsError as e:
            assert "ERR_BAD_MAGIC" in e.args[0]; return
        raise AssertionError("expected ERR_BAD_MAGIC")

    def truncated():
        try: NxsReader(buf[:16])
        except NxsError: return
        raise AssertionError("expected error on truncated file")

    def dict_mismatch():
        bad = bytearray(buf); bad[8] ^= 0xFF
        try: NxsReader(bytes(bad))
        except NxsError as e:
            assert "ERR_DICT_MISMATCH" in e.args[0]; return
        raise AssertionError("expected ERR_DICT_MISMATCH")

    case("bad magic raises ERR_BAD_MAGIC", bad_magic)
    case("truncated file raises NxsError", truncated)
    case("corrupt DictHash raises ERR_DICT_MISMATCH", dict_mismatch)

    # ── Writer round-trip tests ───────────────────────────────────────────────
    print("\nNXS Python Writer — Tests\n")

    def writer_round_trip_3_records():
        schema = NxsSchema(["id", "username", "score", "active"])
        w = NxsWriter(schema)
        recs = [
            (1, "alice", 9.5, True),
            (2, "bob",   7.2, False),
            (3, "carol", 8.8, True),
        ]
        for id_, name, score, active in recs:
            w.begin_object()
            w.write_i64(0, id_)
            w.write_str(1, name)
            w.write_f64(2, score)
            w.write_bool(3, active)
            w.end_object()
        data = w.finish()
        r = NxsReader(data)
        assert r.record_count == 3, r.record_count
        for i, (id_, name, score, active) in enumerate(recs):
            obj = r.record(i)
            assert obj.get_i64("id") == id_,         f"record {i} id"
            assert obj.get_str("username") == name,   f"record {i} username"
            assert math.isclose(obj.get_f64("score"), score), f"record {i} score"
            assert obj.get_bool("active") == active,  f"record {i} active"

    def writer_from_records():
        data = NxsWriter.from_records(
            ["id", "name", "value"],
            [
                {"id": 10, "name": "foo", "value": 1.5},
                {"id": 20, "name": "bar", "value": 2.5},
            ]
        )
        r = NxsReader(data)
        assert r.record_count == 2
        assert r.record(1).get_str("name") == "bar"

    def writer_null_field():
        schema = NxsSchema(["a", "b"])
        w = NxsWriter(schema)
        w.begin_object()
        w.write_i64(0, 99)
        w.write_null(1)
        w.end_object()
        r = NxsReader(w.finish())
        assert r.record(0).get_i64("a") == 99

    def writer_bool_field():
        schema = NxsSchema(["flag"])
        w = NxsWriter(schema)
        w.begin_object(); w.write_bool(0, True);  w.end_object()
        w.begin_object(); w.write_bool(0, False); w.end_object()
        r = NxsReader(w.finish())
        assert r.record(0).get_bool("flag") is True
        assert r.record(1).get_bool("flag") is False

    def writer_unicode_string():
        schema = NxsSchema(["msg"])
        w = NxsWriter(schema)
        w.begin_object()
        w.write_str(0, "héllo wörld")
        w.end_object()
        r = NxsReader(w.finish())
        assert r.record(0).get_str("msg") == "héllo wörld"

    def writer_schema_evolution():
        # Write with 3-field schema ["a","b","c"]
        schema = NxsSchema(["a", "b", "c"])
        w = NxsWriter(schema)
        w.begin_object()
        w.write_i64(0, 100)
        w.write_i64(1, 200)
        w.write_i64(2, 300)
        w.end_object()
        data = w.finish()

        # Read back — all three fields present
        r = NxsReader(data)
        obj = r.record(0)
        assert obj.get_i64("a") == 100
        assert obj.get_i64("b") == 200
        assert obj.get_i64("c") == 300

        # "Old reader" only requests slots 0 and 1; slot 2 is absent, not an error
        assert obj.get_i64("a") == 100, "slot a"
        assert obj.get_i64("b") == 200, "slot b"
        # Unknown key returns None (absent), not an error
        assert obj.get_i64("nonexistent_field") is None, "absent field is None"

    case("writer round-trip: 3 records",           writer_round_trip_3_records)
    case("writer round-trip: from_records",         writer_from_records)
    case("writer round-trip: null field",           writer_null_field)
    case("writer round-trip: bool field",           writer_bool_field)
    case("writer round-trip: unicode string",       writer_unicode_string)
    case("schema evolution: absent field is None",  writer_schema_evolution)

    # ── Query engine tests ───────────────────────────────────────────────────
    print("\nNXS Python Query Engine — Tests\n")

    def test_query_eq_bool():
        r = NxsReader(buf)
        expected = sum(1 for rec in js if rec["active"] is True)
        got = Query(r, Eq("active", True)).count()
        assert got == expected, f"active==True: got {got}, expected {expected}"

    def test_query_gt_float():
        r = NxsReader(buf)
        expected = sum(1 for rec in js if rec["score"] > 80.0)
        got = Query(r, Gt("score", 80.0)).count()
        assert got == expected, f"score>80: got {got}, expected {expected}"

    def test_query_and():
        r = NxsReader(buf)
        expected = sum(1 for rec in js
                       if rec["active"] is True and rec["score"] > 80.0)
        pred = Eq("active", True) & Gt("score", 80.0)
        got = Query(r, pred).count()
        assert got == expected, f"active & score>80: got {got}, expected {expected}"

    def test_query_first_returns_correct_record():
        r = NxsReader(buf)
        # Find the first JSON record where active==True
        expected = next(rec for rec in js if rec["active"] is True)
        result = Query(r, Eq("active", True)).first()
        assert result is not None, "first() returned None"
        assert result["id"] == expected["id"], (
            f"first().id={result['id']}, expected {expected['id']}"
        )
        assert result["active"] is True, "first().active should be True"

    def test_query_count_all():
        r = NxsReader(buf)
        # No predicate — should match every record
        assert Query(r).count() == 1000
        # reader.where() convenience method
        assert r.where().count() == 1000

    case("query: Eq(active, True) count matches JSON",        test_query_eq_bool)
    case("query: Gt(score, 80.0) count matches JSON",         test_query_gt_float)
    case("query: And(Eq(active), Gt(score)) count matches",   test_query_and)
    case("query: first() returns correct record",             test_query_first_returns_correct_record)
    case("query: no predicate counts all records",            test_query_count_all)

    print(f"\n{passed} passed, {failed} failed\n")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
