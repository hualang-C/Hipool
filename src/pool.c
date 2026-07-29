/*
 * pool.c — Slab 内存池
 *
 * 4 级 slab: 256B / 1K / 4K / 16K
 * 比例: 10% / 20% / 30% / 40%
 * 金丝雀检测: 前后 4 字节 0xDEADBEEF
 */
#include "memory.h"

static int bm_find0(const uint8_t *bm, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (!bm_test(bm, i)) return (int)i;
    return -1;
}

int pool_init(Pool *p, size_t sz) {
    p->base = (uint8_t*)calloc(1, sz);
    if (!p->base) return -1;
    p->total = sz; p->used = 0; p->ec = 0; p->ta = p->tf = 0; p->wl = 0;
    double ratios[4] = {0.10, 0.20, 0.30, 0.40};
    size_t off = 0;
    uint32_t sizes[] = SLAB_SIZES;
    for (int lv = 0; lv < 4; lv++) {
        uint32_t ss = sizes[lv], rs = ss + 8;
        size_t budget = (size_t)(sz * ratios[lv]);
        uint32_t max = (uint32_t)((budget * 8) / (rs * 8 + 1));
        if (max < 16) max = 16;
        size_t bm_sz = bm_bytes(max); bm_sz = (bm_sz + 7) & ~7UL;
        size_t need = bm_sz + (size_t)max * rs;
        while (need > budget && max > 16) {
            max--; bm_sz = bm_bytes(max); bm_sz = (bm_sz + 7) & ~7UL;
            need = bm_sz + (size_t)max * rs;
        }
        p->so[lv] = (uint32_t)off; p->ss[lv] = ss; p->sc[lv] = max;
        p->bm[lv] = p->base + off; memset(p->bm[lv], 0, bm_sz);
        off += need;
    }
    return 0;
}

void *pool_alloc(Pool *p, size_t sz) {
    int lv;
    uint32_t sizes[] = SLAB_SIZES;
    if      (sz <= sizes[0]) lv = 0;
    else if (sz <= sizes[1]) lv = 1;
    else if (sz <= sizes[2]) lv = 2;
    else if (sz <= sizes[3]) lv = 3;
    else { HIPOOL_LOG_ERROR("pool_alloc: %zu > max\n", sz); return NULL; }
    int sl = lv, idx = -1;
    for (; lv < 4; lv++) { idx = bm_find0(p->bm[lv], p->sc[lv]); if (idx >= 0) break; }
    if (idx < 0) { HIPOOL_LOG_WARN("pool_alloc: slabs full (lv%d+)\n", sl); return NULL; }
    uint32_t rs = sizes[lv] + 8;
    size_t b_sz = bm_bytes(p->sc[lv]); b_sz = (b_sz + 7) & ~7UL;
    uint8_t *sb = p->base + p->so[lv] + (uint32_t)b_sz + (uint32_t)(idx * rs);
    *(uint32_t*)sb = CANARY_MAGIC;
    *(uint32_t*)(sb + 4 + sizes[lv]) = CANARY_MAGIC;
    bm_set(p->bm[lv], idx);
    void *up = sb + 4;
    p->used += rs; p->ta++; p->wl = (uint32_t)(p->used * 100 / p->total);
    return up;
}

void pool_free(Pool *p, void *ptr) {
    if (!ptr) return;
    uint8_t *up = (uint8_t*)ptr, *sb = up - 4;
    if (*(uint32_t*)sb != CANARY_MAGIC) { HIPOOL_LOG_ERROR("pool_free: bad canary (head)\n"); return; }
    size_t rel = (size_t)(sb - p->base); int found = 0;
    uint32_t sizes[] = SLAB_SIZES;
    for (int lv = 0; lv < 4; lv++) {
        size_t b_sz = bm_bytes(p->sc[lv]); b_sz = (b_sz + 7) & ~7UL;
        uint32_t ds = p->so[lv] + (uint32_t)b_sz, rs = sizes[lv] + 8;
        if (rel >= p->so[lv] && rel < p->so[lv] + b_sz + (size_t)p->sc[lv] * rs && rel >= ds) {
            uint32_t sr = (uint32_t)(rel - ds), idx = sr / rs;
            if (idx < p->sc[lv] && sr % rs == 0) {
                if (*(uint32_t*)(sb + 4 + sizes[lv]) != CANARY_MAGIC) {
                    /* [P1-13] canary 损坏属严重内存破坏, ERROR 级 (保留 slab/idx
                     * 上下文便于定位, 不再输出 %p 指针地址, 避免信息泄露)。 */
                    HIPOOL_LOG_ERROR("pool_free: tail canary corrupted at lv%d idx %u\n", lv, idx);
                    return;
                }
                bm_clear(p->bm[lv], idx);
                *(uint32_t*)sb = 0; *(uint32_t*)(sb + 4 + sizes[lv]) = 0;
                p->used -= rs; p->tf++; p->wl = (uint32_t)(p->used * 100 / p->total);
                found = 1; break;
            }
        }
    }
    /* [P1-13] 指针不在池中: 仅 DEBUG 输出 %p (生产构建静默, 避免泄露堆地址
     * 辅助 ASLR 绕过)。 */
    if (!found) HIPOOL_LOG_DEBUG("pool_free: ptr %p not in pool\n", (void*)ptr);
}
