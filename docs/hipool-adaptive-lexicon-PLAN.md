# hipool Adaptive Lexicon Learning System -- Plan

> Version: v1.0 | Date: 2026-07-19 | Status: Planning

---

## 1. Overview

### 1.1 Background

hipool semantic search module depends on a static lexicon for FMM tokenization. In real usage, technical terms, proper names, and domain-specific vocabulary are often not in the lexicon, causing the FMM tokenizer to tag them as UNK. This degrades vector encoding quality and reduces semantic search accuracy.

### 1.2 Goal

Add **unsupervised adaptive lexicon learning** to hipool:
- Automatically discover high-frequency UNK words
- Infer classification from context
- Promote to lexicon entries
- Persist across restarts
- Maintain hipool's **lightweight, zero external dependency** design philosophy

### 1.3 Design Principles

- **Do not break existing functionality** -- conditional compilation `HIPOOL_ENABLE_SEMANTIC_LEARN`
- **Do not block main path** -- UNK tracking completes in ~2us on SET
- **Controlled capacity** -- fixed upper limit with LRU eviction
- **Observable** -- CLI commands to inspect candidates and learned words

---

## 2. Core Architecture

```
                     memory SET path

  memory_set_with_ts(key, val)
    |- pool_alloc / hash_insert / tag_index (existing)
    |- hs_index_entry(kh, val)            (semantic index)
    - scan_unk_ngrams(val)                NEW: UNK segment scan
          |
          v
    candidate_tracker_add(text, context)
    - hash + count -> candidate table
          |
          v  (reaches PROMOTE_THRESHOLD)
    learn_queue_push(text)  -> file learn_queue.dat
```

---

## 3. Data Structures

### 3.1 UNK Candidate Table (in-memory)

```c
#define MAX_CANDIDATES   4096
#define PROMOTE_THRESHOLD 3
#define MAX_WORD_LEN      48

typedef enum {
    CAND_PENDING = 0,
    CAND_PROCESSING,
    CAND_PROMOTED,
    CAND_DISCARDED
} CandidateStatus;

typedef struct {
    uint32_t   hash;
    char       text[MAX_WORD_LEN];
    uint8_t    length;
    uint16_t   count;
    uint8_t    ent_freq[18];
    uint8_t    tag_freq[6];
    uint16_t   neighbor_total;
    time_t     first_seen;
    uint8_t    status;
    uint16_t   _pad;
} Candidate;
```

### 3.2 Learned Dictionary (in-memory + file)

```c
#define MAX_LEARNED        4096

typedef struct {
    char       word[MAX_WORD_LEN];
    uint8_t    tag;
    uint8_t    entity_id;
    uint16_t   freq;
    time_t     learned_at;
} LearnedWord;
```

### 3.3 Learn Queue (file)

```
learn_queue.dat
Format: one candidate word per line, UTF-8 text, \n delimited
```

### 3.4 First-Character Hash Bucket Modification

```c
// New interfaces
void hs_add_learned(const LearnedWord *word);
void hs_rebuild_buckets(void);
```

---

## 4. Core Algorithms

### 4.1 UNK n-gram Scanning

```
Input: "NewTech released AI platform"
FMM tokens:
  [New](UNK) [Tech](UNK) [released](D,ent=10) [AI](UNK) [platform](UNK)

Consecutive UNK run detection:
  Run 1: "New"/"Tech" -> 2 consecutive UNK
    -> n-gram candidates: "NewTech"(2)
    -> context: right neighbor="released"(D,ent=10)

  Run 2: "AI"/"platform" -> 2 consecutive UNK
    -> candidates: "AIplatform"(2)
    -> context: left neighbor="released"(D,ent=10)
```

### 4.2 Context Classification Voting

```
Candidate "NewTech"
Neighbor samples:
  SET 1: left=start, right="released"(D,ent=10)
    -> subject position, right is verb -> likely noun entity
    -> ent_freq[10]++, tag_freq[M]++

Final vote: tag=M(3/3), entity=10(frequent neighbor)
         -> promote to (M, entity=0) general noun
```

### 4.3 LRU Eviction

```c
// Candidate table eviction (when MAX_CANDIDATES full):
// 1. Preferentially evict DISCARDED status
// 2. Then evict lowest count PENDING
// 3. Then evict oldest first_seen

// Learned dict eviction (when MAX_LEARNED full):
// 1. Evict lowest freq
// 2. Evict oldest learned_at
// 3. Evict shortest words (may contain noise)
```

---

## 5. Command Interface

```bash
memory learn list [--pending] [--promoted] [--limit N]
memory learn stats
memory learn process [--limit N] [--auto] [--dry-run]
memory learn promote <word> [--entity N] [--tag T]
memory learn demote <word>
memory learn load
memory learn save
memory learn tokenize <text>
```

---

## 6. File List

### 6.1 New Files

```
learn.h                   -- Learning module header
learn_tracker.c           -- UNK tracking + candidate table
learn_classifier.c        -- Context classification
learn_persist.c           -- Learned dict + queue persistence
learn_cli.c               -- memory learn subcommands
```

### 6.2 Modified Files

```
src/memory.h              -- Conditional compile entry points
tests/test.sh             -- Learning module tests
Makefile                  -- Build options
```

---

## 7. Test Strategy

### 7.1 Unit Tests

```bash
test_learn_tracker: Manual UNK insert, verify counting
test_learn_ngram:   "NewTech released" -> identify "NewTech" 2-gram candidate
test_learn_dedup:   Same candidate deduplication
test_learn_lru:     Correct eviction at capacity
test_learn_classify: "NewTech company IPO" -> classify as brand(ent=18)
test_learn_persist: Write -> reload -> hit
```

### 7.2 Integration Tests

```bash
# Learning module integration
[9/9] Learning Module
  Tracking -> 3 occurrences enters queue
  Classification + promotion -> new word in learned dict
  Persistence -> flush/load preserves learned words
```

---

## 8. Milestones

### Phase 1 -- UNK Tracking Skeleton (1 day)

**Goal**: UNK n-gram identification + counting + candidate management + CLI

- [ ] Candidate table structure
- [ ] `scan_unk_ngrams()` - scan UNK segments after tokenization
- [ ] `candidate_add()` - insert/update candidate
- [ ] `candidate_list()` - list candidates
- [ ] `learn_queue_push()` - enqueue when threshold reached
- [ ] `memory learn list/stats` subcommands
- [ ] Conditional compilation `HIPOOL_ENABLE_SEMANTIC_LEARN`
- [ ] All existing tests pass

### Phase 2 -- Classification (2 days)

**Goal**: Automatic classification + promotion to learned dictionary

- [ ] `classify_from_context()` - neighbor voting
- [ ] `write_learned_entry()` - write to learned dict
- [ ] `hs_rebuild_buckets()` - rebuild hash buckets
- [ ] `memory learn process/list/save` commands
- [ ] `memory learn promote/demote` commands
- [ ] Persistence `lexicon_learned.dat` + `learn_queue.dat`

### Phase 3 -- Robustness (1 day)

**Goal**: Production-ready, crash-proof, controlled growth

- [ ] LRU eviction (candidate + learned)
- [ ] Error handling
- [ ] English token recognition
- [ ] Automated cron scripts
- [ ] Logging
- [ ] Performance regression tests

---

## 9. Risks and Mitigation

| Risk | Probability | Impact | Mitigation |
|------|------------|--------|------------|
| Misclassification hurts search quality | Medium | Medium | demote command for rollback |
| Candidate table fills with noise | High | Low | Short words filtered, LRU eviction |
| Performance regression (bucket rebuild) | Low | Low | O(N) rebuild, N < 12000 |
| Memory overflow | Low | High | Compile-time constants, LRU safety |

---

## Document Version

| Version | Date | Changes |
|---------|------|---------|
| v1.0 | 2026-07-19 | Initial draft |
