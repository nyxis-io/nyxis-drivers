# NXS — C

Zero-copy `.nxb` reader and direct-to-buffer writer in C99. No dependencies beyond `libc`.

## Build & Test

```bash
make test        # compile test binary
./test ../js/fixtures

make bench       # compile benchmark binary
./bench ../js/fixtures
```

## Read a file

```c
#include "nxs.h"

uint8_t *data = ...;   // mmap or malloc+read
size_t   size = ...;

nxs_reader_t r;
nxs_open(&r, data, size);

printf("%d records, %d keys\n", r.record_count, r.key_count);

nxs_object_t obj;
nxs_record(&r, 42, &obj);

int64_t  id;     nxs_get_i64 (&obj, "id",       &id);
double   score;  nxs_get_f64 (&obj, "score",     &score);
int      active; nxs_get_bool(&obj, "active",    &active);
char     uname[64]; nxs_get_str(&obj, "username", uname, sizeof(uname));

// Slot optimisation — resolve key once, reuse per record
int slot = nxs_slot(&r, "score");
nxs_get_f64_slot(&obj, slot, &score);

// Bulk reducers
double  sum = nxs_sum_f64(&r, "score");
int64_t ids = nxs_sum_i64(&r, "id");
double  mn, mx;
nxs_min_f64(&r, "score", &mn);
nxs_max_f64(&r, "score", &mx);

nxs_close(&r);
```

## Write a file

```c
#include "nxs_writer.h"

const char *keys[] = {"id", "username", "score", "active"};
nxs_writer_t w;
nxs_writer_init(&w, keys, 4, 64 * 1024);

nxs_writer_begin_object(&w);
nxs_write_i64 (&w, 0, 42);
nxs_write_str (&w, 1, "alice", 5);
nxs_write_f64 (&w, 2, 9.5);
nxs_write_bool(&w, 3, 1);
nxs_writer_end_object(&w);

nxs_writer_finish(&w);
// w.out      — pointer to assembled .nxb bytes
// w.out_size — byte count

fwrite(w.out, 1, w.out_size, fopen("out.nxb", "wb"));
nxs_writer_free(&w);
```

## Files

| File | Purpose |
| :--- | :--- |
| `nxs.h` / `nxs.c` | Reader API |
| `nxs_writer.h` / `nxs_writer.c` | Writer API |

## Query engine

```c
#include "nxs.h"

/* Build predicates — slot resolved once at construction */
NxsPred preds[2];
preds[0] = nxs_pred_eq_bool(&reader, "active", 1);
preds[1] = nxs_pred_gt_f64(&reader, "score", 80.0);

/* Stack-allocated iterator — no heap */
NxsQuery q;
nxs_query_init(&q, &reader, preds, 2);  /* AND-combined */

nxs_object_t obj;
while (nxs_query_next(&q, &obj)) {
    double score;
    nxs_get_f64(&obj, "score", &score);
    printf("score: %f\n", score);
}

/* Or just count */
nxs_query_init(&q, &reader, preds, 2);
uint32_t count = nxs_query_count(&q);
```

Predicates are AND-combined. Pass `preds=NULL, npreds=0` to iterate all records.
All state is stack-allocated — `NxsQuery` and `NxsPred` require no heap allocation.

---

For the format specification see [`SPEC.md`](../SPEC.md). For cross-language examples see [`GETTING_STARTED.md`](../GETTING_STARTED.md).
