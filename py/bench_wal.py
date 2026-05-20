"""WAL append benchmark — NXS Writer vs JSON (Python).

Measures span-append throughput for the canonical 10-field SpanSchema.
Outputs ns/span for direct comparison with Rust `cargo run --bin bench`.
"""
from __future__ import annotations

import json
import sys
import time

sys.path.insert(0, ".")
from nxs_writer import NxsSchema, NxsWriter

SPAN_KEYS = [
    "trace_id_hi", "trace_id_lo", "span_id", "parent_span_id",
    "name", "service", "start_time_ns", "duration_ns", "status_code", "payload",
]

SCHEMA = NxsSchema(SPAN_KEYS)

SERVICES = [
    "gateway", "auth-svc", "session-svc", "catalogue-svc", "recommend-svc",
    "inventory-svc", "payment-svc", "notify-svc", "search-svc", "cdn-edge",
    "analytics-svc", "feature-flags", "config-svc", "vector-db",
]

OPS = [
    "http.server", "http.client", "grpc.server", "grpc.unary",
    "db.select", "db.insert", "db.update", "db.index_scan", "db.ann_search",
    "cache.get", "cache.set", "cache.miss",
    "pubsub.publish", "pubsub.consume",
    "llm.inference", "llm.embed",
    "jwt.verify", "auth.token_exchange",
    "queue.send", "queue.receive",
]

OP_DUR_BASE = [
    12_000_000, 11_000_000, 2_100_000, 1_900_000,
    4_200_000, 5_800_000, 4_600_000, 8_100_000, 14_500_000,
    310_000, 290_000, 350_000,
    820_000, 790_000,
    1_800_000_000, 220_000_000,
    590_000, 1_200_000,
    1_480_000, 1_510_000,
]

PAYLOADS = [
    '{"model":"gpt-4o-mini","prompt_tokens":418,"completion_tokens":91,"total_tokens":509,"finish_reason":"stop"}',
    '{"model":"text-embedding-3-small","prompt_tokens":256,"top_k":20,"reranked":8,"latency_to_first_token_ms":19}',
    '{"attempt":1,"provider":"stripe","error":"upstream_timeout","http_status":504}',
    '{"attempt":2,"provider":"adyen","transaction_id":"txn_9f3a21c8","http_status":200}',
    '{"query_plan":"index_scan","rows_examined":18420,"rows_returned":124,"execution_ms":7.3}',
    '{"cache_key":"sess:usr_0x3f8a","ttl_remaining_s":1740,"hit":true,"bytes":892}',
    '{"topic":"order.confirmed","partition":3,"offset":8847219,"ack_ms":0.8}',
]

START_NS = 1_715_018_000_000_000_000


def _span_dur_ns(op_idx: int, i: int) -> int:
    base = OP_DUR_BASE[op_idx]
    h = (i * 2654435761) & 0xFFFFFFFF
    jitter = h % int(base * 0.8)
    return int(base + jitter - base * 0.4)


def _span_status(i: int) -> int:
    h = (i * 2246822519) & 0xFFFFFFFF
    if h < 0x07AE147A: return 1
    if h < 0x0A3D70A4: return 2
    return 0


def _span_payload(op_idx: int, i: int):
    is_llm = op_idx in (14, 15)
    is_pay = op_idx == 1 and i % 7 == 0
    h = (i * 1664525 + 1013904223) & 0xFFFFFFFF
    if is_llm or is_pay or h < 0x26666666:
        return PAYLOADS[i % len(PAYLOADS)]
    return None


def make_span(i: int) -> dict:
    op_idx  = i % len(OPS)
    payload = _span_payload(op_idx, i)
    sp = {
        "trace_id_hi":    i * 1_000_003,
        "trace_id_lo":    -(i * 999_983 + 1),
        "span_id":        i + 1,
        "parent_span_id": 0 if i % 8 == 0 else (i - 1),
        "name":           OPS[op_idx],
        "service":        SERVICES[i % len(SERVICES)],
        "start_time_ns":  START_NS + i * 1_000_000,
        "duration_ns":    _span_dur_ns(op_idx, i),
        "status_code":    _span_status(i),
    }
    if payload is not None:
        sp["payload"] = payload
    return sp


def bench_nxs_wal(n: int, warmup: bool = False) -> float:
    """Returns ns/span for NXS WAL append (NYXO bytes only, no preamble)."""
    w = NxsWriter(SCHEMA)
    spans = [make_span(i) for i in range(n)]

    t0 = time.perf_counter()
    for sp in spans:
        w.begin_object()
        w.write_i64(0, sp["trace_id_hi"])
        w.write_i64(1, sp["trace_id_lo"])
        w.write_i64(2, sp["span_id"])
        w.write_i64(3, sp["parent_span_id"])
        w.write_str(4, sp["name"])
        w.write_str(5, sp["service"])
        w.write_i64(6, sp["start_time_ns"])
        w.write_i64(7, sp["duration_ns"])
        w.write_i64(8, sp["status_code"])
        if "payload" in sp:
            w.write_str(9, sp["payload"])
        w.end_object()
    t1 = time.perf_counter()

    if not warmup:
        return (t1 - t0) / n * 1e9
    return 0.0


def bench_json_ndjson(n: int, warmup: bool = False) -> float:
    """Returns ns/span for JSON NDJSON append (json.dumps per span)."""
    spans = [make_span(i) for i in range(n)]

    t0 = time.perf_counter()
    buf = []
    for sp in spans:
        buf.append(json.dumps(sp))
    t1 = time.perf_counter()

    if not warmup:
        return (t1 - t0) / n * 1e9
    return 0.0


def fmt_ns(ns: float) -> str:
    if ns < 1000:       return f"{ns:.0f} ns"
    if ns < 1_000_000:  return f"{ns/1000:.1f} µs"
    return f"{ns/1_000_000:.2f} ms"


def run(n: int) -> None:
    # Warmup
    bench_nxs_wal(min(n, 1000), warmup=True)
    bench_json_ndjson(min(n, 1000), warmup=True)

    # Repeated runs — take best of 3
    nxs_times  = [bench_nxs_wal(n) for _ in range(3)]
    json_times = [bench_json_ndjson(n) for _ in range(3)]

    nxs_ns  = min(nxs_times)
    json_ns = min(json_times)

    nxs_kps  = 1e9 / nxs_ns  / 1000
    json_kps = 1e9 / json_ns / 1000

    print(f"\n  n = {n:,}")
    print(f"  NXS WAL  {fmt_ns(nxs_ns):>10}  ({nxs_kps:,.0f} k spans/s)")
    print(f"  JSON     {fmt_ns(json_ns):>10}  ({json_kps:,.0f} k spans/s)")
    ratio = json_ns / nxs_ns
    if ratio >= 1:
        print(f"  NXS is {ratio:.2f}x faster than JSON")
    else:
        print(f"  JSON is {1/ratio:.2f}x faster than NXS")


def bench_nxs_wal_c(n: int, warmup: bool = False) -> float:
    """Returns ns/span for NXS WAL append using the C-extension Writer."""
    import _nxs
    schema = _nxs.Schema(SPAN_KEYS)
    spans = [make_span(i) for i in range(n)]
    w = _nxs.Writer(schema)
    t0 = time.perf_counter()
    for sp in spans:
        w.reset()
        w.begin_object()
        w.write_i64(0, sp["trace_id_hi"])
        w.write_i64(1, sp["trace_id_lo"])
        w.write_i64(2, sp["span_id"])
        w.write_i64(3, sp["parent_span_id"])
        w.write_str(4, sp["name"])
        w.write_str(5, sp["service"])
        w.write_i64(6, sp["start_time_ns"])
        w.write_i64(7, sp["duration_ns"])
        w.write_i64(8, sp["status_code"])
        if "payload" in sp:
            w.write_str(9, sp["payload"])
        w.end_object()
        w.data_sector()
    t1 = time.perf_counter()
    if not warmup:
        return (t1 - t0) / n * 1e9
    return 0.0


if __name__ == "__main__":
    try:
        import _nxs  # noqa: F401
        have_c = True
    except ImportError:
        have_c = False

    counts = [int(x) for x in sys.argv[1:]] if len(sys.argv) > 1 else [1_000, 10_000, 100_000]

    print("WAL append benchmark — Python (NxsWriter vs json.dumps)")
    for n in counts:
        run(n)

    if have_c:
        print("\nWAL append benchmark — Python C-extension (_nxs.Writer vs json.dumps)")
        for n in counts:
            bench_nxs_wal_c(min(n, 1000), warmup=True)
            c_ns   = min(bench_nxs_wal_c(n) for _ in range(3))
            json_ns = min(bench_json_ndjson(n) for _ in range(3))
            c_kps   = 1e9 / c_ns   / 1000
            json_kps = 1e9 / json_ns / 1000
            print(f"\n  n = {n:,}")
            print(f"  NXS WAL (C)  {fmt_ns(c_ns):>10}  ({c_kps:,.0f} k spans/s)")
            print(f"  JSON         {fmt_ns(json_ns):>10}  ({json_kps:,.0f} k spans/s)")
            ratio = json_ns / c_ns
            if ratio >= 1:
                print(f"  NXS C is {ratio:.2f}x faster than JSON")
            else:
                print(f"  JSON is {1/ratio:.2f}x faster than NXS C")
    print()
