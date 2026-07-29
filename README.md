# hipool — Hippocampal Memory Pool

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![C](https://img.shields.io/badge/language-C-blue.svg)

**An embedded memory engine for AI agents, edge devices, and resource-constrained systems.**

Based on the hippocampal-cortical dual storage model. Zero external dependencies -- libc and a C compiler only.

---

## Why hipool?

|                  Problem                 |               What hipool does            |
|------------------------------------------|-------------------------------------------|
|   Redis/SQLite too heavy for embedded    |  **39KB binary, 2MB fixed memory pool**   |
|  Database latency too high for hot data  |  **~650ns median SET, ~4.5us median GET** |
| Agent memory needs controlled forgetting | **LRU day-level eviction with 7-day TTL** |
|  Crash safety in unreliable environments |      **WAL + atomic rename writes**       |
|   Need tag-based retrieval, not just KV  |      **Built-in inverted tag index**      |
|              Semantic search             |   **FMM tokenizer + 7D vector encoding**  |

---

## Quick Start

```bash
git clone https://github.com/hualang-C/Hipool/hipool.git
cd hipool

# Build
make

# Use
./build/memory set "2026-06-03-01" "hipool initialized" --tags "system,startup"
./build/memory get "2026-06-03-01"
./build/memory search --tag "startup"
./build/memory stats

# Interactive mode
./build/memory
```

---

## Commands

```
memory set     <key> <value> [--tags a,b,c] [--ts <unix_sec>]    Store
memory get     <key>                                                 Read
memory del     <key>                                                 Delete
memory search  <query>                                               Text search
memory search  --tag <tag>                                           Tag search
memory search  --date YYYY-MM-DD                                     Date search
memory search  --range <start_ts> <end_ts>                           Range search
memory sem-search <query>                                            Semantic search (*)
memory flush                                                         Write snapshot
memory load                                                          Load from disk
memory clean                                                         Purge expired files
memory stats                                                         Pool status
memory shard list                                                    List shards
memory snapshot                                                      Fork snapshot (*nix)

(*): Requires `make semantic` build
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

|                    Component                 |        Brain Analogy      |        hipool Implementation        |
|----------------------------------------------|---------------------------|-------------------------------------|
|               Hot memory pool                | Hippocampus (short-term)  |    2MB slab allocator, μs latency   |
|             Daily overflow files             |   Neocortex (long-term)   |     Day-split JSON, lazy reload     |
|              Day-level eviction              |    Sleep consolidation    | Oldest day evicted at 80% watermark |
|                  Tag index                   |     Associative recall    |     Inverted tag → entry links      |
|                   SkipList                   |     Temporal ordering     |        O(log n) range queries       |
|                      WAL                     |      Episodic buffer      |      Crash recovery on restart      |

---

## Performance

Measured on AMD EPYC 7K62 @ 2.6GHz, 32B values, 50K entries:

|         Metric         | Value                      |
|------------------------|----------------------------|
|        SET p50         |        **~650 ns**         |
|        GET p50         |        **~4.5 us**         |
|        SET p99         |        **~1.9 us**         |
|   Mixed (70%R/30%W)    |      **~255,000 ops/s**    |
| Binary size (stripped) |           ~39 KB           |
|     Runtime memory     | 2 MB (fixed, configurable) |
|     Disk per entry     |          ~1-2 KB           |

All tests pass. Zero segfaults. Zero memory leaks.

---

## Building

```bash
# Standard — single thread, zero overhead
make

# Semantic search (7D vector)
make semantic

# Debug build with symbols
make debug

# Thread-safe
make threadsafe

# Build tools (dict_distill)
make tools

# Run tests
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
├── src/              # Source code (11 modules)
│   ├── memory.h      # Public API header
│   ├── main.c        # CLI + interactive REPL
│   ├── pool.c        # Slab memory allocator
│   ├── hashtable.c   # DJB2 hash table
│   ├── tag_index.c   # Inverted tag index
│   ├── skiplist.c    # SkipList sorted index
│   ├── wal.c         # Write-ahead log
│   ├── json.c        # JSON serialization
│   ├── overflow.c    # Disk overflow manager
│   ├── dedup.c       # Same-day deduplication
│   ├── search.c      # Search functionality
│   ├── memory_api.c  # Public API + shards + fork snapshot
│   └── semantic.c    # Chinese semantic search (*)
├── tests/            # Test scripts
├── tools/            # Batch learning, crawling, lexicon generation
├── benchmark/        # Performance benchmarks
├── lexicon/          # Lexicon files
├── docs/             # Documentation
├── logs/             # Run logs
├── output/           # Runtime data (memory_data/)
├── Makefile          # Build system
└── README.md
```

---

## Crash Safety

All disk writes follow atomic patterns (temp file + rename). WAL (write-ahead log) records every mutation in binary format — on restart, unflushed operations are automatically replayed. Dual canary markers (`0xDEADBEEB`) around each entry detect memory corruption on `pool_free`.

---

## Configuration (compile-time constants in `memory.h`)

|       Constant      | Default |               Description               |
|---------------------|---------|-----------------------------------------|
|     `POOL_SIZE`     |  2 MB   |          Fixed memory pool size         |
|    `MAX_VAL_LEN`    |  16320  |        Max value length per entry       |
|    `EVICT_WATER`    |   80%   |        Eviction trigger threshold       |
|   `FILE_TTL_DAYS`   |    7    | Days before historical files are purged |

---

## License

MIT

---

## Citation (if used in research)

```bibtex
@software{hipool2026,
  title        = {hipool: An Embedded Memory Engine for AI Agents},
  author       = {Cheng, Zihang},
  year         = {2026},
  url          = {https://github.com/hualang-C/Hipool}
}
```
 