/*
 * knowledge_graph.h — 嵌入式知识图谱
 *
 * 纯 C 三元组存储, 用于 hipool 语义搜索重排序
 * 不做循环依赖, MemoryCtx 相关的重排序函数在 semantic.h 中声明
 */
#ifndef HIPOOL_KG_H
#define HIPOOL_KG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 三元组核心 ===================== */
#define KG_MAX_NODES    2048
#define KG_MAX_TRIPLES  8192

#define KG_NODE_NAME_LEN 48

typedef struct {
    uint32_t    id;
    char        name[KG_NODE_NAME_LEN];
    uint8_t     entity_id;
    uint16_t    relation_count;
} KGNode;

typedef enum {
    REL_NONE       = 0,
    REL_IS_A       = 1,
    REL_CATEGORY   = 2,
    REL_DYNASTY    = 3,
    REL_IDENTITY   = 4,
    REL_INVENTED   = 5,
    REL_LOCATED    = 6,
    REL_USED_FOR   = 7,
    REL_SIMILAR    = 8,
    REL_OPPOSITE   = 9,
    REL_PART_OF    = 10,
} KGRelationType;

typedef struct {
    uint32_t       subject_id;
    KGRelationType relation;
    uint32_t       object_id;
    float          weight;
} KGTriple;

/* ===================== 存储结构 ===================== */
typedef struct {
    KGNode    nodes[KG_MAX_NODES];
    uint32_t  node_count;
    uint32_t  name_hash[KG_MAX_NODES];
    KGTriple  triples[KG_MAX_TRIPLES];
    uint32_t  triple_count;
    uint32_t  subj_idx[KG_MAX_NODES + 1];
} KnowledgeGraph;

/* ===================== 核心 API ===================== */
uint32_t kg_add_node(KnowledgeGraph *kg, const char *name, uint8_t entity_id);
int      kg_add_triple(KnowledgeGraph *kg,
                       const char *subject, KGRelationType rel,
                       const char *object, float weight);
int      kg_query(KnowledgeGraph *kg, const char *name,
                  KGTriple *out, int max_out);
int      kg_query_by_rel(KnowledgeGraph *kg, const char *name,
                         KGRelationType rel, uint32_t *out_ids, int max_out);

int kg_save(const KnowledgeGraph *kg, const char *path);
int kg_load(KnowledgeGraph *kg, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* HIPOOL_KG_H */
