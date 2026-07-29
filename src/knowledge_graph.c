/*
 * knowledge_graph.c — 嵌入式知识图谱
 *
 * 纯 C 三元组存储, 用于 hipool 语义搜索重排序
 */
#include "knowledge_graph.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ===================== 节点操作 ===================== */

static uint32_t kg_name_hash(const char *name) {
    uint32_t h = 5381;
    int c;
    while ((c = (unsigned char)*name++)) h = ((h << 5) + h) + (uint32_t)c;
    return h;
}

uint32_t kg_add_node(KnowledgeGraph *kg, const char *name, uint8_t entity_id) {
    if (kg->node_count >= KG_MAX_NODES) return UINT32_MAX;

    /* 去重 */
    for (uint32_t i = 0; i < kg->node_count; i++) {
        if (strcmp(kg->nodes[i].name, name) == 0) return i;
    }

    uint32_t id = kg->node_count++;
    strncpy(kg->nodes[id].name, name, sizeof(kg->nodes[id].name) - 1);
    kg->nodes[id].name[sizeof(kg->nodes[id].name) - 1] = '\0';
    kg->nodes[id].entity_id = entity_id;
    kg->nodes[id].relation_count = 0;
    kg->name_hash[id] = kg_name_hash(name);
    return id;
}

static int kg_find_node(KnowledgeGraph *kg, const char *name) {
    uint32_t h = kg_name_hash(name);
    for (uint32_t i = 0; i < kg->node_count; i++) {
        if (kg->name_hash[i] == h && strcmp(kg->nodes[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

/* ===================== 三元组操作 ===================== */

int kg_add_triple(KnowledgeGraph *kg,
                  const char *subject, KGRelationType rel,
                  const char *object, float weight) {
    int sid = kg_find_node(kg, subject);
    if (sid < 0) sid = (int)kg_add_node(kg, subject, 0);
    int oid = kg_find_node(kg, object);
    if (oid < 0) oid = (int)kg_add_node(kg, object, 0);

    if (sid < 0 || oid < 0 || kg->triple_count >= KG_MAX_TRIPLES)
        return -1;

    /* 去重 */
    for (uint32_t i = 0; i < kg->triple_count; i++) {
        KGTriple *t = &kg->triples[i];
        if (t->subject_id == (uint32_t)sid && t->relation == rel &&
            t->object_id == (uint32_t)oid) {
            t->weight = weight > t->weight ? weight : t->weight;
            return 0;
        }
    }

    KGTriple *t = &kg->triples[kg->triple_count++];
    t->subject_id = (uint32_t)sid;
    t->relation = rel;
    t->object_id = (uint32_t)oid;
    t->weight = weight;
    return 0;
}

int kg_query(KnowledgeGraph *kg, const char *name, KGTriple *out, int max_out) {
    int id = kg_find_node(kg, name);
    if (id < 0) return 0;

    int count = 0;
    for (uint32_t i = 0; i < kg->triple_count && count < max_out; i++) {
        if (kg->triples[i].subject_id == (uint32_t)id) {
            out[count++] = kg->triples[i];
        }
    }
    return count;
}

int kg_query_by_rel(KnowledgeGraph *kg, const char *name,
                    KGRelationType rel, uint32_t *out_ids, int max_out) {
    int id = kg_find_node(kg, name);
    if (id < 0) return 0;

    int count = 0;
    for (uint32_t i = 0; i < kg->triple_count && count < max_out; i++) {
        if (kg->triples[i].subject_id == (uint32_t)id &&
            kg->triples[i].relation == rel) {
            out_ids[count++] = kg->triples[i].object_id;
        }
    }
    return count;
}

/* ===================== 持久化 ===================== */

int kg_save(const KnowledgeGraph *kg, const char *path) {
    if (!kg) return -1;
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    fwrite(&kg->node_count, sizeof(uint32_t), 1, fp);
    fwrite(&kg->triple_count, sizeof(uint32_t), 1, fp);
    fwrite(kg->nodes, sizeof(KGNode), kg->node_count, fp);
    fwrite(kg->triples, sizeof(KGTriple), kg->triple_count, fp);
    fclose(fp);
    return 0;
}

int kg_load(KnowledgeGraph *kg, const char *path) {
    if (!kg) return -1;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    uint32_t nc, tc;
    if (fread(&nc, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); return -1; }
    if (fread(&tc, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); return -1; }
    if (nc > KG_MAX_NODES) nc = KG_MAX_NODES;
    if (tc > KG_MAX_TRIPLES) tc = KG_MAX_TRIPLES;
    kg->node_count = nc;
    kg->triple_count = tc;
    fread(kg->nodes, sizeof(KGNode), nc, fp);
    fread(kg->triples, sizeof(KGTriple), tc, fp);
    /* 重建 name_hash */
    for (uint32_t i = 0; i < nc; i++)
        kg->name_hash[i] = kg_name_hash(kg->nodes[i].name);
    fclose(fp);
    return 0;
}

/* ===================== 关系距离 ===================== */

/* BFS 计算两个节点间最短距离 (0=相同, 1=直接关联, 2=一跳, MAX=无关) */
#define KG_MAX_DIST 1000

static int kg_bfs_dist(KnowledgeGraph *kg, uint32_t start, uint32_t target) {
    if (start == target) return 0;

    /* 简单的 2 跳 BFS (嵌入式不搞复杂图算法) */
    uint32_t queue[KG_MAX_NODES], visited[KG_MAX_NODES] = {0};
    int dist[KG_MAX_NODES];
    int head = 0, tail = 0;

    queue[tail++] = start;
    visited[start] = 1;
    dist[start] = 0;

    while (head < tail) {
        uint32_t cur = queue[head++];
        if (dist[cur] >= 3) continue; /* 限制 3 跳 */

        for (uint32_t i = 0; i < kg->triple_count; i++) {
            uint32_t nid;
            if (kg->triples[i].subject_id == cur)
                nid = kg->triples[i].object_id;
            else if (kg->triples[i].object_id == cur)
                nid = kg->triples[i].subject_id;
            else
                continue;

            if (nid == target) return dist[cur] + 1;
            if (!visited[nid]) {
                visited[nid] = 1;
                dist[nid] = dist[cur] + 1;
                queue[tail++] = nid;
            }
        }
    }
    return KG_MAX_DIST;
}

/* knowledge_graph.c 末尾 — 不包含重排序, 重排序在 semantic.c 中实现 */
