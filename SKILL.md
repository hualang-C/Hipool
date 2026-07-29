# hipool Adaptive Dictionary Maintenance Guide

## Overview

hipool semantic search relies on a three-layer knowledge system, each solving a different problem:

| Layer | Component | Purpose | Modification Frequency |
|-------|-----------|---------|----------------------|
| **L1 Lexicon** | `lexicon/lexicon_generated.h` | Enables FMM tokenizer to recognize words for vector encoding | Daily |
| **L2 Classification** | `src/memory.h` entity constants | Clusters similar entities on Z-axis for semantic search | When new categories emerge |
| **L3 Knowledge Graph** | `memory_data/kg_data.bin` | Explicit relations for re-ranking and filtering results | When relationships emerge |

**Three-layer workflow**:
```
LLM discovers new word -> L1 lexicon update -> L2 if new category -> add entity constant
                                                                   |
                                                            collect relations -> L3 knowledge graph triples
                                                                                          |
                                                                   recompile -> improved search quality
```

---

## Part 1: L1 -- Lexicon Maintenance

### 1.1 Scan for UNK words

The FMM tokenizer in `src/semantic.c` implements `hs_tokenize()`. Words not in the lexicon are tagged `HS_TAG_UNK`.

```
Input: "artist_1 released a new version"
Tokens:
  [artist_1](UNK)          <- not in lexicon
  [released](ent=10, verb) <- lexicon hit
  [a](F, adverb)           <- lexicon hit
  [new](UNK)               <- not in lexicon
  [version](UNK)           <- not in lexicon
```

### 1.2 Extract Candidates

From UNK segments extract:
- Length 2-6 CJK characters, or 2-16 ASCII characters
- Exclude pure digits, single-character noise
- English contiguous letters preserved as whole (e.g., `entity_a`, `concept_b`)

### 1.3 Classification

#### entity_id Reference

```c
// Entity constants from src/memory.h (also the Z-axis encoding basis for semantic search)
#define ENTITY_FRUIT      1   // Fruit
#define ENTITY_SEAFOOD    2   // Seafood
#define ENTITY_BIRD       3   // Bird
#define ENTITY_ANIMAL     4   // Animal
#define ENTITY_VEGETABLE  5   // Vegetable
#define ENTITY_FOOD       6   // Food
#define ENTITY_FURNITURE  7   // Furniture
#define ENTITY_APPLIANCE  8   // Appliance
#define ENTITY_CLOTHING   9   // Clothing
#define ENTITY_VERB      10   // Verb
#define ENTITY_NUMERAL   11   // Numeral/measure word
#define ENTITY_VEHICLE   12   // Vehicle
#define ENTITY_BUILDING  13   // Building
#define ENTITY_BODY      14   // Body
#define ENTITY_COLOR     15   // Color
#define ENTITY_NATURE    16   // Weather/Nature
#define ENTITY_ADJECTIVE 17   // Adjective
#define ENTITY_BRAND     18   // Brand/Company
#define ENTITY_TECH      19   // Technology/Framework
#define ENTITY_LITERATURE 20  // Literary works
#define ENTITY_PERSON    21   // Historical/notable persons
#define ENTITY_TOY       22   // Toys
#define ENTITY_APPLIANCE_HOME 23 // Home appliances
#define ENTITY_NETWORK_DEVICE 24 // Network devices
// LLM append new categories here ->
```

#### POS Tag Reference

```c
HS_TAG_M   = 0  // Noun (person/place/thing/concept)
HS_TAG_D   = 1  // Verb (action/behavior)
HS_TAG_X   = 2  // Adjective (description/state)
HS_TAG_S   = 3  // Numeral/measure word
HS_TAG_D1  = 4  // Pronoun
HS_TAG_F   = 5  // Adverb
HS_TAG_UNK = -1 // Unknown (not in lexicon)
```

#### Brand/Technology Classification Rules

**Brand/Company (ent=18)**: Words containing company indicators, or known company names
**Technology/Framework (ent=19)**: Words containing tech indicators, or programming languages/frameworks/tools
**General Noun (ent=0)**: Anything not matching the above categories

### 1.4 Modify lexicon_generated.h

Find the end of the array and append after the last entry:

```c
    /* existing entries */
    {"existing_word", HS_TAG_M, 6},  /* Food -- existing */
    /* LLM appended new words below */
    {"entity_a", HS_TAG_M, 18},  /* Brand/Company -- YYYY-MM-DD */
    {"concept_x", HS_TAG_M, 19}, /* Technology/Framework -- YYYY-MM-DD */
    {"new_term", HS_TAG_M, 0},   /* General noun -- YYYY-MM-DD */
};
```

**Format**: `{"word_text", HS_TAG_?, ?}, /* category comment -- YYYY-MM-DD */`

**Constraints**:
- Only append, do not delete or modify existing entries
- Do not modify `HS_DICT_GENERATED_ENTRIES` (sizeof auto-calculates)
- Do not modify `hs_dict[]` (handcrafted lexicon), only modify `hs_dict_generated[]`

---

## Part 2: L2 -- Custom Classification

When a new word **cannot be classified into existing categories 0-24**, the LLM should create a new category.

### 2.1 When to Create a New Category

Create a new entity when:
- **3 or more** different words can be classified into the same category
- The new category words appear frequently in semantic search queries
- The new category entities are far from any existing category on the Z-axis

### 2.2 Procedure

**Step 1**: Append entity constant in `src/memory.h`

```c
/* LLM append new categories here */
#define ENTITY_LITERATURE 20    // Literary works -- YYYY-MM-DD
#define ENTITY_PERSON    21    // Notable persons -- YYYY-MM-DD
#define ENTITY_TOY       22    // Toys -- YYYY-MM-DD
```

**Step 2**: Update Z-axis array upper limit in `src/semantic.c` `hs_encode()`

Find:
```c
    for (int e = 1; e <= 24; e++) {  // change this number to match new max entity_id
```
Change to the current maximum `ENTITY_XXX` value.

**Step 3**: Reclassify existing lexicon entries

In `lexicon/lexicon_generated.h`, change old entity values to new ones:
```c
    {"entity_y", HS_TAG_M, 20},  // was 0 (general noun), now 20 (literary works)
```

**Step 4**: Update this SKILL.md entity reference table

### 2.3 Effect After Adding

Similar entities cluster together on the Z-axis, different categories spread apart:
```
Fruit(ent=1): Z~0.03    Literature(ent=20): Z~0.60    Persons(ent=21): Z~0.63
Search "fruit": apple(0.04) watermelon(0.05) vs novel(0.22) <- Z-axis clusters similar types
```

---

## Part 3: L3 -- Knowledge Graph

### 3.1 What is a Knowledge Graph

A knowledge graph is a collection of **triples**: `subject --[relation]--> object`.

```
artist_1 --[dynasty]--> dynasty_a
artist_1 --[identity]--> painter
emperor_1 --[dynasty]--> dynasty_a
emperor_1 --[created]--> style_x
item_a --[category]--> toy
item_b --[category]--> movie
item_c --[purpose]--> function_x
device_a --[part_of]--> network_equipment
```

When a user searches for "dynasty_a", 7D vector search returns `[artist_1(0.11), item_a(0.11), ...]`.

After knowledge graph re-ranking:
- artist_1 -- graph has `artist_1--[dynasty]-->dynasty_a` -- **1 hop** -- boost
- item_a -- graph has no "dynasty_a" relation -- **irrelevant** -- penalize

Result: artist_1 ranked first, item_a pushed down.

### 3.2 Relation Types Reference

```c
// src/knowledge_graph.h -- KGRelationType enum
REL_NONE       = 0   // Unspecified
REL_IS_A       = 1   // Is a (artist_1 IS_A person)
REL_CATEGORY   = 2   // Category (item_b CATEGORY movie)
REL_DYNASTY    = 3   // Dynasty (artist_1 DYNASTY dynasty_a)
REL_IDENTITY   = 4   // Identity (artist_1 IDENTITY painter)
REL_INVENTED   = 5   // Invented (emperor_1 INVENTED style_x)
REL_LOCATED    = 6   // Located in (item_c LOCATED kitchen)
REL_USED_FOR   = 7   // Used for (item_c USED_FOR function_x)
REL_SIMILAR    = 8   // Similar to (item_b SIMILAR item_a)
REL_OPPOSITE   = 9   // Opposite (good OPPOSITE bad)
REL_PART_OF    = 10  // Part of (device_a PART_OF network_equipment)
// LLM append new relation types here ->
```

### 3.3 When to Create a New Relation

- Two entities have a clear, stable semantic connection
- This relation helps exclude irrelevant search results
- The relation is factual (not temporary or subjective)

### 3.4 Storage

Knowledge graph data is stored in `memory_data/kg_data.bin`.
Loaded automatically on `memory_init()`, saved automatically on `memory_destroy()`.
In-memory footprint approximately 200KB (2048 nodes + 8192 triples).

### 3.5 Procedure

**Step 1**: Add or extend relation types

Append to the `KGRelationType` enum in `src/knowledge_graph.h`:
```c
    REL_DYNASTY    = 3,
    // ...
    REL_PART_OF    = 10,
    REL_WRITTEN_BY = 11,  // novel WRITTEN_BY author -- YYYY-MM-DD
};
```

**Step 2**: Use `tools/kg_populate.c` to fill data

`kg_populate.c` is the knowledge graph population tool. Add nodes and triples in `main()`:
```c
    /* New relations */
    kg_add_node(&kg, "author_a", 21);     // Notable person
    kg_add_node(&kg, "novel_a", 20);      // Literary work

    kg_add_triple(&kg, "novel_a", REL_WRITTEN_BY, "author_a", 1.0f);
```

Where `REL_WRITTEN_BY` is the relation type added in Step 1.

**Step 3**: Compile and run the population tool

```bash
gcc -O2 tools/kg_populate.c src/knowledge_graph.c -I src -o build/kg_populate
./build/kg_populate memory_data
```

### 3.6 Verification

```bash
# Rebuild semantic version (with KG)
gcc -O2 -DHIPOOL_ENABLE_SEMANTIC src/*.c -I src -o build/memory-semantic -lm

# Test
./build/memory-semantic sem-search "dynasty_a"
# Expected: artist_1(0.13) emperor_1(0.32) ranked high, item_a/device_a pushed down

./build/memory-semantic sem-search "function_x"
# Expected: item_c(0.26) first, others 0.5+
```

### 3.7 Workflow Summary

```
LLM discovers:
  "artist_1" and "dynasty_a" often appear together
  -> this is a stable factual relation -> add to knowledge graph

LLM discovers:
  "novel_a" and "novel_b" are both literary works
  -> ENTITY_LITERATURE=20 exists -> ensure lexicon entity=20

LLM discovers:
  "app" is not in lexicon and cannot be classified into 0-24
  -> wait for 3+ similar words before creating new category
  -> classify as general noun (ent=0) for now, upgrade later
```

### 3.8 Constraints

- Only append, never modify: do not delete or change existing triples
- Relations must be **factual** (not temporary or subjective)
- Default weight 1.0, only use 0.5-0.8 for low confidence
- KG data file `kg_data.bin` is overwritten each time `kg_populate` runs

---

## Part 4: Complete Workflow

### Daily Maintenance Checklist

```
1. Scan UNK words
     Review stored text, extract UNK segments from FMM tokenizer

2. L1 Lexicon Management
     - Determine entity_id + POS tag for each candidate word
     - Append to lexicon/lexicon_generated.h
     - If search terms are also UNK, add them to lexicon too

3. L2 Classification Management
     - Check if >=3 words can be classified into one new category
     - If yes -> add ENTITY_XXX define in memory.h
     - If yes -> update for loop upper bound in semantic.c
     - If yes -> reclassify existing words to new entity
     - Update SKILL.md entity reference table

4. L3 Knowledge Graph
     - Check for stable semantic relations between entities
     - If yes -> add nodes + triples in tools/kg_populate.c
     - If new relation type needed -> add enum in knowledge_graph.h
     - Compile and run: ./build/kg_populate memory_data

5. Verification
     - Standard build: gcc -O2 src/*.c -I src -o build/memory
     - Semantic build: gcc -O2 -DHIPOOL_ENABLE_SEMANTIC src/*.c -I src -o build/memory-semantic -lm
     - Run tests: bash tests/test.sh
     - Manual test: ./build/memory-semantic sem-search "<new word>"

6. Commit
     git add src/memory.h src/semantic.c src/knowledge_graph.h
         lexicon/lexicon_generated.h tools/kg_populate.c SKILL.md
     git commit -m "daily lexicon: add N words / M relations / P new categories"
```

### Three-Layer Effectiveness Comparison

| Scenario | No maintenance | L1 only | L1+L2 | L1+L2+L3 |
|----------|---------------|---------|-------|----------|
| Search "dynasty_a" for artist_1 | artist_1 rank 5, item_a rank 1 | artist_1 rank 1 but item_a rank 2 | artist_1 rank 1, item_a further | **artist_1 rank 1, item_a penalized to rank 5** |
| Search "toy" for item_a | item_a rank 2, person rank 1 | item_a rank 2, person rank 1 | item_a rank 1, item_b rank 2 | **item_a rank 1, item_b rank 2, others penalized** |
| Search "function_x" for item_c | item_c rank 1 but small gap | item_c rank 1 | item_c rank 1 | **item_c rank 1, gap 0.5+** |

---

## Part 5: Code Map

```
Semantic search data flow (three-layer collaboration):

  memory set "text"
    |
  semantic.c: hs_index_entry()
    |- hs_tokenize()    -- L1 lexicon FMM tokenization
    |    |               (lexicon/lexicon_generated.h)
    |- hs_encode()      -- 7D vector encoding, Z-axis from L2 entity
    |    |               (src/memory.h ENTITY_XXX constants)
    - store in hs_docs[] -- global semantic index

  memory sem-search "query"
    |
  semantic.c: hs_search()   -- 7D vector similarity search (TopK)
    |
  semantic.c: hs_kg_rerank() -- L3 knowledge graph re-ranking
    |                         (memory_data/kg_data.bin)
    - final sorted results

File index:
  Scenario              File                         Operation
  Add word              lexicon/lexicon_generated.h   Append to array end
  Add category          src/memory.h                  Add #define
                        src/semantic.c                Update for loop bound
                        SKILL.md                      Update entity reference table
  Add relation type     src/knowledge_graph.h         Add enum value
  Add relation data     tools/kg_populate.c           Add kg_add_triple
                        Run ./build/kg_populate       Write kg_data.bin
  Build verification    Command line gcc              No errors = pass
```
