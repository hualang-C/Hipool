/*
 * main.c — CLI 入口 + 交互模式 (REPL)
 *
 * 命令行模式: memory <command> [args...] [--shard <name>]
 * 交互模式: memory (无参数)
 */
#include "memory.h"
#ifndef _WIN32
#include <sys/wait.h>
#endif

static void usage(void) {
    printf("usage: memory <command> [args...] [--shard <name>]\n\n"
        "commands:\n"
        "  set   <key> <value> [--tags a,b,c] [--ts <unix_sec>]    存储\n"
        "  get   <key>                            读取\n"
        "  search <query|--tag <tag>|--date YYYY-MM-DD|--range <start_ts> <end_ts>>  搜索\n"
        "  sem-search <query>                   语义检索 (需 -DHIPOOL_ENABLE_SEMANTIC)\n"
        "  del   <key>                            删除\n"
        "  shard list                             列出所有 shard\n"
        "  flush / load / clean / stats / snapshot  管理\n"
        "  --shard <name>  指定操作目标 shard (默认: default)\n");
}

static void print_r(SearchResult *r, int cnt) {
    if (cnt == 0) { printf("(no results)\n"); return; }
    for (int i = 0; i < cnt; i++) {
        char tb[32]; struct tm tm; localtime_r(&r[i].ca, &tm);
        strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M", &tm);
        printf("—— %s ————————————————\n  key: %s\n  tags: [%s]\n  %s\n\n",
               tb, r[i].k, r[i].ts ? r[i].ts : "", r[i].v);
    }
    printf("(%d results)\n", cnt);
}

static const char *extract_shard(int argc, char **argv) {
    for (int i = 2; i < argc - 1; i++)
        if (strcmp(argv[i], "--shard") == 0)
            return argv[i + 1];
    return NULL;
}

#ifdef HIPOOL_ENABLE_SEMANTIC
static void sem_search_print(MemoryCtx *ctx, const char *query) {
    hs_search_result_t hsr[HS_TOP_K];
    int n = hs_search(query, HS_TOP_K, hsr);
    if (n == 0) { printf("(no results)\n"); return; }
    /* 知识图谱重排序 */
    hs_kg_rerank(query, hsr, &n, ctx);
    for (int i = 0; i < n; i++) {
        int found = 0;
        for (int b = 0; b < HASHTABLE_SIZE && !found; b++) {
            HashNode *hn = ctx->table.b[b];
            while (hn && !found) {
                if (hn->e && (hn->e->flags & FLAG_ACTIVE) && hn->e->key_hash == hsr[i].key_hash) {
                    char *kd = ENTRY_KEY(hn->e);
                    char *vd = ENTRY_VAL(hn->e);
                    printf("[%.2f] key: %.*s | val: %.*s\n",
                           hsr[i].distance,
                           (int)hn->e->key_len, kd,
                           (int)hn->e->val_len, vd);
                    found = 1;
                }
                hn = hn->next;
            }
        }
    }
    printf("(%d results)\n", n);
}
#endif

/* 交互模式 */
static int interactive(MemoryCtx *ctx) {
    printf("hipool v6.0 interactive (2MB pool, 'help'/'exit'/'quit')\n");
    char line[8192];
    while (1) {
        printf("hipool> "); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        size_t ll = strlen(line);
        while (ll > 0 && (line[ll - 1] == '\n' || line[ll - 1] == '\r')) line[--ll] = '\0';
        if (ll == 0) continue;

        char *tokens[32]; int nt = 0;
        char *p = line;
        while (*p && nt < 32) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            if (*p == '"') {
                p++; tokens[nt++] = p;
                while (*p && *p != '"') p++;
                if (*p == '"') *p++ = '\0';
            } else {
                tokens[nt++] = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                if (*p) *p++ = '\0';
            }
        }
        if (nt == 0) continue;

        const char *cmd = tokens[0];
        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) break;
        if (strcmp(cmd, "help") == 0) { usage(); continue; }

        if (strcmp(cmd, "set") == 0) {
            if (nt < 3) { printf("usage: set <key> <value> [--tags a,b,c]\n"); continue; }
            const char *tp[16]; int tc = 0;
            for (int i = 3; i < nt; i++) {
                if (strcmp(tokens[i], "--tags") == 0 && i + 1 < nt) {
                    char *ts = tokens[++i], *s = ts;
                    while (*s) {
                        char *ss = s;
                        while (*s && *s != ',') s++;
                        if (*s) *s++ = '\0';
                        if (tc < 16) tp[tc++] = ss;
                    }
                }
            }
            memory_set(ctx, tokens[1], tokens[2], tp, tc);
            printf("ok\n");
        } else if (strcmp(cmd, "get") == 0) {
            if (nt < 2) { printf("usage: get <key>\n"); continue; }
            const char *v = memory_get(ctx, tokens[1]);
            printf("%s\n", v ? v : "(not found)");
            free((void*)v);
        } else if (strcmp(cmd, "del") == 0) {
            if (nt < 2) { printf("usage: del <key>\n"); continue; }
            printf("%s\n", memory_del(ctx, tokens[1]) < 0 ? "(not found)" : "ok");
        } else if (strcmp(cmd, "search") == 0) {
            if (nt < 2) { printf("usage: search <query|--tag <tag>|--date YYYY-MM-DD>\n"); continue; }
            SearchResult *r = NULL; int cnt = 0;
            if (strcmp(tokens[1], "--tag") == 0 && nt > 2)
                memory_search_by_tag(ctx, tokens[2], &r, &cnt);
            else if (strcmp(tokens[1], "--date") == 0 && nt > 2)
                memory_search_date(ctx, tokens[2], &r, &cnt);
            else
                memory_search_text(ctx, tokens[1], &r, &cnt);
            print_r(r, cnt); memory_search_free(r, cnt);
        }
#ifdef HIPOOL_ENABLE_SEMANTIC
        else if (strcmp(cmd, "sem-search") == 0) {
            if (nt < 2) { printf("usage: sem-search <query>\n"); continue; }
            sem_search_print(ctx, tokens[1]);
        }
#endif
        else if (strcmp(cmd, "flush") == 0)
            printf("flushed %d\n", memory_flush_shards(ctx));
        else if (strcmp(cmd, "clean") == 0)
            printf("cleaned %d\n", memory_cleanup_ttl(ctx));
        else if (strcmp(cmd, "stats") == 0) {
            char b[4096]; memory_stats(ctx, b, sizeof(b)); printf("%s\n", b);
        } else {
            printf("unknown: %s (try 'help')\n", cmd);
        }
    }
    return 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    win_argv_to_utf8(&argc, &argv);
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (argc < 2) {
        MemoryCtx ctx;
        if (memory_init(&ctx, OVERFLOW_DIR, FILE_TTL_DAYS) < 0) {
            fprintf(stderr, "init failed\n"); return 1;
        }
        interactive(&ctx);
        memory_destroy(&ctx);
        return 0;
    }

    const char *cmd = argv[1];

    /* load 命令可在不持久化的情况下查看数据 */
    if (strcmp(cmd, "load") == 0) {
        const char *dd = OVERFLOW_DIR; int ttl = FILE_TTL_DAYS;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) dd = argv[++i];
            else if (strcmp(argv[i], "--ttl") == 0 && i + 1 < argc) ttl = atoi(argv[++i]);
        }
        /* [P1-11] 校验 --dir 不含 ".." 成分, 防止路径遍历 */
        if (validate_data_dir(dd) != 0) {
            fprintf(stderr, "invalid --dir (path traversal rejected)\n");
            return 1;
        }
        MemoryCtx ctx; memory_init(&ctx, dd, ttl);
        char buf[4096]; memory_stats(&ctx, buf, sizeof(buf)); printf("%s\n", buf);
        memory_destroy(&ctx); return 0;
    }

    /* 初始化主上下文 */
    MemoryCtx ctx;
    if (memory_init(&ctx, OVERFLOW_DIR, FILE_TTL_DAYS) < 0) {
        fprintf(stderr, "init failed\n"); return 1;
    }

    /* shard 子命令 */
    if (strcmp(cmd, "shard") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: memory shard (list|create <name>)\n");
            memory_destroy(&ctx); return 1;
        }
        if (strcmp(argv[2], "list") == 0) {
            printf("Shards:\n");
            printf("  default\n");
            for (int i = 0; i < ctx.shard_count; i++)
                printf("  %s\n", ctx.shards[i].name);
            memory_destroy(&ctx); return 0;
        }
        if (strcmp(argv[2], "create") == 0 && argc > 3) {
            MemoryCtx *sc = memory_shard_ensure(&ctx, argv[3]);
            printf("%s\n", sc ? "ok" : "failed");
            if (sc) memory_flush(sc);
            memory_destroy(&ctx); return 0;
        }
        fprintf(stderr, "unknown: shard %s\n", argv[2]);
        memory_destroy(&ctx); return 1;
    }

    /* 提取 --shard 参数 */
    const char *shard_name = extract_shard(argc, argv);
    MemoryCtx *target = &ctx;
    if (shard_name) {
        target = memory_shard_ensure(&ctx, shard_name);
        if (!target) { memory_destroy(&ctx); return 1; }
    }

    if (strcmp(cmd, "set") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: memory set <key> <value> [--tags a,b,c] [--ts <unix_sec>] [--shard <name>]\n");
            memory_destroy(&ctx); return 1;
        }
        const char *tp[MAX_TAGS]; int tc = 0;
        uint64_t explicit_ts = 0;
        for (int i = 4; i < argc; i++) {
            if ((strcmp(argv[i], "--tags") == 0 || strcmp(argv[i], "--tag") == 0) && i + 1 < argc) {
                char *ts = argv[++i], *sp;
                char *t = strtok_r(ts, ",", &sp);
                while (t && tc < MAX_TAGS) {
                    while (*t == ' ') t++;
                    size_t l = strlen(t);
                    while (l > 0 && t[l - 1] == ' ') t[--l] = '\0';
                    tp[tc++] = t;
                    t = strtok_r(NULL, ",", &sp);
                }
            } else if (strcmp(argv[i], "--ts") == 0 && i + 1 < argc) {
                explicit_ts = (uint64_t)atoll(argv[++i]);
            }
        }
        int rc = memory_set_with_ts(target, argv[2], argv[3], tp, tc, explicit_ts);
        if (rc == -2) printf("skipped (duplicate)\n");
        else if (rc < 0) { fprintf(stderr, "set failed\n"); memory_destroy(&ctx); return 1; }
        else printf("ok\n");
    } else if (strcmp(cmd, "get") == 0) {
        if (argc < 3) { memory_destroy(&ctx); return 1; }
        const char *v = memory_get(target, argv[2]);
        printf("%s\n", v ? v : "(not found)");
        free((void*)v);
    } else if (strcmp(cmd, "del") == 0) {
        if (argc < 3) { memory_destroy(&ctx); return 1; }
        printf("%s\n", memory_del(target, argv[2]) < 0 ? "(not found)" : "ok");
    } else if (strcmp(cmd, "sem-search") == 0) {
#ifdef HIPOOL_ENABLE_SEMANTIC
        if (argc < 3) { memory_destroy(&ctx); return 1; }
        sem_search_print(target, argv[2]);
#else
        (void)target;
        fprintf(stderr, "sem-search not enabled (recompile with -DHIPOOL_ENABLE_SEMANTIC)\n");
#endif
    } else if (strcmp(cmd, "search") == 0) {
        if (argc < 3) { memory_destroy(&ctx); return 1; }
        SearchResult *r = NULL; int cnt = 0;
        if (strcmp(argv[2], "--tag") == 0 && argc > 3)
            memory_search_by_tag(target, argv[3], &r, &cnt);
        else if (strcmp(argv[2], "--date") == 0 && argc > 3)
            memory_search_date(target, argv[3], &r, &cnt);
        else if (strcmp(argv[2], "--range") == 0 && argc > 4) {
            uint64_t ts_start = (uint64_t)atoll(argv[3]);
            uint64_t ts_end = (uint64_t)atoll(argv[4]);
            memory_search_range(target, ts_start, ts_end, &r, &cnt);
        } else
            memory_search_text(target, argv[2], &r, &cnt);
        print_r(r, cnt); memory_search_free(r, cnt);
    } else if (strcmp(cmd, "flush") == 0) {
        printf("flushed %d\n", memory_flush_shards(&ctx));
    } else if (strcmp(cmd, "snapshot") == 0) {
#ifndef _WIN32
        pid_t child_pid = (pid_t)memory_fork_snapshot(&ctx);
        if (child_pid < 0) {
            fprintf(stderr, "snapshot fork failed\n");
            memory_destroy(&ctx); return 1;
        }
        int status;
        waitpid(child_pid, &status, 0);
        printf("%s\n", (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                       ? "snapshot ok" : "snapshot failed");
#else
        fprintf(stderr, "snapshot not supported on Windows\n");
#endif
    } else if (strcmp(cmd, "clean") == 0) {
        printf("cleaned %d\n", memory_cleanup_ttl(target));
    } else if (strcmp(cmd, "stats") == 0) {
        char b[4096]; memory_stats(&ctx, b, sizeof(b)); printf("%s\n", b);
    } else {
        fprintf(stderr, "unknown: %s\n", cmd);
        usage();
        memory_destroy(&ctx); return 1;
    }

    memory_destroy(&ctx);
    return 0;
}
