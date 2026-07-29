/*
 * tag_index.c — 倒排标签索引
 *
 * 512 桶, 每个桶链式存储 TagEntry
 */
#include "memory.h"

int tag_init(TagIndex *ti) {
    memset(ti->b, 0, sizeof(ti->b));
    ti->cnt = 0;
    return 0;
}

static uint32_t tag_hash(const char *t) {
    return djb2(t) % TAG_HASHTABLE_SIZE;
}

TagEntry *tag_ensure(TagIndex *ti, const char *tag) {
    uint32_t bk = tag_hash(tag);
    TagEntry *te = ti->b[bk];
    while (te) {
        if (strncmp(te->tag, tag, MAX_TAG_LEN) == 0) return te;
        te = te->next;
    }
    te = (TagEntry*)calloc(1, sizeof(TagEntry));
    if (!te) return NULL;
    strncpy(te->tag, tag, MAX_TAG_LEN - 1);
    te->tag[MAX_TAG_LEN - 1] = '\0';
    te->n = 0; te->cap = 0; te->es = NULL;
    te->next = ti->b[bk]; ti->b[bk] = te; ti->cnt++;
    return te;
}

int tag_add(TagIndex *ti, const char *tag, MemEntry *e) {
    TagEntry *te = tag_ensure(ti, tag);
    if (!te) return -1;
    for (uint32_t i = 0; i < te->n; i++)
        if (te->es[i] == e) return 0;
    if (te->n >= te->cap) {
        uint32_t nc = te->cap ? te->cap * 2 : 8;
        MemEntry **na = (MemEntry**)realloc(te->es, nc * sizeof(MemEntry*));
        if (!na) return -1;
        te->es = na; te->cap = nc;
    }
    te->es[te->n++] = e;
    return 0;
}

MemEntry **tag_search(TagIndex *ti, const char *tag, uint32_t *cnt) {
    TagEntry *te = ti->b[tag_hash(tag)];
    while (te) {
        if (strncmp(te->tag, tag, MAX_TAG_LEN) == 0) {
            *cnt = te->n;
            if (te->n == 0) return NULL;
            MemEntry **r = (MemEntry**)malloc(te->n * sizeof(MemEntry*));
            if (!r) { *cnt = 0; return NULL; }
            memcpy(r, te->es, te->n * sizeof(MemEntry*));
            return r;
        }
        te = te->next;
    }
    *cnt = 0; return NULL;
}

void tag_remove_all(TagIndex *ti, MemEntry *e) {
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
