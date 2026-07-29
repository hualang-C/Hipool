# hipool — Hippocampal Memory Pool

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![C](https://img.shields.io/badge/language-C-blue.svg)

**An embedded memory engine for AI agents, edge devices, and resource-constrained systems.**

hipool is a zero-dependency (libc + a C compiler only) memory engine modeled on the
**hippocampus–cortex dual storage model**: a small hot in-memory pool acts as the
hippocampus (short-term, microsecond access), while daily-split files on disk act as
the neocortex (long-term, lazy reload, 7-day TTL). When the hot pool crosses an 80%
watermark, the oldest day is evicted to disk — like sleep consolidation.

---

## Why hipool?

| Problem | What hipool does |
|---|---|
| Redis / SQLite too heavy for embedded | **~39 KB binary, 2 MB fixed memory pool** |
| Database latency too high for hot data | **~650 ns median SET, ~4.5 us median GET** |
| Agent memory needs controlled forgetting | **LRU day-level eviction with 7-day TTL** |
| Crash safety in unreliable environments | **WAL + atomic rename writes** |
| Need tag-based retrieval, not just KV | **Built-in inverted tag index** |
| Semantic search | **FMM tokenizer + 7D vector encoding + knowledge-graph reranking** |

---

## Quick Start

```bash
git clone https://github.com/hualang-C/Hipool.git
cd Hipool

# Build the standard binary (single-threaded, zero overhead)
make

# Store, read, search, inspect
./build/memory set "2026-06-03-01" "hipool initialized" --tags "system,startup"
./build/memory get "2026-06-03-01"
./build/memory search --tag "startup"
./build/memory stats

# Interactive REPL (no arguments)
./build/memory
```

---

## Commands

```
memory set        <key> <value> [--tags a,b,c] [--ts <unix_sec>]   Store
memory get        <key>                                          Read
memory del        <key>                                          Delete
memory search     <query>                                        Text search (memory + disk)
memory search     --tag <tag>                                    Tag search
memory search     --date YYYY-MM-DD                              Date search
memory search     --range <start_ts> <end_ts>                    Time-range search
memory sem-search <query>                                        Semantic search (*)
memory flush                                                      Write snapshot to disk
memory load                                                       Reload from disk
memory clean                                                      Purge expired day files
memory stats                                                      Pool status
memory shard list                                                 List shards
memory shard create <name>                                        Create a named shard
memory snapshot                                                   Fork-based snapshot (*nix)
```

`(*)` semantic search requires the `make semantic` build (`-DHIPOOL_ENABLE_SEMANTIC`).

### Examples for the less obvious commands

```bash
# Named shard — isolated Pool / HashTable / TagIndex / SkipList, 256 KB each
./build/memory shard create sessions
./build/memory set "user-1" "clicked"  --tags "ui" --shard sessions
./build/memory get  "user-1"           --shard sessions
./build/memory shard list

# Time-range query over the SkipList sorted index, O(log n + k)
./build/memory search --range 1719792000 1719878400

# Fork a non-blocking copy-on-write snapshot (POSIX only)
./build/memory snapshot
```

---

## Architecture

```
┌──────────────────────────────────────────────────────┐
│                    CLI  (CLI layer)                  │
├──────────────────────────────────────────────────────┤
│               Public API  (memory_set/get/search)    │
├──────────────────────────────────────────────────────┤
│    ┌─────────┐  ┌──────────┐  ┌──────────────────┐   │
│    │  Hash   │  │   Tag    │  │   Memory Pool    │   │
│    │  Table  │  │  Index   │  │  (Slab Alloc)    │   │
│    │  (djb2) │  │ (invert) │  │ 4 levels:        │   │
│    │ 4096 bkt│  │ 512 bkt  │  │ 256/1K/4K/16K    │   │
│    └────┬────┘  └────┬─────┘  └────────┬─────────┘   │
│         └──────┬─────┘                 │             │
│                ▼                       ▼             │
│         ┌────────────────────────────────┐           │
│    ┌────┤    Overflow Manager            ├────┐      │
│    │    │ (evict, flush, load, clean)    │    │      │
│    │    └──────────────┬─────────────────┘    │      │
│    │                   ▼                      │      │
│    │    ┌──────────────────────────┐          │      │
│    │    │   SkipList (sorted idx)  │          │      │
│    │    └──────────────────────────┘          │      │
│    │    ┌──────────────────────────┐          │      │
│    │    │   WAL (crash recovery)   │          │      │
│    │    └──────────────────────────┘          │      │
├────┼──────────────────────────────────────────┤      │
│    ▼                                          ▼      │
│  output/memory_snapshot.json    output/memory_data/  │
└──────────────────────────────────────────────────────┘
```

### Hippocampal-Cortex Model

| Component | Brain Analogy | hipool Implementation |
|---|---|---|
| Hot memory pool | Hippocampus (short-term) | 2 MB slab allocator, us latency |
| Daily overflow files | Neocortex (long-term) | Day-split JSON, lazy reload |
| Day-level eviction | Sleep consolidation | Oldest day evicted at 80% watermark |
| Tag index | Associative recall | Inverted tag -> entry links |
| SkipList | Temporal ordering | O(log n) range queries |
| WAL | Episodic buffer | Crash recovery on restart |

---

## Module Reference

hipool is split into 14 focused source files under `src/`. Each subsection below
describes a module's role and shows a representative code snippet (comments translated
to English).

### `memory.h` — public API header, core types & compile-time config

Defines the packed on-disk/in-pool entry layout, the master `MemoryCtx` context that
wires every subsystem together, and all compile-time constants (`POOL_SIZE`,
`EVICT_WATER`, `MAX_SHARDS`, entity-class IDs, etc.).

```c
/* Packed entry layout. Key/value/tags are appended right after the header. */
typedef PACKED_STRUCT(MemEntry) {
    uint32_t magic_start;
    uint32_t key_hash;
    uint16_t key_len;
    uint16_t val_len;
    uint8_t  tag_count;
    uint8_t  flags;
    uint8_t  _pad[2];
    uint64_t created_at;
    uint64_t accessed_at;
} MemEntry;
#define ENTRY_KEY(e)  ((char*)(e) + sizeof(MemEntry))
#define ENTRY_VAL(e)  (ENTRY_KEY(e) + (e)->key_len + 1)
#define ENTRY_TAGS(e) (ENTRY_VAL(e) + (e)->val_len + 1)

/* Master context — ties the pool, hash table, tag index, skip list, WAL,
   shards and knowledge graph into a single object. */
struct MemoryCtx {
    Pool pool; HashTable table; TagIndex tag_index; SkipList sorted;
    char data_dir[512]; int file_ttl, initialized, wal_disabled;
    FILE *wal_fp;
    ShardEntry shards[MAX_SHARDS]; int shard_count;
    KnowledgeGraph *kg;
#ifdef HIPOOL_USE_MUTEX
    pthread_mutex_t lock;
#endif
};
```

### `main.c` — CLI entry point and interactive REPL

Parses subcommands (`set`/`get`/`del`/`search`/`sem-search`/`shard`/`flush`/`load`/
`snapshot`/`clean`/`stats`) and flags (`--shard`, `--tags`, `--ts`, `--dir`, `--ttl`).
With no arguments it drops into the `hipool>` interactive prompt. Handles Windows
UTF-8 argv conversion and POSIX `snapshot` fork/wait.

```c
if (strcmp(cmd, "shard") == 0) {
    if (argc < 3) { fprintf(stderr, "usage: memory shard (list|create <name>)\n"); ... }
    if (strcmp(argv[2], "list") == 0) {
        printf("Shards:\n  default\n");
        for (int i = 0; i < ctx.shard_count; i++) printf("  %s\n", ctx.shards[i].name);
    }
    if (strcmp(argv[2], "create") == 0 && argc > 3) {
        MemoryCtx *sc = memory_shard_ensure(&ctx, argv[3]);
        printf("%s\n", sc ? "ok" : "failed");
        if (sc) memory_flush(sc);
    }
}
```

### `memory_api.c` — public API: init / set / get / del / flush / load / shards / fork snapshot

The core `set` path enforces write-ahead logging (WAL is appended **before** any memory
mutation) and allocate-before-overwrite (the new entry is allocated first, so an
allocation failure never destroys the old value). `validate_name` guards shard names
against path traversal.

```c
/* Write-Ahead: append to the WAL first, then mutate memory. During replay
   wal_disabled=1, so wal_append is a no-op and cannot recurse. */
uint64_t eff_ts = ts ? ts : (uint64_t)time(NULL);
if (!ctx->wal_disabled) wal_append(ctx, WAL_OP_SET, key, val, tags, tc, eff_ts);

/* Allocate before overwriting: if alloc fails, the old entry is untouched. */
size_t tot = entry_total_size((uint16_t)kl, (uint16_t)vl, (uint8_t)tc);
void *ptr = pool_alloc(&ctx->pool, tot);
if (!ptr) return -1;
mem_remove_entry(ctx, key);          /* overwrite old entry, no WAL DEL */
MemEntry *e = (MemEntry*)ptr;
e->magic_start = CANARY_MAGIC; e->key_hash = kh; ...
hash_insert(&ctx->table, kh, e);
sl_insert(&ctx->sorted, e);
for (int i = 0; i < tc; i++) if (tags[i] && tags[i][0]) tag_add(&ctx->tag_index, tags[i], e);
```

```c
/* Path-traversal guard: shard names may only contain [A-Za-z0-9_-],
   be non-empty, and be at most 63 chars (the name is concatenated into a
   "%s/shard_%s" path for mkdir + file writes). */
int validate_name(const char *name) {
    if (!name || !name[0]) return -1;
    for (size_t i = 0; name[i]; i++) {
        if (i >= 63) return -1;
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return -1;
    }
    return 0;
}
```

### `pool.c` — 4-level slab memory allocator

A 2 MB pool split across four slab sizes (256 B / 1 K / 4 K / 16 K at 10/20/30/40%).
`pool_alloc` picks the smallest slab that fits (falling back to larger levels when
full) and brackets the allocation with front+rear `0xDEADBEEF` canaries; `pool_free`
validates both canaries before clearing the bitmap bit.

```c
void *pool_alloc(Pool *p, size_t sz) {
    int lv; uint32_t sizes[] = SLAB_SIZES;
    if      (sz <= sizes[0]) lv = 0; else if (sz <= sizes[1]) lv = 1;
    else if (sz <= sizes[2]) lv = 2; else if (sz <= sizes[3]) lv = 3;
    else { HIPOOL_LOG_ERROR("pool_alloc: %zu > max\n", sz); return NULL; }
    int idx = -1;
    for (; lv < 4; lv++) { idx = bm_find0(p->bm[lv], p->sc[lv]); if (idx >= 0) break; }
    if (idx < 0) { HIPOOL_LOG_WARN("pool_alloc: slabs full\n"); return NULL; }
    uint32_t rs = sizes[lv] + 8;
    uint8_t *sb = p->base + p->so[lv] + (uint32_t)b_sz + (uint32_t)(idx * rs);
    *(uint32_t*)sb = CANARY_MAGIC;                  /* head canary */
    *(uint32_t*)(sb + 4 + sizes[lv]) = CANARY_MAGIC;/* tail canary */
    bm_set(p->bm[lv], idx);
    p->used += rs; p->ta++; p->wl = (uint32_t)(p->used * 100 / p->total);
    return sb + 4;
}
```

### `hashtable.c` — DJB2 hash table (4096 buckets, separate chaining)

Lookup uses strict length + `memcmp` comparison (a fix for the old `strncmp`
prefix-collision bug where a longer query key could falsely match a shorter stored
key). Hits update `accessed_at` for LRU bookkeeping.

```c
MemEntry *hash_lookup(HashTable *t, uint32_t kh, const char *key) {
    HashNode *n = t->b[kh % HASHTABLE_SIZE];
    while (n) {
        if (n->kh == kh) {
            /* A real hit requires identical length AND content. The old code used
               strncmp(key, ek, n->e->key_len), which matched on a prefix when the
               query key was longer. */
            size_t kl = strlen(key);
            if (n->e->key_len == kl &&
                memcmp(ENTRY_KEY(n->e), key, kl) == 0) {
                n->e->accessed_at = (uint64_t)time(NULL);
                return n->e;
            }
        }
        n = n->next;
    }
    return NULL;
}
```

### `tag_index.c` — inverted tag index (512 buckets)

Each `TagEntry` holds a dynamically grown `MemEntry**` array. `tag_remove_all`
compacts every bucket to drop a deleted entry in O(total entries) rather than leaving
dangling pointers.

```c
int tag_add(TagIndex *ti, const char *tag, MemEntry *e) {
    TagEntry *te = tag_ensure(ti, tag);
    if (!te) return -1;
    for (uint32_t i = 0; i < te->n; i++)
        if (te->es[i] == e) return 0;            /* already linked */
    if (te->n >= te->cap) {
        uint32_t nc = te->cap ? te->cap * 2 : 8; /* exponential growth */
        MemEntry **na = (MemEntry**)realloc(te->es, nc * sizeof(MemEntry*));
        if (!na) return -1;
        te->es = na; te->cap = nc;
    }
    te->es[te->n++] = e;
    return 0;
}

void tag_remove_all(TagIndex *ti, MemEntry *e) {
    /* Compact every bucket in place, dropping the deleted entry. */
    for (int b = 0; b < TAG_HASHTABLE_SIZE; b++) {
        TagEntry *te = ti->b[b];
        while (te) {
            uint32_t nc = 0;
            for (uint32_t i = 0; i < te->n; i++)
                if (te->es[i] != e) te->es[nc++] = te->es[i];
            te->n = nc;
            te = te->next;
        }
    }
}
```

### `skiplist.c` — sorted index for O(log n) temporal/range queries

Entries are sorted by the composite key `(created_at << 32) | key_hash`, with up to 16
levels and a 1/4 promotion probability. `sl_range` does a proper top-down multi-level
descent to the range start (a fix replacing an earlier O(n) level-0 linear scan), then
walks level 0 collecting entries whose timestamp is below `t_end`.

```c
int sl_range(SkipList *sl, uint64_t t_start, uint64_t t_end,
             MemEntry ***out, uint32_t *cnt) {
    *out = NULL; *cnt = 0;
    if (sl->count == 0) return 0;

    /* Top-down O(log n) descent to the first node with sort_key >= sk_start. */
    uint64_t sk_start = make_sort_key(t_start, 0);
    SLNode *cur = sl->head;
    for (int i = sl->top_level; i >= 0; i--)
        while (cur->next[i] && cur->next[i]->sort_key < sk_start)
            cur = cur->next[i];
    cur = cur->next[0];

    uint32_t cap = 64, found = 0;
    MemEntry **res = (MemEntry**)malloc(cap * sizeof(MemEntry*));
    while (cur && cur->entry && (cur->sort_key >> 32) < t_end) {
        if (found >= cap) { cap *= 2; res = realloc(res, cap * sizeof(MemEntry*)); }
        res[found++] = cur->entry;
        cur = cur->next[0];
    }
    *out = res; *cnt = found; return 0;
}
```

### `wal.c` — binary write-ahead log for crash recovery

`wal_append` serializes SET/DEL records as
`magic|op|key_len|val_len|tag_count|ts|key|val|tags`. `wal_replay` reads the whole log
into a buffer and re-applies SET (via `memory_set_with_ts_unlocked`) and DEL (via
`memory_del_unlocked`), validating every length/magic field and skipping corruption.

```c
int wal_replay(MemoryCtx *ctx, const char *wal_file) {
    /* ... read whole file into buf (nread bytes) ... */
    int restored = 0; size_t pos = 0;
    while (pos + 18 <= nread) {
        uint32_t magic = *(uint32_t*)(buf + pos); pos += 4;
        if (magic != WAL_MAGIC) continue;
        uint8_t op = *(uint8_t*)(buf + pos); pos += 1;
        uint16_t nkl = *(uint16_t*)(buf + pos); pos += 2;
        /* ... read val_len, tag_count, ts ... */
        if (nkl > MAX_KEY_LEN || pos + nkl > nread) break;
        memcpy(key_buf, buf + pos, nkl); pos += nkl; key_buf[nkl] = '\0';
        if (op == WAL_OP_SET) { /* ... read val + tags ... */
            memory_set_with_ts_unlocked(ctx, key_buf, val_buf, tag_ptrs, tci, ts);
            restored++;
        } else if (op == WAL_OP_DEL) {
            if (memory_del_unlocked(ctx, key_buf) == 0) restored++;
        }
    }
    return restored;
}
```

### `overflow.c` — disk overflow manager (the "cortex" tier)

`ovf_flush` writes a full snapshot atomically (temp file + `rename`); `ovf_flush_day`
writes date-sharded `memory-YYYY-MM-DD.json` files via skip-list range queries;
`ovf_evict_oldest` flushes the oldest day whenever the pool hits the 80% watermark;
`scan_day_files` lazily reloads a key from disk on miss; `ovf_clean` deletes day files
older than the TTL.

```c
/* Eviction loop, driven by the pool watermark (wl = used% of total). */
int ovf_evict_oldest(MemoryCtx *ctx) {
    int total = 0;
    while (ctx->pool.wl >= EVICT_WATER) {
        uint64_t oldest_day = UINT64_MAX;
        SLNode *first = ctx->sorted.head->next[0];   /* SkipList is time-sorted */
        if (first && first->entry) oldest_day = day_of_ts(first->entry->created_at);
        if (oldest_day == UINT64_MAX) break;

        int n = ovf_flush_day(ctx, oldest_day);      /* write that day to disk */
        if (n <= 0) break;
        total += n;
    }
    return total;
}
```

```c
/* Atomic snapshot write: write to a temp file, fflush, then rename. */
FILE *fp = fopen(tmp_fn, "w");
/* ... write all active entries as JSON lines ... */
if (fflush(fp) != 0) { fclose(fp); unlink(tmp_fn); return -1; }
fclose(fp);
if (rename(tmp_fn, fn) != 0) { unlink(tmp_fn); return -1; }
```

### `search.c` — tag / text / date / range search

The text-search path demonstrates hipool's lock discipline: in-memory matches are
**materialized under the lock** (copied to malloc'd strings, since the underlying
`MemEntry` could be freed by another thread once unlocked), the set of seen keys is
captured for disk-side dedup, and then **all disk I/O happens lock-free** so the whole
context is never blocked by a directory scan.

```c
LOCK(ctx);
/* Collect in-memory matches under the lock. */
MemEntry *mem_matches[MAX_ENTRIES]; int n_mem = 0;
for (int b = 0; b < HASHTABLE_SIZE; b++)
    for (HashNode *n = ctx->table.b[b]; n && n_mem < MAX_ENTRIES; n = n->next) {
        MemEntry *e = n->e;
        if ((e->flags & FLAG_ACTIVE) && (strstr(ENTRY_KEY(e), q) || strstr(ENTRY_VAL(e), q)))
            mem_matches[n_mem++] = e;
    }
SearchResult *mem_sr = entry_to_search_result(mem_matches, (uint32_t)n_mem, &mem_cnt);
/* Copy seen keys for disk dedup (can't touch the hash table after unlock). */
strncpy(data_dir_cp, ctx->data_dir, sizeof(data_dir_cp) - 1);
UNLOCK(ctx);

/* —— lock-free disk I/O: scan memory-*.json files —— */
DIR *d = opendir(data_dir_cp);
while ((de = readdir(d)) != NULL && f < cap) { /* ... match query substring ... */ }
```

### `dedup.c` — same-day near-duplicate detection

For keys sharing a `prefix-` form (dash at position >= 10) and values >= 20 chars, up
to 10 candidate entries are checked. A candidate counts as a duplicate when its length
ratio is > 0.60 **and** its character-overlap rate is > 0.80, causing `memory_set` to
return `-2` (skipped).

```c
/* Two-gate similarity: length ratio AND character overlap. */
if ((vl >= evl && (double)evl / vl > 0.60) ||
    (evl >= vl && (double)vl / evl > 0.60)) {
    const char *shorter = vl < evl ? val : ev;
    size_t sl = vl < evl ? vl : evl;
    const char *longer  = vl < evl ? ev : val;
    size_t longer_len = vl < evl ? evl : vl;     /* hoisted out of the inner loop */
    size_t match_chars = 0;
    for (size_t i = 0; i < sl; i++)
        if (memchr(longer, shorter[i], longer_len)) match_chars++;
    if ((double)match_chars / sl > 0.80) return 1; /* duplicate — caller skips */
}
```

### `json.c` — lightweight JSON serialization (no external JSON library)

Hand-rolled parser for hipool's `{"k":"..","v":"..","ts":..,"tags":[..]}` line format.
`json_find_field` only accepts a `"key":"` match when it is preceded by `{` or `,`
(i.e. a real field name, not a literal that happens to appear inside a string value),
defending against injection-style false matches.

```c
/* Locate "key":" but reject matches that fall inside a string value: a field name
   must be preceded (ignoring whitespace) by '{' or ','. */
static const char *json_find_field(const char *s, const char *key) {
    char search[256]; snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = s;
    while ((p = strstr(p, search)) != NULL) {
        const char *q = p;
        while (q > s) { q--; if (*q != ' ' && *q != '\t') break; }
        if (q == s || *q == '{' || *q == ',') return p;
        p += strlen(search);
    }
    return NULL;
}
```

### `knowledge_graph.c` — embedded knowledge graph (pure-C triple store)

Used by the semantic module for reranking. `kg_add_triple` auto-creates nodes,
deduplicates triples (keeping the maximum weight), and stores them in a fixed-capacity
`KnowledgeGraph` (2048 nodes, 8192 triples). `kg_bfs_dist` computes BFS graph distance,
capped at 3 hops.

```c
int kg_add_triple(KnowledgeGraph *kg, const char *subject, KGRelationType rel,
                  const char *object, float weight) {
    int sid = kg_find_node(kg, subject); if (sid < 0) sid = (int)kg_add_node(kg, subject, 0);
    int oid = kg_find_node(kg, object);  if (oid < 0) oid = (int)kg_add_node(kg, object, 0);
    if (sid < 0 || oid < 0 || kg->triple_count >= KG_MAX_TRIPLES) return -1;

    for (uint32_t i = 0; i < kg->triple_count; i++) {   /* dedup, keep max weight */
        KGTriple *t = &kg->triples[i];
        if (t->subject_id==(uint32_t)sid && t->relation==rel && t->object_id==(uint32_t)oid) {
            t->weight = weight > t->weight ? weight : t->weight; return 0;
        }
    }
    KGTriple *t = &kg->triples[kg->triple_count++];
    t->subject_id=(uint32_t)sid; t->relation=rel; t->object_id=(uint32_t)oid; t->weight=weight;
    return 0;
}
```

### `semantic.c` — Chinese semantic search (opt-in, `-DHIPOOL_ENABLE_SEMANTIC`)

The largest module. Pipeline: (1) FMM tokenizer over a hand-written dictionary plus a
CC-CEDICT-distilled dictionary; (2) 7D vector encoding — verb ratio, entity density,
dominant entity type, bigram fingerprint, length weight, semantic-role axis (U, via
particles), and sentiment-polarity axis (T); (3) global doc-vector index with Top-K
squared-distance search; (4) knowledge-graph reranking that blends `7D x 0.4 + KG x 0.6`
and re-sorts the results.

```c
/* Z-axis: dominant entity type, normalized to [0, 1]. */
int entity_freq[ENTITY_MAX + 1] = {0};
/* ... count entity occurrences while tokenizing ... */
int max_freq = 0, dominant_entity = 0;
for (int e = 1; e <= ENTITY_MAX; e++)
    if (entity_freq[e] > max_freq) { max_freq = entity_freq[e]; dominant_entity = e; }
if (dominant_entity > 0) {
    float weighted = (float)dominant_entity * (float)HS_ENTITY_WEIGHT;
    v.z = (weighted + (float)(bigram_hash % 100) / 100.0f) / 200.0f;
    if (v.z > 1.0f) v.z = 1.0f;
}
```

```c
/* KG rerank: blend 7D distance (0.4) with KG distance (0.6), then re-sort. */
/* 0-hop = 0.0, 1-hop = 0.15, unrelated = 0.6 */
if      (min_dist > 500.0f)     kg_score = 0.6f;
else if (min_dist <= 0.5f)      kg_score = 0.0f;
else                            kg_score = 0.15f;
results[i].distance = results[i].distance * 0.4f + kg_score * 0.6f;
/* ... insertion sort results by the new distance ... */
```

---

## Performance

Measured on AMD EPYC 7K62 @ 2.6 GHz, 32-byte values, 50 K entries:

| Metric | Value |
|---|---|
| SET p50 | **~650 ns** |
| GET p50 | **~4.5 us** |
| SET p99 | **~1.9 us** |
| Mixed (70% R / 30% W) | **~255,000 ops/s** |
| Binary size (stripped) | ~39 KB |
| Runtime memory | 2 MB (fixed, configurable) |
| Disk per entry | ~1–2 KB |

All tests pass. Zero segfaults. Zero memory leaks.

---

## Building

```bash
# Standard — single-threaded, zero overhead
make

# Semantic search (7D vector + knowledge graph reranking)
make semantic

# Debug build with symbols
make debug

# Thread-safe build (-DHIPOOL_USE_MUTEX -lpthread)
make threadsafe

# Build auxiliary tools (dict_distill)
make tools

# Run the test suite (unit tests + shell tests)
make test

# Clean build artifacts
make clean
```

### Manual compile

```bash
# Standard
gcc -O2 src/*.c -I src -o build/memory

# Semantic
gcc -O2 -DHIPOOL_ENABLE_SEMANTIC src/*.c -I src -o build/memory -lm

# Thread-safe
gcc -O2 -DHIPOOL_USE_MUTEX -lpthread src/*.c -I src -o build/memory
```

---

## Project Structure

```
hipool_v6/
├── src/              # Source code (14 modules)
│   ├── memory.h          # Public API header — types & compile-time config
│   ├── main.c            # CLI + interactive REPL
│   ├── memory_api.c      # Public API + shards + fork snapshot
│   ├── pool.c            # 4-level slab memory allocator
│   ├── hashtable.c       # DJB2 hash table
│   ├── tag_index.c       # Inverted tag index
│   ├── skiplist.c        # SkipList sorted index
│   ├── wal.c             # Write-ahead log (crash recovery)
│   ├── json.c            # JSON serialization helpers
│   ├── overflow.c        # Disk overflow manager (snapshot / day files / TTL)
│   ├── dedup.c           # Same-day deduplication
│   ├── search.c          # Tag / text / date / range search
│   ├── semantic.c        # Chinese semantic search (*)
│   └── knowledge_graph.c # Embedded knowledge graph (used by semantic search)
├── tests/            # unit_test.c, test.sh, ablation_test.sh
├── tools/            # Lexicon generation, batch/crawl/cruise learning, KG population
├── benchmark/        # Latency / throughput / hash-comparison benchmarks
├── lexicon/          # Generated lexicon (lexicon_generated.h)
├── docs/             # Design plans and prior lexicon exports
├── logs/             # Run logs
├── output/           # Runtime data (memory_data/ snapshots & day files)
├── build/            # Build output (the compiled `memory` binary)
├── Makefile          # Build system
├── CHANGELOG.md      # Version history (v4.0 -> v6.0)
├── SKILL.md          # Adaptive lexicon maintenance guide
├── LICENSE           # MIT
└── README.md
```

`(*)` `semantic.c` is excluded from the standard build; use `make semantic`.

---

## Crash Safety

- **Atomic disk writes** — every snapshot and day-file write goes to a temp file,
  `fflush`es, then `rename`s into place, so a crash mid-write never corrupts the
  existing file.
- **WAL replay** — every mutation is appended to a binary write-ahead log first; on
  restart, unflushed SET/DEL operations are automatically replayed.
- **Dual canary markers** — each pool allocation is bracketed with `0xDEADBEEF`
  front + rear canaries; `pool_free` rejects any allocation whose canaries are
  corrupted, turning silent heap corruption into a loud, localized error.

---

## Configuration (compile-time constants in `memory.h`)

| Constant | Default | Description |
|---|---|---|
| `POOL_SIZE` | 2 MB | Fixed memory pool size |
| `MAX_KEY_LEN` | 256 | Maximum key length |
| `MAX_VAL_LEN` | 16320 | Maximum value length per entry |
| `MAX_TAGS` | 16 | Maximum tags per entry |
| `HASHTABLE_SIZE` | 4096 | Hash table buckets |
| `TAG_HASHTABLE_SIZE` | 512 | Tag index buckets |
| `EVICT_WATER` | 80 (%) | Pool watermark that triggers day eviction |
| `FILE_TTL_DAYS` | 7 | Days before historical day files are purged |
| `CANARY_MAGIC` | 0xDEADBEEF | Pool allocation canary |
| `MAX_SHARDS` | 8 | Maximum named shards |
| `SHARD_POOL_SIZE` | 256 KB | Per-shard pool size |

---

## Tools, Tests & Benchmarks

### `tools/`
- `dict_distill.c` — distills CC-CEDICT into `lexicon/lexicon_generated.h`, classifying each English gloss into one of 17 entity categories.
- `kg_populate.c` — seeds the knowledge graph with sample persons/toys/appliances/network-device triples.
- `batch_learn.sh` / `batch_learn.js` — batch-import user-defined lexicon terms into hipool.
- `generate_lexicon.sh` — generate categorized lexicon entries (companies / techs / concepts).
- `crawl_learn.sh` / `cruise_learn.sh` — adaptive crawl/cruise learning loops that promote discovered words into the lexicon.

### `tests/`
- `unit_test.c` — C-level unit tests locking in regression fixes (hash prefix collision, GET ownership, WAL round-trip, pool canary, skip-list range, JSON injection, dedup similarity, path-traversal name validation).
- `test.sh` — black-box CLI suite (~16 tests): set/get, tag/text/semantic search, hash collisions, random data, bulk stress, edge cases, overwrite cycles, persistence.
- `ablation_test.sh` — feature-by-feature ablation: SkipList, WAL recovery, shard isolation, fork snapshot, and edge cases (Unicode, special chars, 16 KB value, 16 tags, mutex build).

### `benchmark/`
- `benchmark.c` — comprehensive micro-benchmark (SET/GET across value sizes, 70R/30W mix, eviction stress, per-slab breakdown).
- `bench_compare.c` — hipool-vs-LMDB/SQLite comparison harness emitting CSV latency percentiles.
- `bench_small.c` — small-data benchmark (N=1000, no eviction).
- `hash_bench.c` — djb2 vs MurmurHash3 vs xxHash collision/throughput analysis.

---

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 Zihang Cheng.

---

## Citation

If you use hipool in research, please cite:

```bibtex
@software{hipool2026,
  title  = {hipool: An Embedded Memory Engine for AI Agents},
  author = {Zihang Cheng},
  year   = {2026},
  url    = {https://github.com/hualang-C/Hipool}
}
```

---

## See Also

- [CHANGELOG.md](CHANGELOG.md) — version history, v4.0 through v6.0.
- [SKILL.md](SKILL.md) — adaptive lexicon / classification / knowledge-graph maintenance guide.
- [docs/](docs/) — design plans, including the adaptive-lexicon learning system plan.
