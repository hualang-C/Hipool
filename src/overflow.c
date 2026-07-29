/*
 * overflow.c — 溢出管理器
 *
 * 海马体-皮层双存储模型的"皮层"部分:
 * - 快照 (memory_snapshot.json): 全部活跃条目的完整快照
 * - 日文件 (memory-YYYY-MM-DD.json): 按日期分片的溢出文件
 * - TTL 清理: 过期日文件自动删除
 * - 惰性加载: 历史数据按需加载回内存
 */
#include "memory.h"
#include <dirent.h>

int ovf_flush(MemoryCtx *ctx) {
    char fn[1024]; snprintf(fn, sizeof(fn), "%s/memory_snapshot.json", ctx->data_dir);
    /* [P1-12] 写入临时文件再 rename, 与 ovf_flush_day 一致, 保证原子性。
     * 旧实现直接 fopen("w") 覆盖写 memory_snapshot.json, 崩溃会损坏快照,
     * 与 README 宣称的"所有磁盘写入遵循原子模式(临时文件+rename)"矛盾。 */
    char tmp_fn[1024]; snprintf(tmp_fn, sizeof(tmp_fn), "%s/.tmp_memory_snapshot.json", ctx->data_dir);
    int mkr = mkdir(ctx->data_dir, 0755);
    if (mkr != 0 && errno != EEXIST) {
        if (errno == ENOTDIR) { unlink(ctx->data_dir); mkdir(ctx->data_dir, 0755); }
    }

    MemEntry *to_evict[MAX_ENTRIES];
    int n_evict = 0;
    for (int b = 0; b < HASHTABLE_SIZE; b++) {
        HashNode *n = ctx->table.b[b];
        while (n) {
            MemEntry *e = n->e;
            if ((e->flags & FLAG_ACTIVE) && n_evict < MAX_ENTRIES)
                to_evict[n_evict++] = e;
            n = n->next;
        }
    }
    if (n_evict == 0) return 0;

    FILE *fp = fopen(tmp_fn, "w"); if (!fp) return -1;
    int w = 0;
    for (int i = 0; i < n_evict; i++) {
        MemEntry *e = to_evict[i];
        char *key = ENTRY_KEY(e), *val = ENTRY_VAL(e), *tb = ENTRY_TAGS(e);
        char ke[MAX_KEY_LEN * 2], ve[MAX_VAL_LEN * 2];
        json_esc(key, ke, sizeof(ke)); json_esc(val, ve, sizeof(ve));
        fprintf(fp, "{\"k\":\""); fwrite(ke, 1, strlen(ke), fp);
        fprintf(fp, "\",\"v\":\""); fwrite(ve, 1, strlen(ve), fp);
        fprintf(fp, "\",\"ts\":%llu,\"tags\":[", (unsigned long long)e->created_at);
        for (int j = 0; j < e->tag_count; j++) {
            if (j > 0) fputc(',', fp);
            char *t = tb + j * MAX_TAG_LEN;
            fputc('"', fp); fwrite(t, 1, strlen(t), fp); fputc('"', fp);
        }
        fprintf(fp, "]}\n"); w++;
    }
    if (fflush(fp) != 0) { fclose(fp); unlink(tmp_fn); return -1; }
    fclose(fp);

    if (rename(tmp_fn, fn) != 0) {
        unlink(tmp_fn);
        return -1;
    }

    for (int i = 0; i < n_evict; i++) {
        MemEntry *e = to_evict[i];
        tag_remove_all(&ctx->tag_index, e);
        hash_remove(&ctx->table, e->key_hash, ENTRY_KEY(e));
        sl_remove(&ctx->sorted, e);
#ifdef HIPOOL_ENABLE_SEMANTIC
        hs_deindex_entry(e->key_hash);
#endif
        e->flags = 0;
        pool_free(&ctx->pool, e);
        if (ctx->pool.ec > 0) ctx->pool.ec--;
    }
    return w;
}

uint64_t day_of_ts(uint64_t ts) {
    struct tm tm; time_t t = (time_t)ts;
    localtime_r(&t, &tm);
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    return (uint64_t)mktime(&tm);
}

int ovf_flush_day(MemoryCtx *ctx, uint64_t day_start) {
    uint64_t day_end = day_start + 86400;

    MemEntry *to_evict[MAX_ENTRIES];
    int n_evict = 0;
    MemEntry **sl_entries = NULL;
    uint32_t sl_count = 0;
    if (sl_range(&ctx->sorted, day_start, day_end, &sl_entries, &sl_count) == 0) {
        for (uint32_t i = 0; i < sl_count && n_evict < MAX_ENTRIES; i++) {
            MemEntry *e = sl_entries[i];
            if (e->flags & FLAG_ACTIVE)
                to_evict[n_evict++] = e;
        }
    }
    free(sl_entries);
    if (n_evict == 0) return 0;

    time_t dt = (time_t)day_start;
    struct tm tm; localtime_r(&dt, &tm);
    char fn[1024], tmp_fn[1024];
    snprintf(fn, sizeof(fn), "%s/memory-%04d-%02d-%02d.json",
        ctx->data_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    snprintf(tmp_fn, sizeof(tmp_fn), "%s/.tmp_memory-%04d-%02d-%02d.json",
        ctx->data_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    int mkr = mkdir(ctx->data_dir, 0755);
    if (mkr != 0 && errno != EEXIST) {
        if (errno == ENOTDIR) { unlink(ctx->data_dir); mkdir(ctx->data_dir, 0755); }
    }

    FILE *fp = fopen(tmp_fn, "w");
    if (!fp) return -1;
    int w = 0;
    for (int i = 0; i < n_evict; i++) {
        MemEntry *e = to_evict[i];
        char *key = ENTRY_KEY(e), *val = ENTRY_VAL(e), *tb = ENTRY_TAGS(e);
        char ke[MAX_KEY_LEN * 2], ve[MAX_VAL_LEN * 2];
        json_esc(key, ke, sizeof(ke)); json_esc(val, ve, sizeof(ve));
        fprintf(fp, "{\"k\":\""); fwrite(ke, 1, strlen(ke), fp);
        fprintf(fp, "\",\"v\":\""); fwrite(ve, 1, strlen(ve), fp);
        fprintf(fp, "\",\"ts\":%llu,\"tags\":[", (unsigned long long)e->created_at);
        for (int j = 0; j < e->tag_count; j++) {
            if (j > 0) fputc(',', fp);
            char *t = tb + j * MAX_TAG_LEN;
            fputc('"', fp); fwrite(t, 1, strlen(t), fp); fputc('"', fp);
        }
        fprintf(fp, "]}\n"); w++;
    }
    fclose(fp);

    if (rename(tmp_fn, fn) != 0) {
        unlink(tmp_fn);
        return -1;
    }

    for (int i = 0; i < n_evict; i++) {
        MemEntry *e = to_evict[i];
        tag_remove_all(&ctx->tag_index, e);
        hash_remove(&ctx->table, e->key_hash, ENTRY_KEY(e));
        sl_remove(&ctx->sorted, e);
#ifdef HIPOOL_ENABLE_SEMANTIC
        hs_deindex_entry(e->key_hash);
#endif
        e->flags = 0;
        pool_free(&ctx->pool, e);
        if (ctx->pool.ec > 0) ctx->pool.ec--;
    }
    return w;
}

int ovf_evict_oldest(MemoryCtx *ctx) {
    int total = 0;
    while (ctx->pool.wl >= EVICT_WATER) {
        uint64_t oldest_day = UINT64_MAX;
        SLNode *first = ctx->sorted.head->next[0];
        if (first && first->entry) {
            oldest_day = day_of_ts(first->entry->created_at);
        }
        if (oldest_day == UINT64_MAX) break;

        int n = ovf_flush_day(ctx, oldest_day);
        if (n <= 0) break;
        total += n;
    }
    return total;
}

char *scan_day_files(MemoryCtx *ctx, const char *key) {
    DIR *d = opendir(ctx->data_dir);
    if (!d) return NULL;

    time_t now = time(NULL);
    struct tm today_tm; localtime_r(&now, &today_tm);
    char today_file[64];
    snprintf(today_file, sizeof(today_file), "memory-%04d-%02d-%02d.json",
             today_tm.tm_year + 1900, today_tm.tm_mon + 1, today_tm.tm_mday);

    char fnames[32][256];
    int nf = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && nf < 32) {
        const char *nm = de->d_name;
        if (strncmp(nm, "memory-", 7) != 0) continue;
        size_t nl = strlen(nm);
        if (nl < 18 || strcmp(nm + nl - 5, ".json") != 0) continue;
        if (strcmp(nm, today_file) == 0) continue;
        int y, m, dd;
        if (sscanf(nm, "memory-%04d-%02d-%02d.json", &y, &m, &dd) == 3) {
            snprintf(fnames[nf], 256, "%s", nm);
            nf++;
        }
    }
    closedir(d);

    for (int i = 0; i < nf - 1; i++)
        for (int j = i + 1; j < nf; j++)
            if (strcmp(fnames[i], fnames[j]) < 0) {
                char tmp[256];
                strcpy(tmp, fnames[i]);
                strcpy(fnames[i], fnames[j]);
                strcpy(fnames[j], tmp);
            }

    for (int fi = 0; fi < nf; fi++) {
        char fp[4096];
        snprintf(fp, sizeof(fp), "%s/%s", ctx->data_dir, fnames[fi]);
        FILE *f = fopen(fp, "r");
        if (!f) continue;

        char line[65536];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '\n' || line[0] == '\0') continue;
            char kb[MAX_KEY_LEN];
            if (json_get_str(line, "k", kb, sizeof(kb)) < 0) continue;
            if (strcmp(kb, key) != 0) continue;

            char vb[MAX_VAL_LEN];
            uint64_t ts = 0;
            json_get_str(line, "v", vb, sizeof(vb));
            json_get_u64(line, "ts", &ts);
            char tags[MAX_TAGS][MAX_TAG_LEN];
            int tc = json_get_tags(line, tags, MAX_TAGS);
            const char *tp[MAX_TAGS];
            for (int ti = 0; ti < tc; ti++) tp[ti] = tags[ti];

            memory_set_with_ts_unlocked(ctx, kb, vb, tp, tc, ts);
            MemEntry *e = hash_lookup(&ctx->table, djb2(kb), kb);
            if (e) e->accessed_at = (uint64_t)time(NULL);
            fclose(f);

            e = hash_lookup(&ctx->table, djb2(kb), kb);
            /* [P0-2] 返回 malloc 拷贝: memory_get 现在的契约是"调用方 free",
             * 不能再返回内存池内部裸指针 ENTRY_VAL(e)。 */
            if (e) return strdup(ENTRY_VAL(e));
            return NULL;
        }
        fclose(f);
    }
    return NULL;
}

int ovf_load(MemoryCtx *ctx) {
    char snap[1024]; snprintf(snap, sizeof(snap), "%s/memory_snapshot.json", ctx->data_dir);
    FILE *sf = fopen(snap, "r");
    if (sf) {
        char line[65536];
        while (fgets(line, sizeof(line), sf)) {
            if (line[0] == '\n' || line[0] == '\0') continue;
            char kb[MAX_KEY_LEN], vb[MAX_VAL_LEN]; uint64_t ts = 0;
            if (json_get_str(line, "k", kb, sizeof(kb)) < 0) continue;
            if (json_get_str(line, "v", vb, sizeof(vb)) < 0) continue;
            json_get_u64(line, "ts", &ts);
            char tags[MAX_TAGS][MAX_TAG_LEN]; int tc = json_get_tags(line, tags, MAX_TAGS);
            const char *tp[MAX_TAGS]; for (int i = 0; i < tc; i++) tp[i] = tags[i];
            memory_set_with_ts_unlocked(ctx, kb, vb, tp, tc, ts);
        }
        fclose(sf);
    }

    time_t now = time(NULL);
    struct tm ttm; localtime_r(&now, &ttm);
    char today[64]; snprintf(today, sizeof(today), "memory-%04d-%02d-%02d.json",
        ttm.tm_year + 1900, ttm.tm_mon + 1, ttm.tm_mday);
    char fp[4096]; snprintf(fp, sizeof(fp), "%s/%s", ctx->data_dir, today);
    FILE *df = fopen(fp, "r");
    if (df) {
        char line[65536];
        while (fgets(line, sizeof(line), df)) {
            if (line[0] == '\n' || line[0] == '\0') continue;
            char kb[MAX_KEY_LEN], vb[MAX_VAL_LEN]; uint64_t ts = 0;
            if (json_get_str(line, "k", kb, sizeof(kb)) < 0) continue;
            if (json_get_str(line, "v", vb, sizeof(vb)) < 0) continue;
            json_get_u64(line, "ts", &ts);
            char tags[MAX_TAGS][MAX_TAG_LEN]; int tc = json_get_tags(line, tags, MAX_TAGS);
            const char *tp[MAX_TAGS]; for (int i = 0; i < tc; i++) tp[i] = tags[i];
            memory_set_with_ts_unlocked(ctx, kb, vb, tp, tc, ts);
        }
        fclose(df);
    }
    return 0;
}

int ovf_clean(const MemoryCtx *ctx) {
    DIR *d = opendir(ctx->data_dir); if (!d) return 0;
    struct dirent *de; time_t now = time(NULL); int r = 0;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name; size_t nl = strlen(nm);
        if (nl < 18 || strncmp(nm, "memory-", 7) != 0) continue;
        int y, m, dd; if (sscanf(nm, "memory-%04d-%02d-%02d.json", &y, &m, &dd) != 3) continue;
        struct tm ft = {0}; ft.tm_year = y - 1900; ft.tm_mon = m - 1; ft.tm_mday = dd;
        if (difftime(now, mktime(&ft)) / 86400.0 > ctx->file_ttl) {
            char fp[4096]; snprintf(fp, sizeof(fp), "%s/%s", ctx->data_dir, nm);
            if (unlink(fp) == 0) r++;
        }
    }
    closedir(d); return r;
}
