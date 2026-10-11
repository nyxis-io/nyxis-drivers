package wal

import (
	"bufio"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"flag"
	"fmt"
	"math/rand/v2"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	nxs "github.com/nyxis-io/nyxis-drivers/go"
)

var crashIters = flag.Int("wal.crash.iters", 200, "kill/recover iterations for TestCrash_NoAckedRecordLost")

const crashCyclesPerDir = 10

func recordHash(id int64, body string) string {
	sum := sha256.Sum256([]byte(strconv.FormatInt(id, 10) + "|" + body))
	return hex.EncodeToString(sum[:])
}

// TestCrash_NoAckedRecordLost kills a child appender with SIGKILL at a random
// moment, then checks that every record the child saw acknowledged is on disk
// with identical content. Each directory goes through several kill/recover
// cycles so recovery of a torn tail or an interrupted rotation is exercised
// by the next child. SIGKILL covers process crashes; power-loss durability
// rests on TestSyncGroup_FsyncBeforeAck, the rotation op-order assertion, and
// directory syncs.
func TestCrash_NoAckedRecordLost(t *testing.T) {
	iters := *crashIters
	if testing.Short() {
		iters = min(iters, 20)
	}
	rng := rand.New(rand.NewPCG(uint64(time.Now().UnixNano()), 1))
	var dir string
	acked := map[Offset]string{}
	totalAcked := 0
	for i := range iters {
		if i%crashCyclesPerDir == 0 {
			dir = t.TempDir()
			acked = map[Offset]string{}
		}
		lines := runCrashChild(t, dir, uint64(i), time.Duration(1+rng.IntN(50))*time.Millisecond)
		for off, h := range lines {
			acked[off] = h
		}
		totalAcked += len(lines)

		s, err := OpenScanner(dir, Offset{})
		if err != nil {
			t.Fatalf("iteration %d: OpenScanner: %v", i, err)
		}
		found := map[Offset]string{}
		for s.Next() {
			r := s.Record()
			id, _ := r.GetI64("id")
			body, _ := r.GetStr("body")
			found[s.Offset()] = recordHash(id, body)
		}
		err = s.Err()
		s.Close()
		if err != nil {
			t.Fatalf("iteration %d: scan: %v", i, err)
		}
		for off, h := range acked {
			got, ok := found[off]
			if !ok {
				t.Fatalf("iteration %d: acknowledged record at %+v lost", i, off)
			}
			if got != h {
				t.Fatalf("iteration %d: record at %+v changed: hash %s, want %s", i, off, got, h)
			}
		}
	}
	t.Logf("%d kill/recover iterations, %d acknowledged records verified", iters, totalAcked)
	if totalAcked == 0 {
		t.Fatal("no record was acknowledged before any kill; the test did not exercise appends")
	}
}

// runCrashChild starts the helper, waits until it has opened the WAL, kills
// it after delay, and returns the acknowledged offsets it reported.
func runCrashChild(t *testing.T, dir string, seed uint64, delay time.Duration) map[Offset]string {
	t.Helper()
	cmd := exec.Command(os.Args[0], "-test.run=^TestCrashHelper$")
	cmd.Env = append(os.Environ(), "WAL_CRASH_DIR="+dir, "WAL_CRASH_SEED="+strconv.FormatUint(seed, 10))
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	var stderr strings.Builder
	cmd.Stderr = &stderr
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	r := bufio.NewReader(stdout)
	first, err := r.ReadString('\n')
	if err != nil || strings.TrimSpace(first) != "READY" {
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		t.Fatalf("crash child did not start: %q %v\nstderr: %s", first, err, stderr.String())
	}
	time.Sleep(delay)
	_ = cmd.Process.Signal(syscall.SIGKILL)

	lines := map[Offset]string{}
	for {
		line, err := r.ReadString('\n')
		if err != nil {
			break // a partial last line was never fully reported
		}
		var off Offset
		var h string
		if _, err := fmt.Sscanf(line, "%d,%d,%s", &off.Segment, &off.Pos, &h); err != nil {
			_ = cmd.Wait()
			t.Fatalf("crash child output %q: %v", line, err)
		}
		lines[off] = h
	}
	_ = cmd.Wait()
	return lines
}

func TestCrashHelper(t *testing.T) {
	dir := os.Getenv("WAL_CRASH_DIR")
	if dir == "" {
		t.Skip("helper for TestCrash_NoAckedRecordLost")
	}
	seed, _ := strconv.ParseUint(os.Getenv("WAL_CRASH_SEED"), 10, 64)
	w, _, err := Open(dir, testSchema, Options{Sync: SyncGroup, SegmentBytes: 64 << 10})
	if err != nil {
		fmt.Printf("OPEN_ERR %v\n", err)
		os.Exit(2)
	}
	fmt.Println("READY")

	var out sync.Mutex
	var wg sync.WaitGroup
	for g := range 4 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			rng := rand.New(rand.NewPCG(seed, uint64(g)))
			for n := int64(0); ; n++ {
				id := int64(seed)<<32 | int64(g)<<24 | n
				body := strings.Repeat(string(rune('a'+rng.IntN(26))), rng.IntN(2000))
				off, err := w.Append(context.Background(), func(e *nxs.ObjectEncoder) {
					e.WriteI64(0, id)
					e.WriteStr(1, body)
				})
				if err != nil {
					fmt.Fprintf(os.Stderr, "append: %v\n", err)
					os.Exit(3)
				}
				line := fmt.Sprintf("%d,%d,%s\n", off.Segment, off.Pos, recordHash(id, body))
				out.Lock()
				_, _ = os.Stdout.WriteString(line)
				out.Unlock()
			}
		}()
	}
	wg.Wait()
}

func TestGroupCommit_ConcurrentAppenders(t *testing.T) {
	const writers, perWriter = 32, 1000
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, Options{})

	offsets := make(chan Offset, writers*perWriter)
	var wg sync.WaitGroup
	for g := range writers {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range perWriter {
				off, err := appendRec(w, int64(g*perWriter+i), "concurrent")
				if err != nil {
					t.Error(err)
					return
				}
				offsets <- off
			}
		}()
	}
	wg.Wait()
	close(offsets)
	closeWAL(t, w)

	seen := map[Offset]bool{}
	for off := range offsets {
		if seen[off] {
			t.Fatalf("offset %+v returned twice", off)
		}
		seen[off] = true
	}
	recs := mustScan(t, dir, Offset{})
	if len(recs) != writers*perWriter || len(seen) != writers*perWriter {
		t.Fatalf("scanned %d records, %d unique offsets; want %d", len(recs), len(seen), writers*perWriter)
	}
	ids := map[int64]bool{}
	for _, r := range recs {
		if !seen[r.off] {
			t.Fatalf("scanned offset %+v was never acknowledged", r.off)
		}
		ids[r.id] = true
	}
	if len(ids) != writers*perWriter {
		t.Fatalf("%d distinct ids; want %d", len(ids), writers*perWriter)
	}
}
