/*
 * dedup.c — 同日期去重
 *
 * 检查同一天是否已有相似内容的条目。
 * 基于长度比和字符重叠率的双重检测。
 */
#include "memory.h"

int dedup_same_day(MemoryCtx *ctx, const char *key, const char *val) {
    const char *last_dash = strrchr(key, '-');
    if (!last_dash) return 0;
    int dash_pos = (int)(last_dash - key);
    if (dash_pos < 10) return 0;

    size_t vl = strlen(val);
    if (vl < 20) return 0;

    int checked = 0;
    for (int b = 0; b < HASHTABLE_SIZE && checked < 10; b++) {
        HashNode *n = ctx->table.b[b];
        while (n && checked < 10) {
            MemEntry *e = n->e;
            if (!(e->flags & FLAG_ACTIVE)) { n = n->next; continue; }
            char *ek = ENTRY_KEY(e);
            size_t ekl = e->key_len;
            if (ekl >= (size_t)dash_pos
                && strncmp(ek, key, dash_pos) == 0
                && ek[dash_pos] == '-') {
                checked++;
                char *ev = ENTRY_VAL(e);
                size_t evl = e->val_len;
                if ((vl >= evl && (double)evl / vl > 0.60) ||
                    (evl >= vl && (double)vl / evl > 0.60)) {
                    const char *shorter = vl < evl ? val : ev;
                    size_t sl = vl < evl ? vl : evl;
                    const char *longer = vl < evl ? ev : val;
                    /* [P1-9] 旧实现在内层循环每次调用 strlen(longer),
                     * 复杂度退化为 O(sl × len(longer)) (编译器未必能证明
                     * memchr 不会改写 longer 而消除 strlen)。longer 的长度
                     * == max(vl, evl), 提到循环外缓存。 */
                    size_t longer_len = vl < evl ? evl : vl;
                    size_t match_chars = 0;
                    for (size_t i = 0; i < sl; i++) {
                        if (memchr(longer, shorter[i], longer_len))
                            match_chars++;
                    }
                    if ((double)match_chars / sl > 0.80) return 1;
                }
            }
            n = n->next;
        }
    }
    return 0;
}
