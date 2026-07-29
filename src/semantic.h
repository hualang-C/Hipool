/*
 * semantic.h — 语义检索模块头文件
 *
 * 中文语义检索: FMM 分词 + 5D 向量编码 + 向量相似度搜索
 * 编译时需 -DHIPOOL_ENABLE_SEMANTIC 和 -lm
 */
#ifndef HIPOOL_SEMANTIC_H
#define HIPOOL_SEMANTIC_H

#include <stdint.h>

/* 前置声明 (避免循环 include) */
struct MemoryCtx;

#ifdef HIPOOL_ENABLE_SEMANTIC

#define HS_MAX_TOKENS      256
#define HS_TOP_K           8
#define HS_DIST_THRESHOLD  2.0f
#define HS_MAX_DOCS        4096

typedef struct {
    uint32_t key_hash;
    float    distance;
} hs_search_result_t;

/* 索引一条记录 */
int hs_index_entry(uint32_t key_hash, const char *text);

/* 删除索引 */
int hs_deindex_entry(uint32_t key_hash);

/* 语义检索: 返回 TopK 结果 (7D 向量) */
int hs_search(const char *query, int top_k, hs_search_result_t results[]);

/* 知识图谱重排序: 在 hs_search 之后调用, 用关系距离重排结果 */
void hs_kg_rerank(const char *query_text,
                  hs_search_result_t *results, int *count,
                  struct MemoryCtx *ctx);

#endif /* HIPOOL_ENABLE_SEMANTIC */
#endif /* HIPOOL_SEMANTIC_H */
