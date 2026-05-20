// Command bench compares NXS, encoding/json, and encoding/csv on the existing
// fixtures. Matches the 5-scenario layout used by the JS and Python benches.
package main

import (
	"bytes"
	"encoding/csv"
	"encoding/json"
	"fmt"
	"math/rand"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"time"

	"github.com/micaelmalta/nyxis/go"
)

type record struct {
	ID        int64   `json:"id"`
	Username  string  `json:"username"`
	Email     string  `json:"email"`
	Age       int64   `json:"age"`
	Balance   float64 `json:"balance"`
	Active    bool    `json:"active"`
	Score     float64 `json:"score"`
	CreatedAt string  `json:"created_at"`
}

// ── Timing harness ──────────────────────────────────────────────────────────

func timeIt(iters int, fn func()) time.Duration {
	// Warmup
	warm := iters / 10
	if warm < 3 {
		warm = 3
	}
	for i := 0; i < warm; i++ {
		fn()
	}
	start := time.Now()
	for i := 0; i < iters; i++ {
		fn()
	}
	return time.Since(start) / time.Duration(iters)
}

func fmtDur(d time.Duration) string {
	switch {
	case d < time.Microsecond:
		return fmt.Sprintf("%d ns", d.Nanoseconds())
	case d < time.Millisecond:
		return fmt.Sprintf("%.1f µs", float64(d.Nanoseconds())/1000)
	case d < time.Second:
		return fmt.Sprintf("%.2f ms", float64(d.Nanoseconds())/1_000_000)
	default:
		return fmt.Sprintf("%.2f s", d.Seconds())
	}
}

func fmtBytes(n int) string {
	switch {
	case n < 1024:
		return fmt.Sprintf("%d B", n)
	case n < 1024*1024:
		return fmt.Sprintf("%.1f KB", float64(n)/1024)
	default:
		return fmt.Sprintf("%.2f MB", float64(n)/(1024*1024))
	}
}

func row(label string, avg, baseline time.Duration) {
	var ratio string
	switch {
	case avg == baseline:
		ratio = "baseline"
	case avg < baseline:
		ratio = fmt.Sprintf("%.1fx faster", float64(baseline)/float64(avg))
	default:
		ratio = fmt.Sprintf("%.1fx slower", float64(avg)/float64(baseline))
	}
	fmt.Printf("  │  %-44s %10s   %s\n", label, fmtDur(avg), ratio)
}

func header(title string) {
	dashes := 76 - len(title)
	if dashes < 0 {
		dashes = 0
	}
	fmt.Printf("\n  ┌─ %s %s┐\n", title, repeatDash(dashes))
}

func footer() { fmt.Printf("  └%s┘\n", repeatDash(79)) }

func repeatDash(n int) string {
	b := make([]byte, 0, n*3)
	for i := 0; i < n; i++ {
		b = append(b, 0xE2, 0x94, 0x80) // U+2500 ─
	}
	return string(b)
}

// ── CSV helpers (minimal; matches JS bench parity) ──────────────────────────

// sumCsvScore scans the raw CSV bytes for just the `score` column.
// Column order: id,username,email,age,balance,active,score,created_at
// Assumption matches fixture: no quoted fields, no embedded newlines.
func sumCsvScore(data []byte) float64 {
	// Skip header line
	i := bytes.IndexByte(data, '\n') + 1
	var sum float64
	for i < len(data) {
		// Find the 6th comma on this line, then parse up to the 7th.
		commas := 0
		scoreStart := i
		for i < len(data) {
			c := data[i]
			if c == ',' {
				commas++
				if commas == 6 {
					scoreStart = i + 1
				} else if commas == 7 {
					v, _ := strconv.ParseFloat(string(data[scoreStart:i]), 64)
					sum += v
					// skip to EOL
					for i < len(data) && data[i] != '\n' {
						i++
					}
					i++
					break
				}
			}
			i++
		}
	}
	return sum
}

// parseCsvAll returns all records, using encoding/csv (the realistic Go API).
func parseCsvAll(data []byte) ([]record, error) {
	r := csv.NewReader(bytes.NewReader(data))
	rows, err := r.ReadAll()
	if err != nil {
		return nil, err
	}
	out := make([]record, 0, len(rows)-1)
	for _, row := range rows[1:] {
		id, _ := strconv.ParseInt(row[0], 10, 64)
		age, _ := strconv.ParseInt(row[3], 10, 64)
		balance, _ := strconv.ParseFloat(row[4], 64)
		active := row[5] == "true"
		score, _ := strconv.ParseFloat(row[6], 64)
		out = append(out, record{
			ID: id, Username: row[1], Email: row[2], Age: age,
			Balance: balance, Active: active, Score: score, CreatedAt: row[7],
		})
	}
	return out, nil
}

// ── Benchmark ──────────────────────────────────────────────────────────────

func runScale(fixtureDir string, n int) {
	nxbPath := filepath.Join(fixtureDir, fmt.Sprintf("records_%d.nxb", n))
	jsonPath := filepath.Join(fixtureDir, fmt.Sprintf("records_%d.json", n))
	csvPath := filepath.Join(fixtureDir, fmt.Sprintf("records_%d.csv", n))

	nxbBuf, err := os.ReadFile(nxbPath)
	if err != nil {
		fmt.Printf("\n  ⚠  skipping n=%d: %v\n", n, err)
		return
	}
	jsonBuf, _ := os.ReadFile(jsonPath)
	csvBuf, _ := os.ReadFile(csvPath)

	fmt.Printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  n = %s  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n", withCommas(n))
	fmt.Printf("  .nxb  %10s   (%d%% of JSON)\n", fmtBytes(len(nxbBuf)), 100*len(nxbBuf)/len(jsonBuf))
	fmt.Printf("  .json %10s   (100%% of JSON)\n", fmtBytes(len(jsonBuf)))
	fmt.Printf("  .csv  %10s   (%d%% of JSON)\n", fmtBytes(len(csvBuf)), 100*len(csvBuf)/len(jsonBuf))

	var parseIters, randomIters, iterateIters, coldIters int
	switch {
	case n >= 1_000_000:
		parseIters, randomIters, iterateIters, coldIters = 10, 50_000, 3, 5
	case n >= 100_000:
		parseIters, randomIters, iterateIters, coldIters = 50, 100_000, 10, 30
	case n >= 10_000:
		parseIters, randomIters, iterateIters, coldIters = 300, 200_000, 100, 200
	default:
		parseIters, randomIters, iterateIters, coldIters = 3000, 200_000, 1000, 1000
	}

	// Pre-parse (warm path needs these)
	var parsedJSON []record
	_ = json.Unmarshal(jsonBuf, &parsedJSON)
	parsedCSV, _ := parseCsvAll(csvBuf)
	reader, err := nxs.NewReader(nxbBuf)
	if err != nil {
		fmt.Printf("  ⚠  reader error: %v\n", err)
		return
	}

	// ── 1. Open file ─────────────────────────────────────────────────────
	header("Open file (parse full structure)")
	tJSON := timeIt(parseIters, func() {
		var x []record
		json.Unmarshal(jsonBuf, &x)
	})
	tCSV := timeIt(parseIters, func() { parseCsvAll(csvBuf) })
	tNXS := timeIt(parseIters, func() { nxs.NewReader(nxbBuf) })
	row("json.Unmarshal(entire document)", tJSON, tJSON)
	row("csv.NewReader.ReadAll + struct build", tCSV, tJSON)
	row("nxs.NewReader(buffer)", tNXS, tJSON)
	footer()

	// ── 2. Warm random access ───────────────────────────────────────────
	header("Random-access read (1 field from 1 record)")
	rng := rand.New(rand.NewSource(0))
	idxs := make([]int, randomIters)
	for i := range idxs {
		idxs[i] = rng.Intn(n)
	}
	ii := 0
	tJSONr := timeIt(randomIters, func() {
		_ = parsedJSON[idxs[ii%randomIters]].Username
		ii++
	})
	ii = 0
	tCSVr := timeIt(randomIters, func() {
		_ = parsedCSV[idxs[ii%randomIters]].Username
		ii++
	})
	ii = 0
	tNXSr := timeIt(randomIters, func() {
		_, _ = reader.Record(idxs[ii%randomIters]).GetStr("username")
		ii++
	})
	slot := reader.Slot("username")
	ii = 0
	tNXSrSlot := timeIt(randomIters, func() {
		_, _ = reader.Record(idxs[ii%randomIters]).GetStrBySlot(slot)
		ii++
	})
	row("parsedJSON[k].Username (pre-parsed)", tJSONr, tJSONr)
	row("parsedCSV[k].Username (pre-parsed)", tCSVr, tJSONr)
	row("reader.Record(k).GetStr('username')", tNXSr, tJSONr)
	row("reader.Record(k).GetStrBySlot(slot)", tNXSrSlot, tJSONr)
	footer()

	// ── 3. Cold start ───────────────────────────────────────────────────
	header("First access — open + read 1 field (cold start)")
	k := n / 2
	tJSONc := timeIt(coldIters, func() {
		var x []record
		json.Unmarshal(jsonBuf, &x)
		_ = x[k].Username
	})
	tCSVc := timeIt(coldIters, func() {
		x, _ := parseCsvAll(csvBuf)
		_ = x[k].Username
	})
	tNXSc := timeIt(coldIters, func() {
		r, _ := nxs.NewReader(nxbBuf)
		_, _ = r.Record(k).GetStr("username")
	})
	row("json.Unmarshal + arr[k].Username", tJSONc, tJSONc)
	row("parseCsvAll + arr[k].Username", tCSVc, tJSONc)
	row("nxs.NewReader + record(k).GetStr", tNXSc, tJSONc)
	footer()

	// ── 4. Full scan (per-record API) ───────────────────────────────────
	header("Full scan — sum of 'score' (per-record API)")
	tJSONi := timeIt(iterateIters, func() {
		var s float64
		for i := range parsedJSON {
			s += parsedJSON[i].Score
		}
		_ = s
	})
	tCSVi := timeIt(iterateIters, func() {
		var s float64
		for i := range parsedCSV {
			s += parsedCSV[i].Score
		}
		_ = s
	})
	scoreSlot := reader.Slot("score")
	tNXSi := timeIt(iterateIters, func() {
		var s float64
		rc := reader.RecordCount()
		for i := 0; i < rc; i++ {
			v, _ := reader.Record(i).GetF64BySlot(scoreSlot)
			s += v
		}
		_ = s
	})
	row("for i: s += parsedJSON[i].Score", tJSONi, tJSONi)
	row("for i: s += parsedCSV[i].Score", tCSVi, tJSONi)
	row("NXS per-record (by slot)", tNXSi, tJSONi)
	footer()

	// ── 5. Columnar scan / reducer ──────────────────────────────────────
	header("Columnar scan — same sum, using bulk APIs")
	tJSONbulk := timeIt(iterateIters, func() {
		var s float64
		for i := range parsedJSON {
			s += parsedJSON[i].Score
		}
		_ = s
	})
	tCSVbulk := timeIt(iterateIters, func() { _ = sumCsvScore(csvBuf) })
	tNXSbulk := timeIt(iterateIters, func() { _ = reader.SumF64("score") })
	tNXSfast := timeIt(iterateIters, func() { _ = reader.SumF64Fast("score") })
	tNXSpar := timeIt(iterateIters, func() { _ = reader.SumF64FastPar("score", 0) })
	scoreIdx, _ := reader.BuildFieldIndex("score")
	tNXSindexed := timeIt(iterateIters, func() { _ = reader.SumF64Indexed(scoreIdx) })
	row("JSON baseline (re-measured)", tJSONbulk, tJSONbulk)
	row("sumCsvScore(raw bytes)", tCSVbulk, tJSONbulk)
	row("reader.SumF64('score')  [safe]", tNXSbulk, tJSONbulk)
	row("reader.SumF64Fast('score')  [uniform]", tNXSfast, tJSONbulk)
	row(fmt.Sprintf("reader.SumF64FastPar (%d workers)", runtime.GOMAXPROCS(0)), tNXSpar, tJSONbulk)
	row("reader.SumF64Indexed  [hot, index pre-built]", tNXSindexed, tJSONbulk)
	footer()

	// ── 6. Cold pipeline: file → aggregate ──────────────────────────────
	header("Cold pipeline — open file + reduce (no pre-parsed state)")
	tJSONpipe := timeIt(coldIters, func() {
		buf, _ := os.ReadFile(jsonPath)
		var x []record
		json.Unmarshal(buf, &x)
		var s float64
		for i := range x {
			s += x[i].Score
		}
		_ = s
	})
	tCSVpipe := timeIt(coldIters, func() {
		buf, _ := os.ReadFile(csvPath)
		_ = sumCsvScore(buf)
	})
	tNXSpipe := timeIt(coldIters, func() {
		buf, _ := os.ReadFile(nxbPath)
		r, _ := nxs.NewReader(buf)
		_ = r.SumF64("score")
	})
	tNXSpipeFast := timeIt(coldIters, func() {
		buf, _ := os.ReadFile(nxbPath)
		r, _ := nxs.NewReader(buf)
		_ = r.SumF64Fast("score")
	})
	tNXSpipePar := timeIt(coldIters, func() {
		buf, _ := os.ReadFile(nxbPath)
		r, _ := nxs.NewReader(buf)
		_ = r.SumF64FastPar("score", 0)
	})
	row("JSON: ReadFile + Unmarshal + loop", tJSONpipe, tJSONpipe)
	row("CSV:  ReadFile + sumCsvScore", tCSVpipe, tJSONpipe)
	row("NXS:  ReadFile + NewReader + SumF64          [safe]", tNXSpipe, tJSONpipe)
	row("NXS:  ReadFile + NewReader + SumF64Fast      [fast]", tNXSpipeFast, tJSONpipe)
	row("NXS:  ReadFile + NewReader + SumF64FastPar   [par]", tNXSpipePar, tJSONpipe)
	footer()
}

func withCommas(n int) string {
	s := strconv.Itoa(n)
	out := make([]byte, 0, len(s)+len(s)/3)
	for i, c := range []byte(s) {
		if i > 0 && (len(s)-i)%3 == 0 {
			out = append(out, ',')
		}
		out = append(out, c)
	}
	return string(out)
}

func main() {
	dir := "../js/fixtures"
	if len(os.Args) > 1 {
		dir = os.Args[1]
	}

	fmt.Println("\n╔════════════════════════════════════════════════════════════════════════════════╗")
	fmt.Println("║          NXS vs encoding/json vs encoding/csv  —  Go Benchmark                ║")
	fmt.Println("╚════════════════════════════════════════════════════════════════════════════════╝")
	fmt.Printf("\n  Go:       %s\n", goVersion())
	fmt.Printf("  Fixtures: %s\n", dir)

	for _, n := range []int{1_000, 10_000, 100_000, 1_000_000} {
		runScale(dir, n)
	}

	fmt.Println("\n" + repeatDash(80))
	fmt.Println("  Notes:")
	fmt.Println("    • encoding/json.Unmarshal is Go's reflection-based parser (stdlib).")
	fmt.Println("    • encoding/csv.ReadAll then strconv.ParseFloat (stdlib).")
	fmt.Println("    • nxs.NewReader reads only preamble + schema + tail-index header.")
	fmt.Println("    • SumF64 runs a tight in-Go loop — no allocations per record.")
}

func goVersion() string {
	return runtime.Version()
}
