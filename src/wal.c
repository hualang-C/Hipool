/*
 * wal.c — 预写日志 (Write-Ahead Log)
 *
 * 二进制日志格式:
 *   magic(4) + op(1) + key_len(2) + val_len(2) + tag_count(1) + ts(8)
 *   + key(N) + val(N) + tag[0..N](每个 MAX_TAG_LEN 字节)
 */
#include "memory.h"

int wal_path(char *buf, size_t bs, const char *dd) {
    return snprintf(buf, bs, "%s/wal.log", dd);
}

int wal_append(MemoryCtx *ctx, uint8_t op,
               const char *key, const char *val,
               const char **tags, int tc, uint64_t ts) {
    if (ctx->wal_disabled || !ctx->wal_fp) return 0;
    size_t kl = strlen(key), vl = val ? strlen(val) : 0;
    if (kl > MAX_KEY_LEN) kl = MAX_KEY_LEN;
    if (vl > MAX_VAL_LEN) vl = MAX_VAL_LEN;
    if (tc > MAX_TAGS) tc = MAX_TAGS;

    uint32_t magic = WAL_MAGIC;
    uint16_t nkl = (uint16_t)kl, nvl = (uint16_t)vl;
    uint8_t ntc = (uint8_t)tc;

    fwrite(&magic, 4, 1, ctx->wal_fp);
    fwrite(&op, 1, 1, ctx->wal_fp);
    fwrite(&nkl, 2, 1, ctx->wal_fp);
    fwrite(&nvl, 2, 1, ctx->wal_fp);
    fwrite(&ntc, 1, 1, ctx->wal_fp);
    fwrite(&ts, 8, 1, ctx->wal_fp);
    fwrite(key, 1, kl, ctx->wal_fp);
    if (val) fwrite(val, 1, vl, ctx->wal_fp);
    for (int i = 0; i < tc; i++) {
        if (tags[i]) {
            size_t tl = strlen(tags[i]);
            if (tl > MAX_TAG_LEN - 1) tl = MAX_TAG_LEN - 1;
            fwrite(tags[i], 1, tl, ctx->wal_fp);
            if (tl < MAX_TAG_LEN) {
                char pad = 0;
                for (size_t p = tl; p < MAX_TAG_LEN; p++) fwrite(&pad, 1, 1, ctx->wal_fp);
            }
        } else {
            char pad[MAX_TAG_LEN] = {0};
            fwrite(pad, 1, MAX_TAG_LEN, ctx->wal_fp);
        }
    }
    return 0;
}

int wal_replay(MemoryCtx *ctx, const char *wal_file) {
    FILE *fp = fopen(wal_file, "rb");
    if (!fp) return 0;

    fseeko(fp, 0, SEEK_END);
    long sz = ftello(fp);
    if (sz <= 0) { fclose(fp); return 0; }
    fseeko(fp, 0, SEEK_SET);

    char *buf = (char*)malloc((size_t)sz);
    if (!buf) { fclose(fp); return 0; }
    size_t nread = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (nread == 0) { free(buf); return 0; }

    int restored = 0;
    size_t pos = 0;
    while (pos + 18 <= nread) {
        uint32_t magic = *(uint32_t*)(buf + pos); pos += 4;
        if (magic != WAL_MAGIC) continue;

        uint8_t  op   = *(uint8_t*)(buf + pos);  pos += 1;
        uint16_t nkl  = *(uint16_t*)(buf + pos); pos += 2;
        uint16_t nvl  = *(uint16_t*)(buf + pos); pos += 2;
        uint8_t  ntc  = *(uint8_t*)(buf + pos);  pos += 1;
        uint64_t ts   = *(uint64_t*)(buf + pos); pos += 8;

        if (nkl > MAX_KEY_LEN || pos + nkl > nread) break;

        char key_buf[MAX_KEY_LEN + 1];
        memcpy(key_buf, buf + pos, nkl); pos += nkl;
        key_buf[nkl] = '\0';

        char val_buf[MAX_VAL_LEN + 1]; val_buf[0] = '\0';
        if (op == WAL_OP_SET) {
            if (nvl > MAX_VAL_LEN || pos + nvl > nread) break;
            memcpy(val_buf, buf + pos, nvl); pos += nvl;
            val_buf[nvl] = '\0';
        }

        int tci = ntc > MAX_TAGS ? MAX_TAGS : ntc;
        const char *tag_ptrs[MAX_TAGS];
        char tag_buf[MAX_TAGS * MAX_TAG_LEN];
        for (int i = 0; i < tci; i++) {
            if (pos + MAX_TAG_LEN > nread) { tci = i; goto replay_done; }
            memcpy(tag_buf + i * MAX_TAG_LEN, buf + pos, MAX_TAG_LEN); pos += MAX_TAG_LEN;
            if (tag_buf[i * MAX_TAG_LEN] == '\0') {
                tag_ptrs[i] = NULL;
            } else {
                tag_buf[i * MAX_TAG_LEN + MAX_TAG_LEN - 1] = '\0';
                tag_ptrs[i] = tag_buf + i * MAX_TAG_LEN;
            }
        }

        if (op == WAL_OP_SET) {
            if (memory_set_with_ts_unlocked(ctx, key_buf, val_buf, tag_ptrs, tci, ts) >= 0)
                restored++;
        } else if (op == WAL_OP_DEL) {
            if (memory_del_unlocked(ctx, key_buf) == 0)
                restored++;
        }
    }

replay_done:
    free(buf);
    return restored;
}

int wal_compact(MemoryCtx *ctx) {
    if (!ctx->wal_fp) return 0;
    fclose(ctx->wal_fp);
    ctx->wal_fp = NULL;
    char wf[1024]; wal_path(wf, sizeof(wf), ctx->data_dir);
    ctx->wal_fp = fopen(wf, "wb");
    if (!ctx->wal_fp) return -1;
    return 0;
}
