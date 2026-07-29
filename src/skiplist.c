/*
 * skiplist.c — Skip List 排序索引
 *
 * 按 created_at 排序, O(log n) 插入/删除, O(log n + k) 范围查询
 * 复合排序键: (created_at << 32) | key_hash
 */
#include "memory.h"
#include <stdlib.h>

static int sl_rand_level(void) {
    int lv = 0;
    while (lv < SL_MAX_LEVEL - 1 && (rand() & (SL_PROB - 1)) == 0) lv++;
    return lv;
}

int sl_init(SkipList *sl) {
    memset(sl, 0, sizeof(*sl));
    sl->top_level = 0; sl->count = 0;
    sl->head = (SLNode*)calloc(1, sizeof(SLNode) + SL_MAX_LEVEL * sizeof(SLNode*));
    if (!sl->head) return -1;
    sl->head->level = 0; sl->head->entry = NULL; sl->head->sort_key = 0;
    memset(sl->head->next, 0, SL_MAX_LEVEL * sizeof(SLNode*));
    return 0;
}

int sl_insert(SkipList *sl, MemEntry *entry) {
    uint64_t sk = make_sort_key(entry->created_at, entry->key_hash);
    SLNode *update[SL_MAX_LEVEL];
    SLNode *cur = sl->head;

    for (int i = sl->top_level; i >= 0; i--) {
        while (cur->next[i] && cur->next[i]->sort_key < sk)
            cur = cur->next[i];
        update[i] = cur;
    }

    SLNode *nxt = cur->next[0];
    if (nxt && nxt->sort_key == sk && nxt->entry == entry) return 0;

    int nlv = sl_rand_level();
    if (nlv > sl->top_level) {
        for (int i = sl->top_level + 1; i <= nlv; i++)
            update[i] = sl->head;
        sl->top_level = nlv;
    }

    SLNode *nn = (SLNode*)calloc(1, sizeof(SLNode) + (nlv + 1) * sizeof(SLNode*));
    if (!nn) return -1;
    nn->entry = entry; nn->sort_key = sk; nn->level = (uint8_t)nlv;

    for (int i = 0; i <= nlv; i++) {
        nn->next[i] = update[i]->next[i];
        update[i]->next[i] = nn;
    }
    sl->count++;
    return 0;
}

void sl_remove(SkipList *sl, MemEntry *entry) {
    uint64_t sk = make_sort_key(entry->created_at, entry->key_hash);
    SLNode *update[SL_MAX_LEVEL];
    SLNode *cur = sl->head;

    for (int i = sl->top_level; i >= 0; i--) {
        while (cur->next[i] && cur->next[i]->sort_key < sk)
            cur = cur->next[i];
        update[i] = cur;
    }

    SLNode *target = cur->next[0];
    if (!target || target->sort_key != sk || target->entry != entry) return;

    int tlv = (int)target->level;
    for (int i = 0; i <= tlv && i <= sl->top_level; i++)
        update[i]->next[i] = target->next[i];

    free(target);
    sl->count--;

    while (sl->top_level > 0 && !sl->head->next[sl->top_level])
        sl->top_level--;
}

int sl_range(SkipList *sl, uint64_t t_start, uint64_t t_end,
             MemEntry ***out, uint32_t *cnt) {
    *out = NULL; *cnt = 0;
    if (sl->count == 0) return 0;

    /* [P1-8] 原实现从 level 0 线性扫描到起点, 复杂度退化为 O(n + k),
     * 与注释/README 宣称的 O(log n + k) 不符, 丧失跳跃表的核心优势。
     * 改为从 top_level 逐层下降做 O(log n) 定位 (与 sl_insert/sl_remove
     * 一致的标准跳跃表查找): 每层向右走到最后一个 sort_key < sk_start
     * 的节点再下降。 */
    uint64_t sk_start = make_sort_key(t_start, 0);
    SLNode *cur = sl->head;
    for (int i = sl->top_level; i >= 0; i--) {
        while (cur->next[i] && cur->next[i]->sort_key < sk_start)
            cur = cur->next[i];
    }
    cur = cur->next[0];   /* 第一个 sort_key >= sk_start 的节点 (可能为 NULL) */

    uint32_t cap = 64, found = 0;
    MemEntry **res = (MemEntry**)malloc(cap * sizeof(MemEntry*));
    if (!res) return -1;

    while (cur && cur->entry && (cur->sort_key >> 32) < t_end) {
        if (found >= cap) {
            cap *= 2;
            MemEntry **nr = (MemEntry**)realloc(res, cap * sizeof(MemEntry*));
            if (!nr) { free(res); return -1; }
            res = nr;
        }
        res[found++] = cur->entry;
        cur = cur->next[0];
    }

    *out = res; *cnt = found;
    return 0;
}

void sl_destroy(SkipList *sl) {
    SLNode *cur = sl->head ? sl->head->next[0] : NULL;
    while (cur) { SLNode *nx = cur->next[0]; free(cur); cur = nx; }
    free(sl->head);
    memset(sl, 0, sizeof(*sl));
}
