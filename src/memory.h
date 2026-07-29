/*
 * memory.h — hipool 公共 API 头文件
 *
 * 嵌入式 AI Agent 记忆系统
 * 海马体-皮层双存储模型: 热内存池 (2MB) + 日文件溢出 (7天 TTL)
 * 零外部依赖 (libc only)
 */
#ifndef HIPOOL_MEMORY_H
#define HIPOOL_MEMORY_H

#define _POSIX_C_SOURCE 200809L

/* MinGW: 启用 C99 printf 格式 (支持 %zu) */
#ifdef __MINGW32__
#define __USE_MINGW_ANSI_STDIO 1
#endif

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 语义检索 (条件编译) */
#include "semantic.h"
/* 知识图谱 */
#include "knowledge_graph.h"

/* ===================== 编译时配置 ===================== */
#define POOL_SIZE           (2 * 1024 * 1024)
#define MAX_KEY_LEN         256
#define MAX_VAL_LEN         16320
#define MAX_TAGS            16
#define MAX_TAG_LEN         64
#define MAX_ENTRIES         4096
#define HASHTABLE_SIZE      4096
#define TAG_HASHTABLE_SIZE  512
#define EVICT_WATER         80
#define OVERFLOW_DIR        "memory_data"
#define FILE_TTL_DAYS       7
#define CANARY_MAGIC        0xDEADBEEF
#define SL_MAX_LEVEL        16
#define SL_PROB             4           /* 1/4 = 0.25 概率提升层级 */
#define WAL_MAGIC           0x57414C31UL
#define WAL_OP_SET          0x01
#define WAL_OP_DEL          0x02
#define MAX_SHARDS          8
#define SHARD_POOL_SIZE     (256 * 1024)

typedef enum { SLAB_256=0, SLAB_1K=1, SLAB_4K=2, SLAB_16K=3, SLAB_LEVELS=4 } SlabLevel;
#define SLAB_SIZES { 256, 1024, 4096, 16384 }

#define FLAG_ACTIVE     0x01
#define FLAG_OVERFLOWED 0x02

/* ===================== 实体分类常量 ===================== */
/* LLM 可在此自建新分类。扩展规则:
 *   1. 在末尾追加 #define ENTITY_XXX N
 *   2. 确保 N 不重复
 *   3. 同步更新 semantic.c 的 Z 轴向量数组 (entity_freq[ENTITY_MAX+1])
 *   4. 同步更新 dict_distill.c 的 rules[] 表
 */
#define ENTITY_FRUIT      1   // Fruit
#define ENTITY_SEAFOOD    2   // Seafood
#define ENTITY_BIRD       3   // Bird
#define ENTITY_ANIMAL     4   // Animal
#define ENTITY_VEGETABLE  5   // Vegetable
#define ENTITY_FOOD       6   // Food/Beverage
#define ENTITY_FURNITURE  7   // Furniture
#define ENTITY_APPLIANCE  8   // Appliance
#define ENTITY_CLOTHING   9   // Clothing
#define ENTITY_VERB      10   // Verb
#define ENTITY_NUMERAL   11   // Numeral
#define ENTITY_VEHICLE   12   // Vehicle
#define ENTITY_BUILDING  13   // Building
#define ENTITY_BODY      14   // Body part
#define ENTITY_COLOR     15   // Color
#define ENTITY_NATURE    16   // Nature/Weather
#define ENTITY_ADJECTIVE 17   // Adjective
#define ENTITY_BRAND     18   // Brand/Company
#define ENTITY_TECH      19   // Technology/Framework
/* LLM can append new categories below */
#define ENTITY_LITERATURE 20    // Literary works
#define ENTITY_PERSON    21    // Notable persons
#define ENTITY_TOY       22    // Toys
#define ENTITY_APPLIANCE_HOME 23 // Home appliances
#define ENTITY_NETWORK_DEVICE 24 // Network devices
/* [P1-14] 实体分类上限: 新增分类时只需追加 ENTITY_XXX 并更新此值,
 * semantic.c 的 entity_freq[ENTITY_MAX+1] 等边界自动随之扩展,
 * 避免旧实现 [32]/<=24/<=17 等魔数失同步导致越界或漏统计。 */
#define ENTITY_MAX ENTITY_NETWORK_DEVICE

/* ===================== 线程安全 ===================== */
#ifdef HIPOOL_USE_MUTEX
#include <pthread.h>
#define LOCK(c)   pthread_mutex_lock(&(c)->lock)
#define UNLOCK(c) pthread_mutex_unlock(&(c)->lock)
#else
#define LOCK(c)   ((void)0)
#define UNLOCK(c) ((void)0)
#endif

/* ===================== 日志分级 ===================== */
/* [P1-13] 原实现全部裸 fprintf(stderr, ...), 无级别/无开关, 且 pool.c 输出
 * 指针地址 %p (信息泄露)。现提供分级宏: ERROR/WARN 默认输出, INFO/DEBUG
 * 默认静默, 可通过 -DHIPOOL_LOG_DEBUG / -DHIPOOL_LOG_INFO 开启。
 * 指针地址仅 DEBUG 级输出, 生产构建不泄露。 */
#include <stdio.h>
#define HIPOOL_LOG_ERROR(...) fprintf(stderr, "[ERROR] " __VA_ARGS__)
#define HIPOOL_LOG_WARN(...)  fprintf(stderr, "[WARN]  " __VA_ARGS__)
#ifdef HIPOOL_LOG_INFO
#define HIPOOL_LOG_INFO(...)  fprintf(stderr, "[INFO]  " __VA_ARGS__)
#else
#define HIPOOL_LOG_INFO(...)  ((void)0)
#endif
#ifdef HIPOOL_LOG_DEBUG
#define HIPOOL_LOG_DEBUG(...) fprintf(stderr, "[DEBUG] " __VA_ARGS__)
#else
#define HIPOOL_LOG_DEBUG(...) ((void)0)
#endif

/* ===================== Windows 兼容 ===================== */
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define mkdir(d,p) _mkdir(d)
#define localtime_r(tp, tm) localtime_s(tm, tp)
static inline char *win_strndup(const char *s, size_t n) {
    size_t sl = strlen(s);
    if (n > sl) n = sl;
    char *p = (char*)malloc(n + 1);
    if (p) { memcpy(p, s, n); p[n] = '\0'; }
    return p;
}
void win_argv_to_utf8(int *argc, char ***argv);
#else
#define win_strndup(s,n) strndup(s,n)
#endif

/* 跨平台 packed 结构体 */
#ifdef _MSC_VER
#define PACKED_STRUCT(name) __pragma(pack(push, 1)) struct name __pragma(pack(pop))
#define PACKED_STRUCT_DEF(name, body) __pragma(pack(push, 1)) struct name body __pragma(pack(pop))
#else
#define PACKED_STRUCT(name) struct __attribute__((packed)) name
#endif

/* ===================== 核心数据结构 ===================== */
typedef PACKED_STRUCT(MemEntry) {
    uint32_t magic_start;
    uint32_t key_hash;
    uint16_t key_len;
    uint16_t val_len;
    uint8_t  tag_count;
    uint8_t  flags;
    uint8_t  _pad[2];
    uint64_t created_at;
    uint64_t accessed_at;
} MemEntry;

#define ENTRY_HEADER_SIZE  (sizeof(MemEntry) - sizeof(uint32_t))
#define ENTRY_DATA_OFFSET  sizeof(MemEntry)

/* 快速访问 entry 内部字段的宏 */
#define ENTRY_KEY(e)     ((char*)(e) + sizeof(MemEntry))
#define ENTRY_VAL(e)     (ENTRY_KEY(e) + (e)->key_len + 1)
#define ENTRY_TAGS(e)    (ENTRY_VAL(e) + (e)->val_len + 1)

static inline size_t entry_total_size(uint16_t kl, uint16_t vl, uint8_t tc) {
    return sizeof(MemEntry) + (size_t)(kl+1) + (size_t)(vl+1) + (size_t)tc * MAX_TAG_LEN;
}

typedef struct HashNode { uint32_t kh; MemEntry *e; struct HashNode *next; } HashNode;
typedef struct TagEntry { char tag[MAX_TAG_LEN]; MemEntry **es; uint32_t n, cap; struct TagEntry *next; } TagEntry;

typedef struct {
    uint8_t *base; size_t total, used; uint32_t ec;
    uint32_t so[4], ss[4], sc[4]; uint8_t *bm[4];
    uint64_t ta, tf; uint32_t wl;
} Pool;

typedef struct { HashNode *b[HASHTABLE_SIZE]; uint32_t cnt; } HashTable;
typedef struct { TagEntry *b[TAG_HASHTABLE_SIZE]; uint32_t cnt; } TagIndex;

typedef struct SLNode {
    MemEntry *entry;
    uint64_t sort_key;
    uint8_t level;
    struct SLNode *next[];
} SLNode;

typedef struct {
    SLNode *head;
    int top_level;
    uint32_t count;
} SkipList;

typedef struct MemoryCtx MemoryCtx;

typedef struct { char name[64]; MemoryCtx *ctx; } ShardEntry;

struct MemoryCtx {
    Pool pool; HashTable table; TagIndex tag_index; SkipList sorted;
    char data_dir[512]; int file_ttl, initialized, wal_disabled;
    FILE *wal_fp;
    ShardEntry shards[MAX_SHARDS]; int shard_count;

    /* 知识图谱 */
    KnowledgeGraph *kg;

#ifdef HIPOOL_USE_MUTEX
    pthread_mutex_t lock;
#endif
};

typedef struct { char *k, *v, *ts; time_t ca; } SearchResult;

/* ===================== 工具函数声明 ===================== */
static inline size_t bm_bytes(uint32_t n) { return (n+7)/8; }
static inline int    bm_test(const uint8_t *bm, uint32_t i) { return (bm[i/8]>>(i%8))&1; }
static inline void   bm_set(uint8_t *bm, uint32_t i) { bm[i/8] |= (1<<(i%8)); }
static inline void   bm_clear(uint8_t *bm, uint32_t i) { bm[i/8] &= ~(1<<(i%8)); }

static inline uint32_t djb2(const char *s) {
    uint32_t h = 5381; int c;
    while ((c = (unsigned char)*s++)) h = ((h<<5)+h) + (uint32_t)c;
    return h;
}
static inline uint32_t djb2_n(const char *s, size_t n) {
    uint32_t h = 5381;
    for (size_t i=0; i<n && s[i]; i++) h = ((h<<5)+h) + (uint32_t)(unsigned char)s[i];
    return h;
}
static inline uint64_t make_sort_key(uint64_t ts, uint32_t kh) {
    return (ts << 32) | (uint64_t)kh;
}

/* ===================== 内存池 API ===================== */
int     pool_init(Pool *p, size_t sz);
void   *pool_alloc(Pool *p, size_t sz);
void    pool_free(Pool *p, void *ptr);

/* ===================== 哈希表 API ===================== */
int      hash_init(HashTable *t);
int      hash_insert(HashTable *t, uint32_t kh, MemEntry *e);
MemEntry *hash_lookup(HashTable *t, uint32_t kh, const char *key);
int      hash_remove(HashTable *t, uint32_t kh, const char *key);

/* ===================== 标签索引 API ===================== */
int       tag_init(TagIndex *ti);
TagEntry *tag_ensure(TagIndex *ti, const char *tag);
int       tag_add(TagIndex *ti, const char *tag, MemEntry *e);
MemEntry **tag_search(TagIndex *ti, const char *tag, uint32_t *cnt);
void      tag_remove_all(TagIndex *ti, MemEntry *e);

/* ===================== SkipList API ===================== */
int  sl_init(SkipList *sl);
int  sl_insert(SkipList *sl, MemEntry *entry);
void sl_remove(SkipList *sl, MemEntry *entry);
int  sl_range(SkipList *sl, uint64_t t_start, uint64_t t_end, MemEntry ***out, uint32_t *cnt);
void sl_destroy(SkipList *sl);

/* ===================== WAL API ===================== */
int  wal_path(char *buf, size_t bs, const char *dd);
int  wal_append(MemoryCtx *ctx, uint8_t op, const char *key, const char *val,
                const char **tags, int tc, uint64_t ts);
int  wal_replay(MemoryCtx *ctx, const char *wal_file);
int  wal_compact(MemoryCtx *ctx);

/* ===================== Overflow API ===================== */
int  ovf_flush(MemoryCtx *ctx);
int  ovf_flush_day(MemoryCtx *ctx, uint64_t day_start);
int  ovf_evict_oldest(MemoryCtx *ctx);
int  ovf_load(MemoryCtx *ctx);
int  ovf_clean(const MemoryCtx *ctx);
/* 返回 malloc 分配的字符串副本 (命中磁盘日文件时), 调用方负责 free;
 * 未命中返回 NULL。 */
char *scan_day_files(MemoryCtx *ctx, const char *key);
uint64_t day_of_ts(uint64_t ts);

/* ===================== JSON 序列化 API ===================== */
void  json_esc(const char *s, char *d, size_t dm);
int   json_get_str(const char *s, const char *key, char *out, size_t om);
int   json_get_u64(const char *s, const char *key, uint64_t *v);
int   json_get_tags(const char *s, char tags[][MAX_TAG_LEN], int max);

/* ===================== 去重 API ===================== */
int dedup_same_day(MemoryCtx *ctx, const char *key, const char *val);

/* ===================== 公开 API ===================== */
int         memory_init(MemoryCtx *ctx, const char *dd, int ttl);
void        memory_destroy(MemoryCtx *ctx);
int         memory_set(MemoryCtx *ctx, const char *key, const char *val,
                       const char **tags, int tc);
int         memory_set_with_ts(MemoryCtx *ctx, const char *key, const char *val,
                               const char **tags, int tc, uint64_t ts);
/* 内部不加锁实现: 供 wal_replay / ovf_load 等已在锁外/单线程启动阶段
 * 的路径调用。外部代码应使用 memory_set_with_ts(加锁版本)。 */
int         memory_set_with_ts_unlocked(MemoryCtx *ctx, const char *key,
                                        const char *val, const char **tags,
                                        int tc, uint64_t ts);
/* 返回 malloc 分配的字符串副本, 调用方负责 free; 失败返回 NULL。
 * 旧实现返回内存池内部裸指针并提前解锁, 多线程下有 use-after-free,
 * 且两条返回路径(内存命中/磁盘命中)语义不一致(是否需 free), 已修正。 */
char       *memory_get(MemoryCtx *ctx, const char *key);
int         memory_del(MemoryCtx *ctx, const char *key);

int  memory_search_by_tag(MemoryCtx *ctx, const char *tag,
                          SearchResult **r, int *cnt);
int  memory_search_text(MemoryCtx *ctx, const char *q,
                        SearchResult **r, int *cnt);
int  memory_search_date(MemoryCtx *ctx, const char *ds,
                        SearchResult **r, int *cnt);
int  memory_search_range(MemoryCtx *ctx, uint64_t t_start, uint64_t t_end,
                         SearchResult **r, int *cnt);
void memory_search_free(SearchResult *r, int cnt);

int  memory_flush(MemoryCtx *ctx);
int  memory_load(MemoryCtx *ctx);
int  memory_cleanup_ttl(MemoryCtx *ctx);
void memory_stats(const MemoryCtx *ctx, char *buf, size_t bl);

int  memory_flush_shards(MemoryCtx *ctx);
int  memory_fork_snapshot(MemoryCtx *ctx);
MemoryCtx *memory_shard_get(MemoryCtx *ctx, const char *name);
MemoryCtx *memory_shard_ensure(MemoryCtx *ctx, const char *name);

/* [P1-11] 路径遍历防护: 校验标识符 / 数据目录。
 * validate_name: 仅 [A-Za-z0-9_-], 非空, <=63 字符 (用于 shard 名等)。
 * validate_data_dir: 拒绝含 ".." 路径成分的目录 (用于 --dir)。返回 0 合法。 */
int validate_name(const char *name);
int validate_data_dir(const char *dir);

/* 内部用于 memory_del_unlocked 的声明 (search.c/dedup.c 等需要) */
int memory_del_unlocked(MemoryCtx *ctx, const char *key);
/* 仅从内存移除条目不写 WAL, 供 set 覆盖旧值使用 (内部, 不加锁) */
int mem_remove_entry(MemoryCtx *ctx, const char *key);

/* 搜索结果构建辅助函数 (search.c 内部使用, 但也在其他模块中用到) */
SearchResult *entry_to_search_result(MemEntry **entries, uint32_t count, int *out_cnt);

#ifdef __cplusplus
}
#endif

#endif /* HIPOOL_MEMORY_H */
