package nxs

import (
	"encoding/json"
	"fmt"
	"testing"
	"time"
)

// SpanSchema keys matching the canonical 10-field layout.
var walSchema = NewSchema([]string{
	"trace_id_hi", "trace_id_lo", "span_id", "parent_span_id",
	"name", "service", "start_time_ns", "duration_ns", "status_code", "payload",
})

var walServices = []string{
	"gateway", "auth-svc", "session-svc", "catalogue-svc", "recommend-svc",
	"inventory-svc", "payment-svc", "notify-svc", "search-svc", "cdn-edge",
	"analytics-svc", "feature-flags", "config-svc", "vector-db",
}

var walOps = []string{
	"http.server", "http.client", "grpc.server", "grpc.unary",
	"db.select", "db.insert", "db.update", "db.index_scan", "db.ann_search",
	"cache.get", "cache.set", "cache.miss",
	"pubsub.publish", "pubsub.consume",
	"llm.inference", "llm.embed",
	"jwt.verify", "auth.token_exchange",
	"queue.send", "queue.receive",
}

var walOpDurBase = []int64{
	12_000_000, 11_000_000, 2_100_000, 1_900_000,
	4_200_000, 5_800_000, 4_600_000, 8_100_000, 14_500_000,
	310_000, 290_000, 350_000,
	820_000, 790_000,
	1_800_000_000, 220_000_000,
	590_000, 1_200_000,
	1_480_000, 1_510_000,
}

var walPayloads = []string{
	`{"model":"gpt-4o-mini","prompt_tokens":418,"completion_tokens":91,"total_tokens":509,"finish_reason":"stop"}`,
	`{"model":"text-embedding-3-small","prompt_tokens":256,"top_k":20,"reranked":8,"latency_to_first_token_ms":19}`,
	`{"attempt":1,"provider":"stripe","error":"upstream_timeout","http_status":504}`,
	`{"attempt":2,"provider":"adyen","transaction_id":"txn_9f3a21c8","http_status":200}`,
	`{"query_plan":"index_scan","rows_examined":18420,"rows_returned":124,"execution_ms":7.3}`,
	`{"cache_key":"sess:usr_0x3f8a","ttl_remaining_s":1740,"hit":true,"bytes":892}`,
	`{"topic":"order.confirmed","partition":3,"offset":8847219,"ack_ms":0.8}`,
}

const walStartNs int64 = 1_715_018_000_000_000_000

func spanDurNs(opIdx, i int) int64 {
	base := walOpDurBase[opIdx]
	h := uint32(i) * 2654435761
	jitter := int64(h % uint32(float64(base)*0.8))
	return base + jitter - int64(float64(base)*0.4)
}

func spanStatus(i int) int64 {
	h := uint32(i) * 2246822519
	if h < 0x07AE147A {
		return 1
	}
	if h < 0x0A3D70A4 {
		return 2
	}
	return 0
}

func spanPayload(opIdx, i int) string {
	isLLM := opIdx == 14 || opIdx == 15
	isPay := opIdx == 1 && i%7 == 0
	h := uint32(i*1664525 + 1013904223)
	if isLLM || isPay || h < 0x26666666 {
		return walPayloads[i%len(walPayloads)]
	}
	return ""
}

type spanRecord struct {
	TraceIDHi    int64  `json:"trace_id_hi"`
	TraceIDLo    int64  `json:"trace_id_lo"`
	SpanID       int64  `json:"span_id"`
	ParentSpanID int64  `json:"parent_span_id"`
	Name         string `json:"name"`
	Service      string `json:"service"`
	StartTimeNs  int64  `json:"start_time_ns"`
	DurationNs   int64  `json:"duration_ns"`
	StatusCode   int64  `json:"status_code"`
	Payload      string `json:"payload,omitempty"`
}

func makeSpan(i int) spanRecord {
	opIdx := i % len(walOps)
	return spanRecord{
		TraceIDHi: int64(i * 1_000_003),
		TraceIDLo: -int64(i*999_983 + 1),
		SpanID:    int64(i + 1),
		ParentSpanID: func() int64 {
			if i%8 == 0 {
				return 0
			}
			return int64(i - 1)
		}(),
		Name:        walOps[opIdx],
		Service:     walServices[i%len(walServices)],
		StartTimeNs: walStartNs + int64(i)*1_000_000,
		DurationNs:  spanDurNs(opIdx, i),
		StatusCode:  spanStatus(i),
		Payload:     spanPayload(opIdx, i),
	}
}

func runWalNxs(n int) float64 {
	spans := make([]spanRecord, n)
	for i := range spans {
		spans[i] = makeSpan(i)
	}
	w := NewWriterWithCapacity(walSchema, n*128)
	t0 := time.Now()
	for _, sp := range spans {
		w.BeginObject()
		w.WriteI64(0, sp.TraceIDHi)
		w.WriteI64(1, sp.TraceIDLo)
		w.WriteI64(2, sp.SpanID)
		w.WriteI64(3, sp.ParentSpanID)
		w.WriteStr(4, sp.Name)
		w.WriteStr(5, sp.Service)
		w.WriteI64(6, sp.StartTimeNs)
		w.WriteI64(7, sp.DurationNs)
		w.WriteI64(8, sp.StatusCode)
		if sp.Payload != "" {
			w.WriteStr(9, sp.Payload)
		}
		w.EndObject()
	}
	elapsed := time.Since(t0)
	return float64(elapsed.Nanoseconds()) / float64(n)
}

func runWalJSON(n int) float64 {
	spans := make([]spanRecord, n)
	for i := range spans {
		spans[i] = makeSpan(i)
	}
	t0 := time.Now()
	for _, sp := range spans {
		_, _ = json.Marshal(sp)
	}
	elapsed := time.Since(t0)
	return float64(elapsed.Nanoseconds()) / float64(n)
}

func fmtNs(ns float64) string {
	if ns < 1000 {
		return fmt.Sprintf("%.0f ns", ns)
	}
	if ns < 1_000_000 {
		return fmt.Sprintf("%.1f µs", ns/1000)
	}
	return fmt.Sprintf("%.2f ms", ns/1_000_000)
}

func TestWalBench(t *testing.T) {
	counts := []int{1_000, 10_000, 100_000}
	fmt.Println("WAL append benchmark — Go (NxsWriter vs json.Marshal)")
	for _, n := range counts {
		// warmup
		runWalNxs(min(n, 1000))
		runWalJSON(min(n, 1000))

		var nxsBest, jsonBest float64
		for r := 0; r < 3; r++ {
			nxsNs := runWalNxs(n)
			jsonNs := runWalJSON(n)
			if r == 0 || nxsNs < nxsBest {
				nxsBest = nxsNs
			}
			if r == 0 || jsonNs < jsonBest {
				jsonBest = jsonNs
			}
		}
		nxsKps := 1e9 / nxsBest / 1000
		jsonKps := 1e9 / jsonBest / 1000
		fmt.Printf("\n  n = %d\n", n)
		fmt.Printf("  NXS WAL  %s  (%.0f k spans/s)\n", fmtNs(nxsBest), nxsKps)
		fmt.Printf("  JSON     %s  (%.0f k spans/s)\n", fmtNs(jsonBest), jsonKps)
		if jsonBest >= nxsBest {
			fmt.Printf("  NXS is %.2fx faster than JSON\n", jsonBest/nxsBest)
		} else {
			fmt.Printf("  JSON is %.2fx faster than NXS\n", nxsBest/jsonBest)
		}
	}
	fmt.Println()
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

func BenchmarkWalNxs10k(b *testing.B) {
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		runWalNxs(10_000)
	}
}

func BenchmarkWalJSON10k(b *testing.B) {
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		runWalJSON(10_000)
	}
}
