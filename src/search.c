/*
 * search.c — 搜索功能
 *
 * 提供标签搜索、文本搜索、日期搜索、时间范围搜索。
 * 使用公共的 entry_to_search_result 辅助函数消除代码重复。
 */
#include "memory.h"
#include <dirent.h>

/* 根据活跃的 MemEntry 数组构建 SearchResult 数组 */
SearchResult *entry_to_search_result(MemEntry **entries, uint32_t count, int *out_cnt) {
    if (!entries || count == 0) { *out_cnt = 0; return NULL; }
    SearchResult *sr = (SearchResult*)calloc(count, sizeof(SearchResult));
    if (!sr) { *out_cnt = 0; return NULL; }

    int f = 0;
    for (uint32_t i = 0; i < count; i++) {
        MemEntry *e = entries[i];
        if (!(e->flags & FLAG_ACTIVE)) continue;

        char *kd = ENTRY_KEY(e), *vd = ENTRY_VAL(e), *tb = ENTRY_TAGS(e);
        sr[f].k = win_strndup(kd, e->key_len);
        sr[f].v = win_strndup(vd, e->val_len);
        sr[f].ca = (time_t)e->created_at;
        size_t tl = (size_t)e->tag_count * MAX_TAG_LEN + e->tag_count;
        sr[f].ts = (char*)malloc(tl ? tl : 1);
        if (sr[f].ts) {
            sr[f].ts[0] = '\0';
            for (int t = 0; t < e->tag_count; t++) {
                if (t > 0) strcat(sr[f].ts, ",");
                strcat(sr[f].ts, tb + t * MAX_TAG_LEN);
            }
        }
        f++;
    }
    *out_cnt = f;
    return sr;
}

int memory_search_by_tag(MemoryCtx *ctx, const char *tag, SearchResult **r, int *cnt) {
    *r = NULL; *cnt = 0;
    LOCK(ctx);
    uint32_t ec = 0;
    MemEntry **es = tag_search(&ctx->tag_index, tag, &ec);
    if (!es || ec == 0) { free(es); UNLOCK(ctx); return 0; }

    *r = entry_to_search_result(es, ec, cnt);
    free(es);
    UNLOCK(ctx);
    return 0;
}

int memory_search_text(MemoryCtx *ctx, const char *q, SearchResult **r, int *cnt) {
    *r = NULL; *cnt = 0;
    if (!q || !q[0]) return 0;

    /* [P1-7] 原实现在持锁状态下 fopen/fgets 扫描全部磁盘日文件, 期间整个
     * ctx 被锁死, 阻塞所有其他操作。改为: 锁内物化内存匹配结果(复制 key/
     * val/tags, 不能持有 MemEntry* 指针因为解锁后可能被另一线程 pool_free),
     * 并记录已存在的 key 列表供磁盘去重, 然后解锁再做磁盘 I/O。 */
    LOCK(ctx);

    MemEntry *mem_matches[MAX_ENTRIES];
    int n_mem = 0;
    for (int b = 0; b < HASHTABLE_SIZE; b++) {
        HashNode *n = ctx->table.b[b];
        while (n && n_mem < MAX_ENTRIES) {
            MemEntry *e = n->e;
            if (e->flags & FLAG_ACTIVE) {
                char *kd = ENTRY_KEY(e), *vd = ENTRY_VAL(e);
                if (strstr(kd, q) || strstr(vd, q))
                    mem_matches[n_mem++] = e;
            }
            n = n->next;
        }
    }

    /* 物化内存结果 (复用 entry_to_search_result, 消除重复代码)。注意此时
     * 仍持锁, mem_matches 指向的 entry 不会被释放。 */
    int mem_cnt = 0;
    SearchResult *mem_sr = entry_to_search_result(mem_matches, (uint32_t)n_mem, &mem_cnt);

    /* 复制已存在 key 列表供磁盘结果去重 (解锁后不能再访问 hash 表)。 */
    char (*seen_keys)[MAX_KEY_LEN] = NULL;
    if (mem_cnt > 0) {
        seen_keys = (char(*)[MAX_KEY_LEN])malloc((size_t)mem_cnt * MAX_KEY_LEN);
        if (!seen_keys) { memory_search_free(mem_sr, mem_cnt); UNLOCK(ctx); return -1; }
        for (int i = 0; i < mem_cnt; i++)
            if (mem_sr[i].k) {
                strncpy(seen_keys[i], mem_sr[i].k, MAX_KEY_LEN - 1);
                seen_keys[i][MAX_KEY_LEN - 1] = '\0';
            } else
                seen_keys[i][0] = '\0';
    }

    time_t now = time(NULL);
    struct tm ttm; localtime_r(&now, &ttm);
    char today_fn[64];
    snprintf(today_fn, sizeof(today_fn), "memory-%04d-%02d-%02d.json",
             ttm.tm_year + 1900, ttm.tm_mon + 1, ttm.tm_mday);
    char data_dir_cp[512];
    strncpy(data_dir_cp, ctx->data_dir, sizeof(data_dir_cp) - 1);
    data_dir_cp[sizeof(data_dir_cp) - 1] = '\0';

    UNLOCK(ctx);
    /* —— 锁外: 磁盘 I/O —— */

    DIR *d = opendir(data_dir_cp);
    if (!d) { *r = mem_sr; *cnt = mem_cnt; free(seen_keys); return 0; }

    int cap = mem_cnt + 256;
    SearchResult *sr = (SearchResult*)calloc((size_t)cap, sizeof(SearchResult));
    if (!sr) { closedir(d); memory_search_free(mem_sr, mem_cnt); free(seen_keys); return -1; }
    int f = 0;

    /* 先放内存结果 */
    for (int i = 0; i < mem_cnt && f < cap; i++) sr[f++] = mem_sr[i];
    free(mem_sr);   /* 已转移所有权到 sr, 只释放外层数组 */

    /* 再扫磁盘文件 */
    struct dirent *de;
    while ((de = readdir(d)) != NULL && f < cap) {
        const char *nm = de->d_name;
        size_t nl = strlen(nm);
        if (nl < 18 || strncmp(nm, "memory-", 7) != 0 || strcmp(nm + nl - 5, ".json") != 0) continue;
        if (strcmp(nm, today_fn) == 0) continue;
        char fpath[4096]; snprintf(fpath, sizeof(fpath), "%s/%s", data_dir_cp, nm);
        FILE *ff = fopen(fpath, "r"); if (!ff) continue;
        char line[65536];
        while (fgets(line, sizeof(line), ff) && f < cap) {
            if (line[0] == '\n') continue;
            if (!strstr(line, q)) continue;
            char kb[MAX_KEY_LEN], vb[MAX_VAL_LEN]; uint64_t ts = 0;
            if (json_get_str(line, "k", kb, sizeof(kb)) < 0) continue;
            /* 用本地 seen_keys 去重, 替代旧的 hash_lookup (解锁后表不可访问) */
            int dup = 0;
            for (int i = 0; i < mem_cnt; i++)
                if (strcmp(seen_keys[i], kb) == 0) { dup = 1; break; }
            if (dup) continue;
            if (json_get_str(line, "v", vb, sizeof(vb)) < 0) continue;
            json_get_u64(line, "ts", &ts);
            sr[f].k = win_strndup(kb, strlen(kb));
            sr[f].v = win_strndup(vb, strlen(vb));
            sr[f].ca = (time_t)ts;
            char tag_str[512] = "";
            char tags[MAX_TAGS][MAX_TAG_LEN]; int tc = json_get_tags(line, tags, MAX_TAGS);
            for (int ti = 0; ti < tc; ti++) {
                if (ti > 0) strcat(tag_str, ",");
                strcat(tag_str, tags[ti]);
            }
            sr[f].ts = strdup(tag_str);
            f++;
        }
        fclose(ff);
    }
    closedir(d);
    free(seen_keys);
    *r = sr; *cnt = f;
    return 0;
}

int memory_search_date(MemoryCtx *ctx, const char *ds, SearchResult **r, int *cnt) {
    *r = NULL; *cnt = 0;
    LOCK(ctx);
    int y, m, d;
    if (sscanf(ds, "%d-%d-%d", &y, &m, &d) != 3) { UNLOCK(ctx); return -1; }
    struct tm tt = {0}; tt.tm_year = y - 1900; tt.tm_mon = m - 1; tt.tm_mday = d;
    time_t ds0 = mktime(&tt), de = ds0 + 86400;

    /* 优先: Skip List 范围查询 */
    MemEntry **entries = NULL;
    uint32_t ec = 0;
    if (sl_range(&ctx->sorted, (uint64_t)ds0, (uint64_t)de, &entries, &ec) >= 0 && ec > 0) {
        *r = entry_to_search_result(entries, ec, cnt);
        free(entries);
        UNLOCK(ctx);
        return 0;
    }
    free(entries);

    /* 回退: 磁盘日文件扫描 */
    SearchResult *sr = (SearchResult*)calloc(256, sizeof(SearchResult));
    if (!sr) { UNLOCK(ctx); return -1; }
    int f = 0;
    char dfn[64]; snprintf(dfn, sizeof(dfn), "memory-%04d-%02d-%02d.json", y, m, d);
    char fpath[4096]; snprintf(fpath, sizeof(fpath), "%s/%s", ctx->data_dir, dfn);
    FILE *ff = fopen(fpath, "r");
    if (ff) {
        char line[65536];
        while (fgets(line, sizeof(line), ff) && f < 256) {
            if (line[0] == '\n') continue;
            char kb[MAX_KEY_LEN], vb[MAX_VAL_LEN]; uint64_t ts = 0;
            if (json_get_str(line, "k", kb, sizeof(kb)) < 0) continue;
            if (json_get_str(line, "v", vb, sizeof(vb)) < 0) continue;
            json_get_u64(line, "ts", &ts);
            sr[f].k = win_strndup(kb, strlen(kb));
            sr[f].v = win_strndup(vb, strlen(vb));
            sr[f].ca = (time_t)ts;
            char tag_str[512] = "";
            char tags[MAX_TAGS][MAX_TAG_LEN]; int tc = json_get_tags(line, tags, MAX_TAGS);
            for (int ti = 0; ti < tc; ti++) {
                if (ti > 0) strcat(tag_str, ",");
                strcat(tag_str, tags[ti]);
            }
            sr[f].ts = strdup(tag_str);
            f++;
        }
        fclose(ff);
    }
    *r = sr; *cnt = f;
    UNLOCK(ctx);
    return 0;
}

int memory_search_range(MemoryCtx *ctx, uint64_t t_start, uint64_t t_end,
                        SearchResult **r, int *cnt) {
    *r = NULL; *cnt = 0;
    LOCK(ctx);
    MemEntry **entries = NULL;
    uint32_t ec = 0;
    if (sl_range(&ctx->sorted, t_start, t_end, &entries, &ec) < 0) { UNLOCK(ctx); return -1; }
    if (ec == 0) { free(entries); UNLOCK(ctx); return 0; }

    *r = entry_to_search_result(entries, ec, cnt);
    free(entries);
    UNLOCK(ctx);
    return 0;
}

void memory_search_free(SearchResult *r, int cnt) {
    if (!r) return;
    for (int i = 0; i < cnt; i++) { free(r[i].k); free(r[i].v); free(r[i].ts); }
    free(r);
}
