/*
 * hashtable.c — DJB2 哈希表
 *
 * 4096 桶, 链地址法
 */
#include "memory.h"

int hash_init(HashTable *t) {
    memset(t->b, 0, sizeof(t->b));
    t->cnt = 0;
    return 0;
}

int hash_insert(HashTable *t, uint32_t kh, MemEntry *e) {
    uint32_t bk = kh % HASHTABLE_SIZE;
    HashNode *n = t->b[bk];
    while (n) {
        if (n->kh == kh) {
            /* 哈希碰撞: 确认真实 key 匹配才更新 */
            char *ek = ENTRY_KEY(n->e);
            size_t ekl = n->e->key_len;
            char *new_key = ENTRY_KEY(e);
            size_t new_kl = e->key_len;
            if (ekl == new_kl && memcmp(ek, new_key, ekl) == 0) {
                n->e = e; return 0;
            }
        }
        n = n->next;
    }
    HashNode *nn = (HashNode*)malloc(sizeof(HashNode));
    if (!nn) return -1;
    nn->kh = kh; nn->e = e; nn->next = t->b[bk]; t->b[bk] = nn; t->cnt++;
    return 0;
}

MemEntry *hash_lookup(HashTable *t, uint32_t kh, const char *key) {
    HashNode *n = t->b[kh % HASHTABLE_SIZE];
    while (n) {
        if (n->kh == kh) {
            /* 必须长度相同且内容完全一致才算命中。
             * 旧实现用 strncmp(key, ek, n->e->key_len)，当查询 key 比存储
             * key 长时只比存储长度就会错误命中（前缀碰撞），这里改为与
             * hash_insert 一致的严格比较。 */
            size_t kl = strlen(key);
            if (n->e->key_len == kl &&
                memcmp(ENTRY_KEY(n->e), key, kl) == 0) {
                n->e->accessed_at = (uint64_t)time(NULL);
                return n->e;
            }
        }
        n = n->next;
    }
    return NULL;
}

int hash_remove(HashTable *t, uint32_t kh, const char *key) {
    uint32_t bk = kh % HASHTABLE_SIZE;
    HashNode *n = t->b[bk], *pr = NULL;
    while (n) {
        if (n->kh == kh) {
            size_t kl = strlen(key);
            if (n->e->key_len == kl &&
                memcmp(ENTRY_KEY(n->e), key, kl) == 0) {
                if (pr) pr->next = n->next;
                else t->b[bk] = n->next;
                free(n); t->cnt--; return 0;
            }
        }
        pr = n; n = n->next;
    }
    return -1;
}
