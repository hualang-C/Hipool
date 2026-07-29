/*
 * memory_api.c — 公开 API 实现
 *
 * 记忆系统的核心操作: init / destroy / set / get / del / flush / load / stats
 * 以及 shard 管理和 fork 快照。
 */
#include "memory.h"
#include "knowledge_graph.h"
#include <dirent.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

/* Windows 兼容函数 */
#ifdef _WIN32
void win_argv_to_utf8(int *argc, char ***argv) {
    (void)argc;
    for (int i = 0; i < (*argc); i++) {
        int wlen = MultiByteToWideChar(CP_ACP, 0, (*argv)[i], -1, NULL, 0);
        if (wlen <= 0) continue;
        wchar_t *wb = (wchar_t*)malloc((size_t)wlen * sizeof(wchar_t));
        MultiByteToWideChar(CP_ACP, 0, (*argv)[i], -1, wb, wlen);
        int ulen = WideCharToMultiByte(CP_UTF8, 0, wb, -1, NULL, 0, NULL, NULL);
        if (ulen <= 0) { free(wb); continue; }
        char *ub = (char*)malloc((size_t)ulen);
        WideCharToMultiByte(CP_UTF8, 0, wb, -1, ub, ulen, NULL, NULL);
        free(wb);
        free((*argv)[i]);
        (*argv)[i] = ub;
    }
}
#endif

int memory_init(MemoryCtx *ctx, const char *dd, int ttl) {
    memset(ctx, 0, sizeof(MemoryCtx));
    if (pool_init(&ctx->pool, POOL_SIZE) < 0) return -1;
    hash_init(&ctx->table); tag_init(&ctx->tag_index);
    if (sl_init(&ctx->sorted) < 0) return -1;
    strncpy(ctx->data_dir, dd, sizeof(ctx->data_dir) - 1);
    ctx->data_dir[sizeof(ctx->data_dir) - 1] = '\0';
    ctx->file_ttl = ttl > 0 ? ttl : FILE_TTL_DAYS;
    ctx->initialized = 1;
#ifdef HIPOOL_USE_MUTEX
    pthread_mutex_init(&ctx->lock, NULL);
#endif
    ctx->wal_fp = NULL;
    ctx->wal_disabled = 1;
    ovf_load(ctx); ovf_clean(ctx);
    char wf[1024]; wal_path(wf, sizeof(wf), ctx->data_dir);
    int restored = wal_replay(ctx, wf);
    wal_compact(ctx);
    ctx->wal_fp = fopen(wf, "ab");
    ctx->wal_disabled = 0;
    if (restored > 0) fprintf(stderr, "wal_replay: restored %d entries\n", restored);

#ifdef HIPOOL_ENABLE_SEMANTIC
    /* 初始化知识图谱 */
    ctx->kg = (KnowledgeGraph*)calloc(1, sizeof(KnowledgeGraph));
    if (ctx->kg) {
        char kg_path[1024];
        snprintf(kg_path, sizeof(kg_path), "%s/kg_data.bin", ctx->data_dir);
        kg_load(ctx->kg, kg_path);
    }
#endif

    /* 自动发现已有 shard 目录 */
    DIR *d = opendir(ctx->data_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strncmp(de->d_name, "shard_", 6) != 0) continue;
            char sp[1024]; struct stat st;
            snprintf(sp, sizeof(sp), "%s/%s", ctx->data_dir, de->d_name);
            if (stat(sp, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            char sname[64];
            snprintf(sname, sizeof(sname), "%s", de->d_name + 6);
            if (!memory_shard_get(ctx, sname))
                memory_shard_ensure(ctx, sname);
        }
        closedir(d);
    }
    return 0;
}

void memory_destroy(MemoryCtx *ctx) {
    memory_flush_shards(ctx);

    for (int i = 0; i < ctx->shard_count; i++) {
        MemoryCtx *sc = ctx->shards[i].ctx;
        if (!sc) continue;
        for (int b = 0; b < HASHTABLE_SIZE; b++) {
            HashNode *n = sc->table.b[b];
            while (n) { HashNode *nx = n->next; free(n); n = nx; }
        }
        for (int b = 0; b < TAG_HASHTABLE_SIZE; b++) {
            TagEntry *te = sc->tag_index.b[b];
            while (te) { TagEntry *nx = te->next; free(te->es); free(te); te = nx; }
        }
        sl_destroy(&sc->sorted);
        free(sc->pool.base);
        free(sc);
        ctx->shards[i].ctx = NULL;
    }
    ctx->shard_count = 0;

    if (ctx->wal_fp) { fclose(ctx->wal_fp); ctx->wal_fp = NULL; }

#ifdef HIPOOL_ENABLE_SEMANTIC
    if (ctx->kg) {
        char kg_path[1024];
        snprintf(kg_path, sizeof(kg_path), "%s/kg_data.bin", ctx->data_dir);
        kg_save(ctx->kg, kg_path);
        free(ctx->kg);
        ctx->kg = NULL;
    }
#endif

    for (int b = 0; b < HASHTABLE_SIZE; b++) {
        HashNode *n = ctx->table.b[b];
        while (n) { HashNode *nx = n->next; free(n); n = nx; }
    }
    for (int b = 0; b < TAG_HASHTABLE_SIZE; b++) {
        TagEntry *te = ctx->tag_index.b[b];
        while (te) { TagEntry *nx = te->next; free(te->es); free(te); te = nx; }
    }
    sl_destroy(&ctx->sorted);
    free(ctx->pool.base);
#ifdef HIPOOL_USE_MUTEX
    pthread_mutex_destroy(&ctx->lock);
#endif
    memset(ctx, 0, sizeof(MemoryCtx));
}

int memory_set_with_ts_unlocked(MemoryCtx *ctx, const char *key, const char *val,
                                const char **tags, int tc, uint64_t ts) {
    if (!ctx->initialized || !key || !val) return -1;
    if (tc > MAX_TAGS) tc = MAX_TAGS;
    size_t kl = strlen(key), vl = strlen(val);
    if (kl > MAX_KEY_LEN) kl = MAX_KEY_LEN;
    if (vl > MAX_VAL_LEN) vl = MAX_VAL_LEN;

    char tk[MAX_KEY_LEN + 1]; memcpy(tk, key, kl); tk[kl] = '\0';
    uint32_t kh = djb2(tk);

    if (ctx->pool.wl >= EVICT_WATER) ovf_evict_oldest(ctx);
    if (dedup_same_day(ctx, key, val)) return -2;

    /* [P0-3] Write-Ahead 语义: 先写 WAL, 再改内存。
     * 原实现把 wal_append 放在最后, 内存已改但 WAL 未落盘的窗口崩溃会丢数据,
     * 违背 WAL 的核心保证。注意重放期间 wal_disabled=1, wal_append 直接返回,
     * 不会递归。 */
    uint64_t eff_ts = ts ? ts : (uint64_t)time(NULL);
    if (!ctx->wal_disabled) {
        wal_append(ctx, WAL_OP_SET, key, val, tags, tc, eff_ts);
    }

    /* [P1-5] 先分配再覆盖旧条目。原实现先 memory_del_unlocked 再 pool_alloc,
     * 若 alloc 失败旧数据已被删除 → 数据丢失。此处用 mem_remove_entry
     * (不写 WAL DEL, 因为上面的 WAL SET 已隐含覆盖语义) 删旧内存条目。 */
    size_t tot = entry_total_size((uint16_t)kl, (uint16_t)vl, (uint8_t)tc);
    void *ptr = pool_alloc(&ctx->pool, tot);
    if (!ptr) return -1;
    mem_remove_entry(ctx, key);   /* 覆盖同 key 旧条目, 不产生 WAL 记录 */

    MemEntry *e = (MemEntry*)ptr;
    e->magic_start = CANARY_MAGIC; e->key_hash = kh;
    e->key_len = (uint16_t)kl; e->val_len = (uint16_t)vl; e->tag_count = (uint8_t)tc;
    e->flags = FLAG_ACTIVE; e->_pad[0] = e->_pad[1] = 0;
    e->created_at = eff_ts;
    e->accessed_at = (uint64_t)time(NULL);

    char *kp = ENTRY_KEY(e), *vp = ENTRY_VAL(e), *tp = ENTRY_TAGS(e);
    memcpy(kp, key, kl); kp[kl] = '\0';
    memcpy(vp, val, vl); vp[vl] = '\0';
    memset(tp, 0, (size_t)tc * MAX_TAG_LEN);
    for (int i = 0; i < tc; i++)
        if (tags[i]) strncpy(tp + i * MAX_TAG_LEN, tags[i], MAX_TAG_LEN - 1);
    hash_insert(&ctx->table, kh, e);
    sl_insert(&ctx->sorted, e);
    for (int i = 0; i < tc; i++)
        if (tags[i] && tags[i][0]) tag_add(&ctx->tag_index, tags[i], e);
    ctx->pool.ec++;

#ifdef HIPOOL_ENABLE_SEMANTIC
    hs_index_entry(kh, val);
#endif
    return 0;
}

int memory_set_with_ts(MemoryCtx *ctx, const char *key, const char *val,
                       const char **tags, int tc, uint64_t ts) {
    LOCK(ctx);
    int rc = memory_set_with_ts_unlocked(ctx, key, val, tags, tc, ts);
    UNLOCK(ctx);
    return rc;
}

int memory_set(MemoryCtx *ctx, const char *key, const char *val,
               const char **tags, int tc) {
    LOCK(ctx);
    int rc = memory_set_with_ts_unlocked(ctx, key, val, tags, tc, 0);
    UNLOCK(ctx);
    return rc;
}

char *memory_get(MemoryCtx *ctx, const char *key) {
    if (!ctx->initialized || !key) return NULL;

    /* [P0-2] 旧实现返回内存池内部裸指针且提前解锁: 多线程下另一线程
     * set/del 触发 pool_free 会使该指针失效(use-after-free); 且两条返回
     * 路径(内存命中返回池指针 vs 磁盘命中返回 malloc 字符串)语义不一致,
     * 调用方无法判断该不该 free → 内存泄漏。现统一返回 malloc 拷贝,
     * 调用方负责 free。 */
    char *val_copy = NULL;
    LOCK(ctx);
    size_t kl = strlen(key); if (kl > MAX_KEY_LEN) kl = MAX_KEY_LEN;
    MemEntry *e = hash_lookup(&ctx->table, djb2_n(key, kl), key);
    if (e) val_copy = strdup(ENTRY_VAL(e));
    UNLOCK(ctx);
    if (val_copy) return val_copy;

    /* [P1-7] 磁盘扫描移出锁外: scan_day_files 仅读磁盘文件, 不访问共享
     * 内存结构, 持锁会阻塞所有并发操作。 */
    return scan_day_files(ctx, key);
}

/* 仅从内存结构中移除并释放某 key 对应条目, 不写 WAL 记录。
 * 供 memory_set_with_ts_unlocked 在已写 WAL SET 后覆盖旧条目使用
 * (避免在 SET 重放期间产生自相矛盾的 DEL 记录)。
 * 不存在该 key 时返回 -1。 */
int mem_remove_entry(MemoryCtx *ctx, const char *key) {
    if (!ctx->initialized || !key) return -1;
    size_t kl = strlen(key); if (kl > MAX_KEY_LEN) kl = MAX_KEY_LEN;
    uint32_t kh = djb2_n(key, kl);
    MemEntry *e = hash_lookup(&ctx->table, kh, key);
    if (!e) return -1;
    sl_remove(&ctx->sorted, e);
    tag_remove_all(&ctx->tag_index, e);
    hash_remove(&ctx->table, kh, key);
    e->flags = 0; pool_free(&ctx->pool, e);
    if (ctx->pool.ec > 0) ctx->pool.ec--;

#ifdef HIPOOL_ENABLE_SEMANTIC
    hs_deindex_entry(kh);
#endif
    return 0;
}

int memory_del_unlocked(MemoryCtx *ctx, const char *key) {
    /* [P0-3] 先写 WAL DEL, 再改内存, 保证 write-ahead 语义
     * (内存已删但 WAL 未写时崩溃会丢失删除意图, 重启后旧数据复活)。 */
    if (!ctx->initialized || !key) return -1;
    if (!ctx->wal_disabled) {
        wal_append(ctx, WAL_OP_DEL, key, NULL, NULL, 0, (uint64_t)time(NULL));
    }
    return mem_remove_entry(ctx, key);
}

int memory_del(MemoryCtx *ctx, const char *key) {
    LOCK(ctx);
    int rc = memory_del_unlocked(ctx, key);
    UNLOCK(ctx);
    return rc;
}

int memory_flush(MemoryCtx *ctx) {
    LOCK(ctx);
    int r = ovf_flush(ctx);
    UNLOCK(ctx);
    return r;
}

int memory_load(MemoryCtx *ctx) {
    LOCK(ctx);
    int r = ovf_load(ctx);
    UNLOCK(ctx);
    return r;
}

int memory_cleanup_ttl(MemoryCtx *ctx) {
    LOCK(ctx);
    int r = ovf_clean(ctx);
    UNLOCK(ctx);
    return r;
}

void memory_stats(const MemoryCtx *ctx, char *buf, size_t bl) {
    uint32_t total_entries = ctx->pool.ec;
    for (int i = 0; i < ctx->shard_count; i++)
        if (ctx->shards[i].ctx) total_entries += ctx->shards[i].ctx->pool.ec;

    char sb[2048];
    int pos = snprintf(sb, sizeof(sb),
        "=== Memory System Stats ===\n"
        "Pool: %zu/%zu (%.1f%%), %u entries, %lu allocs, %lu frees\n"
        "  Slab256: 0/%u   Slab1K: 0/%u   Slab4K: 0/%u   Slab16K: 0/%u\n"
        "  Hash table: %u entries / %u buckets\n"
        "  Tag index:  %u tags indexed\n"
        "  Sorted idx: %u entries / max lv %d\n"
        "  Data dir:   %s (TTL: %d days)\n"
        "  Shards:     %d total, %u entries\n",
        ctx->pool.used, ctx->pool.total,
        ctx->pool.total > 0 ? (double)ctx->pool.used * 100 / ctx->pool.total : 0,
        ctx->pool.ec, (unsigned long)ctx->pool.ta, (unsigned long)ctx->pool.tf,
        ctx->pool.sc[0], ctx->pool.sc[1], ctx->pool.sc[2], ctx->pool.sc[3],
        ctx->table.cnt, (uint32_t)HASHTABLE_SIZE, ctx->tag_index.cnt,
        ctx->sorted.count, ctx->sorted.top_level,
        ctx->data_dir, ctx->file_ttl,
        ctx->shard_count, total_entries);

    for (int i = 0; i < ctx->shard_count; i++) {
        MemoryCtx *sc = ctx->shards[i].ctx;
        if (!sc) continue;
        pos += snprintf(sb + pos,
            (pos < (int)sizeof(sb) && (size_t)pos < sizeof(sb)) ? sizeof(sb) - (size_t)pos : 0,
            "    [%s] %u entries, %zu/%zu bytes\n",
            ctx->shards[i].name, sc->pool.ec, sc->pool.used, sc->pool.total);
    }
    snprintf(buf, bl, "%s", sb);
}

/* ===================== Shard 管理 ===================== */

/* [P1-11] 校验标识符(如 shard 名)仅含 [A-Za-z0-9_-], 非空, 长度<=63。
 * 防止 --shard ../../etc/evil 之类的路径遍历 (name 会被拼进
 * "%s/shard_%s" 做 mkdir + 文件写入)。返回 0 合法, 非 0 非法。 */
int validate_name(const char *name) {
    if (!name || !name[0]) return -1;
    for (size_t i = 0; name[i]; i++) {
        if (i >= 63) return -1;
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return -1;
    }
    return 0;
}

/* [P1-11] 拒绝数据目录路径中的 ".." 成分 (含 "/../", "\\..", 以 ".." 开头等),
 * 防止 --dir 指向越权目录后写文件。允许相对/绝对路径, 但路径段不得为 ".."。 */
int validate_data_dir(const char *dir) {
    if (!dir || !dir[0]) return -1;
    /* 逐段检查: 按分隔符 / 或 \ 切分, 任一段为 ".." 即拒绝 */
    const char *p = dir;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '/' && *p != '\\') p++;
        size_t slen = (size_t)(p - seg);
        if (slen == 2 && seg[0] == '.' && seg[1] == '.') return -1;
        if (*p) p++;
    }
    return 0;
}

MemoryCtx *memory_shard_get(MemoryCtx *ctx, const char *name) {
    if (!name || name[0] == '\0' || strcmp(name, "default") == 0)
        return ctx;
    for (int i = 0; i < ctx->shard_count; i++)
        if (strcmp(ctx->shards[i].name, name) == 0)
            return ctx->shards[i].ctx;
    return NULL;
}

MemoryCtx *memory_shard_ensure(MemoryCtx *ctx, const char *name) {
    if (!name || name[0] == '\0' || strcmp(name, "default") == 0)
        return ctx;
    if (validate_name(name) != 0) {   /* [P1-11] 拒绝路径遍历 */
        fprintf(stderr, "invalid shard name (allowed: [A-Za-z0-9_-])\n");
        return NULL;
    }
    MemoryCtx *existing = memory_shard_get(ctx, name);
    if (existing) return existing;
    if (ctx->shard_count >= MAX_SHARDS) {
        fprintf(stderr, "max shards (%d) reached\n", MAX_SHARDS);
        return NULL;
    }

    int idx = ctx->shard_count;
    strncpy(ctx->shards[idx].name, name, 63);
    ctx->shards[idx].name[63] = '\0';

    char sd[1024];
    snprintf(sd, sizeof(sd), "%s/shard_%s", ctx->data_dir, name);

    MemoryCtx *sc = (MemoryCtx*)calloc(1, sizeof(MemoryCtx));
    if (!sc) return NULL;
    ctx->shards[idx].ctx = sc;
    ctx->shard_count++;

    sc->wal_disabled = 1;
    if (pool_init(&sc->pool, SHARD_POOL_SIZE) < 0) { free(sc); ctx->shard_count--; return NULL; }
    hash_init(&sc->table); tag_init(&sc->tag_index);
    sl_init(&sc->sorted);
    strncpy(sc->data_dir, sd, sizeof(sc->data_dir) - 1);
    sc->file_ttl = ctx->file_ttl;
    sc->initialized = 1;

    sc->wal_fp = NULL;
    ovf_load(sc); ovf_clean(sc);
    char wf[1024]; wal_path(wf, sizeof(wf), sd);
    int restored = wal_replay(sc, wf);
    wal_compact(sc);
    sc->wal_fp = fopen(wf, "ab");
    sc->wal_disabled = 0;
    if (restored > 0)
        fprintf(stderr, "shard '%s': restored %d entries\n", name, restored);

    return sc;
}

/* ===================== Fork 快照 ===================== */

int memory_fork_snapshot(MemoryCtx *ctx) {
#ifdef _WIN32
    (void)ctx; return -1;
#else
#ifdef HIPOOL_USE_MUTEX
    fprintf(stderr, "fork_snapshot: unsafe with threads, skipping\n");
    return -1;
#endif
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid > 0) return (int)pid;

    ctx->wal_disabled = 1;
    int r = ovf_flush(ctx);
    for (int i = 0; i < ctx->shard_count; i++) {
        if (ctx->shards[i].ctx)
            r += ovf_flush(ctx->shards[i].ctx);
    }
    for (int fd = 3; fd < 256; fd++) close(fd);
    _exit(r >= 0 ? 0 : 1);
#endif
}

int memory_flush_shards(MemoryCtx *ctx) {
    int total = memory_flush(ctx);
    for (int i = 0; i < ctx->shard_count; i++) {
        if (ctx->shards[i].ctx) {
            LOCK(ctx->shards[i].ctx);
            total += ovf_flush(ctx->shards[i].ctx);
            UNLOCK(ctx->shards[i].ctx);
        }
    }
    return total;
}
