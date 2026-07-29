/*
 * unit_test.c — C 层单元测试
 *
 * 目的: 这些 bug 之所以长期未被发现, 根因是只有黑盒 shell 测试、无 C 单元
 * 测试。本文件直接测试内部数据结构/API, 锁定本次修复的回归点:
 *   - hashtable 前缀碰撞 (P0-1)
 *   - memory_get 返回 malloc 拷贝需 free (P0-2)
 *   - WAL 顺序 + 持久化往返 (P0-3)
 *   - pool canary / slab (附带)
 *   - skiplist sl_range 多层查找正确性 (P1-8)
 *   - json 键名注入防护 (P1-10)
 *   - dedup 相似判定 (P1-9 回归)
 *   - validate_name / validate_data_dir (P1-11)
 *
 * 编译: 与 src/*.c (除 main.c, semantic.c) 链接。
 * 退出码: 0 全部通过, 非 0 表示失败用例数。
 */
#include "memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s\n", msg); } \
} while (0)

/* 注意: 用 #name 字符化测试名, 否则 name 会被当作标识符传给 printf */
#define RUN(name) do { printf("[test] %s\n", #name); test_##name(); } while (0)

/* 前置声明 (函数定义在 main 之后) */
static void test_hashtable_prefix_collision(void);
static void test_memory_get_ownership(void);
static void test_wal_persistence_roundtrip(void);
static void test_pool_canary(void);
static void test_skiplist_range(void);
static void test_json_injection(void);
static void test_dedup_similarity(void);
static void test_validate_names(void);

/* ---------- P0-1: hashtable 前缀碰撞 ---------- */
static void test_hashtable_prefix_collision(void) {
    HashTable t; hash_init(&t);
    Pool p; pool_init(&p, 1 << 20);  /* 1MB */

    /* 存 key="abc" */
    size_t sz1 = entry_total_size(3, 3, 0);
    MemEntry *e1 = (MemEntry*)pool_alloc(&p, sz1);
    ASSERT(e1 != NULL, "alloc e1");
    e1->magic_start = CANARY_MAGIC; e1->key_hash = djb2("abc");
    e1->key_len = 3; e1->val_len = 3; e1->tag_count = 0; e1->flags = FLAG_ACTIVE;
    memcpy(ENTRY_KEY(e1), "abc", 4); memcpy(ENTRY_VAL(e1), "v1", 3);
    hash_insert(&t, e1->key_hash, e1);

    /* 旧 bug: 查 "abcdef" (比存储 key 长) 会因 strncmp 只比 3 字节而错误命中。
     * 修复后应返回 NULL。 */
    MemEntry *r = hash_lookup(&t, djb2("abcdef"), "abcdef");
    ASSERT(r == NULL, "P0-1: 查询长 key abcdef 不应命中存储的 abc");

    /* 正向: 查 "abc" 应命中 */
    MemEntry *r2 = hash_lookup(&t, djb2("abc"), "abc");
    ASSERT(r2 == e1, "P0-1: 查 abc 应命中 e1");

    /* hash 表的 HashNode 是 malloc 的, 需手动清理; pool 只 free base */
    for (int b = 0; b < HASHTABLE_SIZE; b++) {
        HashNode *n = t.b[b];
        while (n) { HashNode *nx = n->next; free(n); n = nx; }
    }
    free(p.base);
}

/* ---------- P0-2: memory_get 返回 malloc 拷贝, 需 free ---------- */
static void test_memory_get_ownership(void) {
    system("rm -rf ut_get_data 2>/dev/null");   /* 清理上次残留, 保证测试可复现 */
    MemoryCtx ctx; memory_init(&ctx, "ut_get_data", 7);
    memory_set(&ctx, "k1", "value1", NULL, 0);

    char *v = memory_get(&ctx, "k1");
    ASSERT(v != NULL, "P0-2: get k1 返回非 NULL");
    ASSERT(strcmp(v, "value1") == 0, "P0-2: 值正确");
    /* 关键: 返回的是 malloc 内存, 可以 free 且不影响后续 get (旧实现返回池指针,
     * free 会损坏 slab)。 */
    free(v);

    /* free 后再次 get 仍应正常 (证明返回的是独立拷贝) */
    char *v2 = memory_get(&ctx, "k1");
    ASSERT(v2 != NULL && strcmp(v2, "value1") == 0, "P0-2: free 后再次 get 仍正确");
    free(v2);

    memory_destroy(&ctx);
}

/* ---------- P0-3: WAL 顺序 + 持久化往返 ---------- */
static void test_wal_persistence_roundtrip(void) {
    const char *dir = "ut_wal_data";
    /* 清理可能的残留 */
    char cmd[256]; snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", dir);
    int sysret = system(cmd); (void)sysret;

    /* 写入若干条目 */
    MemoryCtx ctx; memory_init(&ctx, dir, 7);
    memory_set(&ctx, "walkey1", "walval1", NULL, 0);
    memory_set(&ctx, "walkey2", "walval2", NULL, 0);
    memory_del(&ctx, "walkey1");   /* 删除一条, 验证 DEL 也能恢复 */
    memory_destroy(&ctx);

    /* 重新 init: ovf_load + wal_replay 应恢复到删除后的状态。
     * 关键: WAL 顺序修复后, 重启不应丢失 SET 或误恢复已 DEL 的 key。 */
    MemoryCtx ctx2; memory_init(&ctx2, dir, 7);

    char *v1 = memory_get(&ctx2, "walkey1");
    ASSERT(v1 == NULL, "P0-3: 已删除的 walkey1 重启后不应复活");
    free(v1);

    char *v2 = memory_get(&ctx2, "walkey2");
    ASSERT(v2 != NULL && strcmp(v2, "walval2") == 0, "P0-3: walkey2 重启后应存活");
    free(v2);

    memory_destroy(&ctx2);
}

/* ---------- pool canary 检测 ---------- */
static void test_pool_canary(void) {
    Pool p; pool_init(&p, 1 << 20);  /* 1MB, 避免 slab 初始化空间不足 */
    size_t sz = entry_total_size(8, 16, 0);
    MemEntry *e = (MemEntry*)pool_alloc(&p, sz);
    ASSERT(e != NULL, "canary: alloc 成功");
    /* 正常填充 entry 头部 (保证 head canary 不被破坏, 能进入 free 流程) */
    e->magic_start = CANARY_MAGIC; e->key_len = 8; e->val_len = 16;
    e->tag_count = 0; e->flags = FLAG_ACTIVE;
    memcpy(ENTRY_KEY(e), "testkey", 8);
    memcpy(ENTRY_VAL(e), "0123456789abcdef", 17);

    /* 正常 free: canary 完整, 应成功归还 (不打印告警) */
    pool_free(&p, e);
    ASSERT(p.tf == 1, "canary: 完整 canary free 后 tf==1");

    /* 二次 alloc 应复用刚释放的 slab (水位/计数正常) */
    MemEntry *e2 = (MemEntry*)pool_alloc(&p, sz);
    ASSERT(e2 != NULL, "canary: 复用 slab alloc 成功");
    pool_free(&p, e2);
    free(p.base);
}

/* ---------- P1-8: skiplist sl_range 多层查找 ---------- */
static void test_skiplist_range(void) {
    SkipList sl; sl_init(&sl);
    /* 插入 1000 条, 时间戳 0..999, key_hash 递增 */
    for (uint64_t i = 0; i < 1000; i++) {
        /* 构造一个 MemEntry 仅用于 sl_insert (需要 created_at/key_hash) */
        MemEntry *e = (MemEntry*)calloc(1, sizeof(MemEntry) + 32);
        e->magic_start = CANARY_MAGIC; e->key_hash = (uint32_t)i;
        e->created_at = i; e->accessed_at = i; e->flags = FLAG_ACTIVE;
        e->key_len = 1; e->val_len = 1; e->tag_count = 0;
        sl_insert(&sl, e);
    }
    ASSERT(sl.count == 1000, "P1-8: 插入 1000 条 count==1000");

    /* 范围 [100, 200) 应返回 100 条 (时间戳 100..199) */
    MemEntry **res = NULL; uint32_t cnt = 0;
    int rc = sl_range(&sl, 100, 200, &res, &cnt);
    ASSERT(rc == 0, "P1-8: sl_range 返回成功");
    ASSERT(cnt == 100, "P1-8: [100,200) 应返回 100 条");
    /* 验证第一条时间戳是 100 (定位正确, 而非从 0 线性扫) */
    if (cnt > 0) {
        ASSERT(res[0]->created_at == 100, "P1-8: 首条 created_at==100 (定位正确)");
    } else {
        g_fail++; printf("  FAIL: 无结果无法验证首条\n");
    }
    free(res);

    /* 先遍历 level0 收集 entry 指针 (sl_destroy 会释放所有 SLNode,
     * 必须在 destroy 之前拿到 entry 指针, 否则 use-after-free)。 */
    MemEntry *entries[1000]; int ne = 0;
    SLNode *cur = sl.head ? sl.head->next[0] : NULL;
    while (cur && ne < 1000) { entries[ne++] = cur->entry; cur = cur->next[0]; }
    sl_destroy(&sl);   /* 释放所有 SLNode (含 head) */
    for (int i = 0; i < ne; i++) free(entries[i]);
}

/* ---------- P1-10: json 键名注入防护 ---------- */
static void test_json_injection(void) {
    char out[256];

    /* value 内部含字面量 "k":" 的伪匹配。旧实现 json_get_str("k",...) 会
     * 错误匹配到 value 内部。修复后应只在字段名位置匹配。
     * 注意: 我们写出的格式是 {"k":"...","v":"...","ts":N,"tags":[...]}
     * 这里构造一个 value 含 "x":" 的行。 */
    const char *line = "{\"k\":\"realkey\",\"v\":\"has \\\"x\\\":\\\"fake\\\" inside\",\"ts\":42,\"tags\":[]}";
    /* json_get_str 读 v 应得到完整 value, 不被内部的 "x":" 干扰 */
    int rc = json_get_str(line, "v", out, sizeof(out));
    ASSERT(rc == 0, "P1-10: json_get_str(v) 找到字段");
    ASSERT(strstr(out, "fake") != NULL, "P1-10: v 含完整内容含 fake");
    ASSERT(strstr(out, "realkey") == NULL, "P1-10: v 不应错位读到 k 的值");

    /* k 字段 */
    rc = json_get_str(line, "k", out, sizeof(out));
    ASSERT(rc == 0 && strcmp(out, "realkey") == 0, "P1-10: k==realkey");

    /* u64 正常 */
    uint64_t ts = 0;
    rc = json_get_u64(line, "ts", &ts);
    ASSERT(rc == 0 && ts == 42, "P1-10: ts==42");

    /* u64 畸形 (字段值非数字) 应返回 -1 */
    const char *bad = "{\"k\":\"a\",\"ts\":\"notanumber\"}";
    rc = json_get_u64(bad, "ts", &ts);
    ASSERT(rc == -1, "P1-10: 畸形 ts 返回 -1");

    /* 缺失字段 */
    rc = json_get_str(line, "nope", out, sizeof(out));
    ASSERT(rc == -1, "P1-10: 缺失字段返回 -1");
}

/* ---------- P1-9: dedup 相似判定回归 ---------- */
static void test_dedup_similarity(void) {
    system("rm -rf ut_dedup_data 2>/dev/null");   /* 清理残留, 保证可复现 */
    MemoryCtx ctx; memory_init(&ctx, "ut_dedup_data", 7);
    /* dedup 要求 key 有 dash 前缀且 dash_pos >= 10 (见 dedup.c:13)。
     * 用 "conversation-001" (前缀 conversation 长度 12 >= 10)。
     * value 需 >= 20 字符 (dedup.c:16)。 */
    memory_set(&ctx, "conversation-001", "abcdefghijklmnopqrstuvwxyz0123", NULL, 0);
    /* 第二条与第一条内容完全相同 (>80% 字符重叠), 应被判为重复 (-2) */
    int rc = memory_set(&ctx, "conversation-002", "abcdefghijklmnopqrstuvwxyz0123", NULL, 0);
    ASSERT(rc == -2, "P1-9: 高度相似内容应判重 (-2)");

    /* 完全不同的内容应正常写入 */
    rc = memory_set(&ctx, "conversation-003", "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", NULL, 0);
    ASSERT(rc == 0, "P1-9: 不同内容正常写入 (0)");

    memory_destroy(&ctx);
}

/* ---------- P1-11: 路径遍历校验 ---------- */
static void test_validate_names(void) {
    ASSERT(validate_name("myshard") == 0, "P1-11: 合法名 myshard");
    ASSERT(validate_name("shard-1_2") == 0, "P1-11: 合法名 shard-1_2");
    ASSERT(validate_name("") != 0, "P1-11: 空名非法");
    ASSERT(validate_name("../etc") != 0, "P1-11: ../etc 非法");
    ASSERT(validate_name("a/b") != 0, "P1-11: 含分隔符非法");
    ASSERT(validate_name("..") != 0, "P1-11: .. 非法");

    ASSERT(validate_data_dir("data/mydir") == 0, "P1-11: 合法目录 data/mydir");
    ASSERT(validate_data_dir("../evil") != 0, "P1-11: ../evil 目录非法");
    ASSERT(validate_data_dir("a/../b") != 0, "P1-11: 含 .. 段非法");
    ASSERT(validate_data_dir("/var/data") == 0, "P1-11: 绝对路径合法");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* 无缓冲, 崩溃前也能看到输出 */
    RUN(hashtable_prefix_collision);
    RUN(memory_get_ownership);
    RUN(wal_persistence_roundtrip);
    RUN(pool_canary);
    RUN(skiplist_range);
    RUN(json_injection);
    RUN(dedup_similarity);
    RUN(validate_names);

    printf("\n========================================\n");
    printf("  Unit tests: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}
