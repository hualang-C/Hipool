/*
 * semantic.c — 语义检索模块
 *
 * 中文语义检索引擎:
 * 1. FMM (Forward Maximum Matching) 分词器
 * 2. 手工词典 + CC-CEDICT 蒸馏词典
 * 3. 5D 向量编码 (动词比例/实体密度/实体类型/bigram指纹/长度)
 * 4. TopK 向量相似度搜索
 */
#include "memory.h"
#include "semantic.h"
#include "knowledge_graph.h"
#include <math.h>

#ifdef HIPOOL_ENABLE_SEMANTIC

/* ===================== 类型定义 ===================== */
#define HS_MAX_WORD_LEN    48

typedef enum {
    HS_TAG_M   = 0,  // 名词
    HS_TAG_D   = 1,  // 动词
    HS_TAG_X   = 2,  // 形容词
    HS_TAG_S   = 3,  // 数词/量词
    HS_TAG_D1  = 4,  // 代词
    HS_TAG_F   = 5,  // 副词
    HS_TAG_UNK = -1  // 未知
} hs_pos_tag_t;

typedef struct {
    const char *word;
    size_t      len;
    hs_pos_tag_t tag;
    int         entity_id;
    int         freq;
    float       weight;
} hs_token_t;

typedef struct {
    float x;  // 动词比例
    float y;  // 实体密度
    float z;  // 主要实体类型
    float w;  // bigram 指纹
    float v;  // 长度权重
    float u;  // 语义角色 (句式模式: 陈述/动作/描述/判断)
    float t;  // 语义极性 (语气: 正面/中性/负面)
} hs_vec7_t;

typedef struct {
    uint32_t key_hash;
    hs_vec7_t vec;
    float    len;
} hs_doc_vec_t;

typedef struct {
    const char *word;
    hs_pos_tag_t tag;
    int         entity_id;
} hs_dict_entry_t;

/* ===================== 手工词典 ===================== */
static const hs_dict_entry_t hs_dict[] = {
    /* 水果 entity_id=1 */
    {"苹果", HS_TAG_M, 1}, {"梨",   HS_TAG_M, 1}, {"西瓜", HS_TAG_M, 1},
    {"香蕉", HS_TAG_M, 1}, {"葡萄", HS_TAG_M, 1}, {"草莓", HS_TAG_M, 1},
    {"橙子", HS_TAG_M, 1}, {"橘子", HS_TAG_M, 1}, {"桃子", HS_TAG_M, 1},
    {"樱桃", HS_TAG_M, 1}, {"芒果", HS_TAG_M, 1}, {"荔枝", HS_TAG_M, 1},
    {"菠萝", HS_TAG_M, 1}, {"猕猴桃",HS_TAG_M,1}, {"柿子", HS_TAG_M, 1},
    /* 水产 entity_id=2 */
    {"鲤鱼", HS_TAG_M, 2}, {"鲫鱼", HS_TAG_M, 2}, {"草鱼", HS_TAG_M, 2},
    {"鲢鱼", HS_TAG_M, 2}, {"三文鱼",HS_TAG_M,2}, {"大西洋鲑",HS_TAG_M,2},
    {"带鱼", HS_TAG_M, 2}, {"黄鱼", HS_TAG_M, 2}, {"鲈鱼", HS_TAG_M, 2},
    {"虾",   HS_TAG_M, 2}, {"蟹",   HS_TAG_M, 2}, {"螃蟹", HS_TAG_M, 2},
    {"甲鱼", HS_TAG_M, 2}, {"鲍鱼", HS_TAG_M, 2}, {"海参", HS_TAG_M, 2},
    /* 禽鸟 entity_id=3 */
    {"鸡", HS_TAG_M, 3}, {"鸭", HS_TAG_M, 3}, {"鹅", HS_TAG_M, 3},
    {"鸽子", HS_TAG_M, 3}, {"麻雀", HS_TAG_M, 3}, {"乌鸦", HS_TAG_M, 3},
    {"鹰",   HS_TAG_M, 3}, {"孔雀", HS_TAG_M, 3}, {"鹦鹉", HS_TAG_M, 3},
    {"燕子", HS_TAG_M, 3},
    /* 走兽 entity_id=4 */
    {"狗", HS_TAG_M, 4}, {"猫", HS_TAG_M, 4}, {"猪", HS_TAG_M, 4},
    {"牛", HS_TAG_M, 4}, {"羊", HS_TAG_M, 4}, {"马", HS_TAG_M, 4},
    {"老虎", HS_TAG_M, 4}, {"狮子", HS_TAG_M, 4}, {"大象", HS_TAG_M, 4},
    {"兔子", HS_TAG_M, 4}, {"老鼠", HS_TAG_M, 4},
    /* 日常动词 entity_id=10 */
    {"吃", HS_TAG_D, 10}, {"喝", HS_TAG_D, 10}, {"跑", HS_TAG_D, 10},
    {"走", HS_TAG_D, 10}, {"看", HS_TAG_D, 10}, {"听", HS_TAG_D, 10},
    {"说", HS_TAG_D, 10}, {"想", HS_TAG_D, 10}, {"做", HS_TAG_D, 10},
    {"写", HS_TAG_D, 10}, {"读", HS_TAG_D, 10}, {"睡", HS_TAG_D, 10},
    {"坐", HS_TAG_D, 10}, {"站", HS_TAG_D, 10}, {"买", HS_TAG_D, 10},
    {"卖", HS_TAG_D, 10}, {"打", HS_TAG_D, 10}, {"拿", HS_TAG_D, 10},
    {"放", HS_TAG_D, 10}, {"开", HS_TAG_D, 10}, {"关", HS_TAG_D, 10},
    {"去", HS_TAG_D, 10}, {"来", HS_TAG_D, 10},
    /* 数词/量词 entity_id=11 */
    {"一", HS_TAG_S, 11}, {"二", HS_TAG_S, 11}, {"三", HS_TAG_S, 11},
    {"四", HS_TAG_S, 11}, {"五", HS_TAG_S, 11}, {"六", HS_TAG_S, 11},
    {"七", HS_TAG_S, 11}, {"八", HS_TAG_S, 11}, {"九", HS_TAG_S, 11},
    {"十", HS_TAG_S, 11}, {"百", HS_TAG_S, 11}, {"千", HS_TAG_S, 11},
    {"万", HS_TAG_S, 11}, {"个", HS_TAG_S, 11}, {"只", HS_TAG_S, 11},
    {"条", HS_TAG_S, 11}, {"斤", HS_TAG_S, 11}, {"两", HS_TAG_S, 11},
    {"碗", HS_TAG_S, 11}, {"杯", HS_TAG_S, 11}, {"瓶", HS_TAG_S, 11},
    {"根", HS_TAG_S, 11}, {"块", HS_TAG_S, 11}, {"片", HS_TAG_S, 11}, {"张", HS_TAG_S, 11},
    /* 代词 */
    {"我", HS_TAG_D1, -1}, {"你", HS_TAG_D1, -1}, {"他", HS_TAG_D1, -1},
    {"她", HS_TAG_D1, -1}, {"它", HS_TAG_D1, -1},
    {"这", HS_TAG_D1, -1}, {"那", HS_TAG_D1, -1},
    {"谁", HS_TAG_D1, -1}, {"什么",HS_TAG_D1,-1},
    /* 副词 */
    {"很", HS_TAG_F, -1}, {"非常",HS_TAG_F,-1}, {"太", HS_TAG_F, -1},
    {"最", HS_TAG_F, -1}, {"更", HS_TAG_F, -1}, {"还", HS_TAG_F, -1},
    {"也", HS_TAG_F, -1}, {"都", HS_TAG_F, -1}, {"就", HS_TAG_F, -1},
    {"又", HS_TAG_F, -1}, {"再", HS_TAG_F, -1}, {"不", HS_TAG_F, -1},
    {"没", HS_TAG_F, -1}, {"已", HS_TAG_F, -1}, {"了", HS_TAG_F, -1},
    {"在", HS_TAG_F, -1},
    /* 常用名词(无实体) */
    {"昨天",HS_TAG_M, -1}, {"今天",HS_TAG_M, -1}, {"明天",HS_TAG_M, -1},
    {"苹果公司",HS_TAG_M, -1},
};
#define HS_DICT_ENTRIES (int)(sizeof(hs_dict)/sizeof(hs_dict[0]))

/* 包含蒸馏生成的扩展词典 */
#include "../lexicon/lexicon_generated.h"

/* ===================== FMM 分词器 ===================== */

#define HS_BUCKETS 256
static const hs_dict_entry_t *hs_buckets[HS_BUCKETS][2000];
static int hs_bucket_cnt[HS_BUCKETS];
static int hs_buckets_built = 0;

static inline size_t hs_utf8_len(const char *s) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) return 1;
    if (c < 0xC0) return 1;
    if (c < 0xE0) return 2;
    if (c < 0xF0) return 3;
    return 4;
}

static inline uint32_t hs_bkdr(const char *s, size_t n) {
    uint32_t h = 131;
    for (size_t i = 0; i < n && s[i]; i++)
        h = h * 131 + (unsigned char)s[i];
    return h;
}

static void hs_build_buckets(void) {
    if (hs_buckets_built) return;
    for (int i = 0; i < HS_DICT_ENTRIES; i++) {
        unsigned char first = (unsigned char)hs_dict[i].word[0];
        int b = hs_bucket_cnt[first];
        if (b < 2000) hs_buckets[first][b] = &hs_dict[i];
        hs_bucket_cnt[first]++;
    }
    for (int i = 0; i < HS_DICT_GENERATED_ENTRIES; i++) {
        unsigned char first = (unsigned char)hs_dict_generated[i].word[0];
        int b = hs_bucket_cnt[first];
        if (b < 2000) hs_buckets[first][b] = &hs_dict_generated[i];
        hs_bucket_cnt[first]++;
    }
    hs_buckets_built = 1;
}

static const hs_dict_entry_t *hs_dict_match(const char *text, size_t max_len) {
    hs_build_buckets();
    const hs_dict_entry_t *best = NULL;
    size_t best_len = 0;
    unsigned char first = (unsigned char)text[0];
    int n = hs_bucket_cnt[first];
    for (int i = 0; i < n; i++) {
        const char *dw = hs_buckets[first][i]->word;
        size_t dl = strlen(dw);
        if (dl > max_len || dl <= best_len) continue;
        if (strncmp(text, dw, dl) == 0) { best = hs_buckets[first][i]; best_len = dl; }
    }
    return best;
}

static int hs_tokenize(const char *text, hs_token_t tokens[], int max_tokens) {
    if (!text || !text[0]) return 0;
    int count = 0;
    const char *p = text;
    while (*p && count < max_tokens) {
        size_t remain = strlen(p);
        if (*p == ' ' || *p == '\t' || *p == '\n') { p++; continue; }
        if (*p >= '0' && *p <= '9') {
            tokens[count].word = p;
            tokens[count].len = 1;
            tokens[count].tag = HS_TAG_S;
            tokens[count].entity_id = 11;
            tokens[count].freq = 1;
            p++; count++;
            continue;
        }
        size_t max_match = remain > 18 ? 18 : remain;
        const hs_dict_entry_t *de = hs_dict_match(p, max_match);
        if (de) {
            tokens[count].word = p;
            tokens[count].len = strlen(de->word);
            tokens[count].tag = de->tag;
            tokens[count].entity_id = de->entity_id;
            tokens[count].freq = 1;
            p += tokens[count].len;
            count++;
        } else {
            int matched = 0;
            for (int clen = 12; clen >= 3; clen -= 3) {
                if (clen > (int)remain) continue;
                char buf[16] = {0};
                memcpy(buf, p, (size_t)clen);
                const hs_dict_entry_t *de2 = hs_dict_match(buf, (size_t)clen);
                if (de2) {
                    tokens[count].word = p;
                    tokens[count].len = strlen(de2->word);
                    tokens[count].tag = de2->tag;
                    tokens[count].entity_id = de2->entity_id;
                    tokens[count].freq = 1;
                    p += tokens[count].len;
                    count++;
                    matched = 1;
                    break;
                }
            }
            if (matched) continue;
            size_t ulen = hs_utf8_len(p);
            tokens[count].word = p;
            tokens[count].len = ulen;
            tokens[count].tag = HS_TAG_UNK;
            tokens[count].entity_id = -1;
            tokens[count].freq = 1;
            p += ulen;
            count++;
        }
    }
    if (count > 1) {
        int w = 0;
        for (int r = 1; r < count; r++) {
            if (tokens[w].len == tokens[r].len &&
                memcmp(tokens[w].word, tokens[r].word, tokens[w].len) == 0) {
                tokens[w].freq++;
            } else {
                w++;
                if (w != r) memcpy(&tokens[w], &tokens[r], sizeof(hs_token_t));
            }
        }
        count = w + 1;
    }
    return count;
}

/* ===================== 7D 向量编码 ===================== */
/* 7D 向量中 entity_id 的权重 */
#ifndef HS_ENTITY_WEIGHT
#define HS_ENTITY_WEIGHT 6
#endif

/* --- U轴: 语义角色检测 ---
 * 通过虚词标记判断句式结构:
 *   "XX是XX" → 判断句式 0.2
 *   "XX很XX" → 描述句式 0.4
 *   "XX了XX" → 完成句式 0.6
 *   "XX的XX" → 修饰句式 0.8
 *   无虚词 → 名词列表 0.0
 */
static float hs_detect_role(const hs_token_t tokens[], int count) {
    int has_shi = 0, has_le = 0, has_hen = 0, has_de = 0, has_zai = 0;
    for (int i = 0; i < count; i++) {
        if (tokens[i].len == 3) { /* 单汉字 UTF-8 是 3 字节 */
            if (strncmp(tokens[i].word, "是", 3) == 0) has_shi = 1;
            if (strncmp(tokens[i].word, "了", 3) == 0) has_le = 1;
            if (strncmp(tokens[i].word, "很", 3) == 0) has_hen = 1;
            if (strncmp(tokens[i].word, "的", 3) == 0) has_de = 1;
            if (strncmp(tokens[i].word, "在", 3) == 0) has_zai = 1;
        }
    }
    if (has_shi && has_de) return 0.9f;  /* "XX是XX的" - 复合判断 */
    if (has_shi)            return 0.2f;  /* "XX是XX" - 判断句式 */
    if (has_hen)            return 0.4f;  /* "XX很XX" - 描述句式 */
    if (has_zai)            return 0.5f;  /* "XX在XX" - 正在进行 */
    if (has_le)             return 0.6f;  /* "XX了XX" - 完成句式 */
    if (has_de)             return 0.8f;  /* "XX的XX" - 修饰句式 */
    return 0.0f;                         /* 名词列表 */
}

/* --- T轴: 语义极性检测 ---
 * 通过情感词比例判断语气
 */
static float hs_detect_tone(const hs_token_t tokens[], int count) {
    /* 正面词: 好/优秀/完美/成功/先进/有效/喜欢/漂亮/方便/新鲜 */
    static const char *pos_words[] = {"好", "优秀", "完美", "成功", "先进",
                                      "有效", "喜欢", "漂亮", "方便", "新鲜"};
    /* 负面词: 差/坏/失败/危险/错误/难/讨厌/麻烦/落后/无聊 */
    static const char *neg_words[] = {"差", "坏", "失败", "危险", "错误",
                                      "难", "讨厌", "麻烦", "落后", "无聊"};
    /* 加强词: 非常/很/太/最/更 */
    int pos = 0, neg = 0, intensifier = 0;
    for (int i = 0; i < count; i++) {
        size_t wl = tokens[i].len;
        for (size_t p = 0; p < sizeof(pos_words)/sizeof(pos_words[0]); p++) {
            if (wl == strlen(pos_words[p]) && memcmp(tokens[i].word, pos_words[p], wl) == 0)
                { pos++; break; }
        }
        for (size_t n = 0; n < sizeof(neg_words)/sizeof(neg_words[0]); n++) {
            if (wl == strlen(neg_words[n]) && memcmp(tokens[i].word, neg_words[n], wl) == 0)
                { neg++; break; }
        }
        if (wl == 3) {
            if (strncmp(tokens[i].word, "非常", 6) == 0 ||
                strncmp(tokens[i].word, "很", 3) == 0 ||
                strncmp(tokens[i].word, "太", 3) == 0 ||
                strncmp(tokens[i].word, "最", 3) == 0 ||
                strncmp(tokens[i].word, "更", 3) == 0) intensifier++;
        }
    }
    /* T = (总词数 + pos*2 - neg*2 + intensifier_sign) / (总词数 × 2 + 2) */
    /* 范围 0~1, 0.5=中性 */
    float fcount = (float)(count > 0 ? count : 1);
    float base = (float)((int)(fcount + (float)pos * 2.0f - (float)neg * 2.0f + (float)intensifier * ((pos > neg) ? 1.0f : -1.0f)));
    float t = base / (fcount * 2.0f + 2.0f);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

static hs_vec7_t hs_encode(const hs_token_t tokens[], int count) {
    hs_vec7_t v = {0, 0, 0, 0, 0, 0, 0};
    if (count == 0) return v;

    int verb_count = 0, entity_count = 0, valid_count = 0;
    float entity_type_sum = 0;
    int dominant_entity = -1;
    /* [P1-14] 数组大小绑定 ENTITY_MAX: 旧实现硬编码 [32] 且注释说 max=20,
     * 实际 ENTITY_NETWORK_DEVICE=24, 若新增分类到 >32 会越界。现用常量,
     * 新增分类只需更新 ENTITY_MAX 即自动扩展。 */
    int entity_freq[ENTITY_MAX + 1] = {0};

    uint32_t bigram_hash = 5381;
    int bigram_count = 0;
    char prev_chars[4] = {0};
    int has_prev = 0;

    for (int i = 0; i < count; i++) {
        if (tokens[i].tag == HS_TAG_UNK) continue;
        valid_count++;
        if (tokens[i].tag == HS_TAG_D) verb_count++;
        if (tokens[i].entity_id >= 0) {
            entity_count++;
            entity_type_sum += (float)tokens[i].entity_id;
            int eid = tokens[i].entity_id;
            /* [P1-14] 旧实现 eid<=17 漏统计 18..24 的实体 (ENTITY_BRAND 等),
             * 与下方 e<=24 的遍历范围不一致。统一用 ENTITY_MAX。 */
            if (eid >= 1 && eid <= ENTITY_MAX) entity_freq[eid]++;
        }
        if (bigram_count < 32) {
            const unsigned char *wp = (const unsigned char*)tokens[i].word;
            size_t wl = tokens[i].len;
            size_t take = wl < 6 ? wl : 6;
            for (size_t b = 0; b < take; b++)
                bigram_hash = bigram_hash * 33 + wp[b];
            bigram_count++;
        }
        if (has_prev) {
            bigram_hash = bigram_hash * 33 + (unsigned char)prev_chars[0];
            bigram_hash = bigram_hash * 33 + (unsigned char)tokens[i].word[0];
        }
        prev_chars[0] = tokens[i].word[0];
        has_prev = 1;
    }

    if (valid_count == 0) {
        v.x = 0.5f; v.y = 0.0f; v.z = 0.5f; v.w = 0.5f; v.v = 0.1f;
        v.u = 0.5f; v.t = 0.5f;
        return v;
    }

    float fc = (float)valid_count;
    v.x = (float)verb_count / fc;
    v.y = (float)entity_count / fc;

    int max_freq = 0;
    for (int e = 1; e <= ENTITY_MAX; e++) {
        if (entity_freq[e] > max_freq) {
            max_freq = entity_freq[e];
            dominant_entity = e;
        }
    }
    if (dominant_entity > 0) {
        float weighted = (float)dominant_entity * (float)HS_ENTITY_WEIGHT;
        v.z = (weighted + (float)(bigram_hash % 100) / 100.0f) / 200.0f;
        if (v.z > 1.0f) v.z = 1.0f;
    } else {
        v.z = 0.5f;
    }

    v.w = (float)(bigram_hash % 100000) / 100000.0f;
    float raw_v = fc / 20.0f;
    v.v = raw_v > 1.0f ? 1.0f : raw_v;

    /* U轴: 语义角色 */
    v.u = hs_detect_role(tokens, count);

    /* T轴: 语义极性 */
    v.t = hs_detect_tone(tokens, count);

    return v;
}

static float hs_vec_len(hs_vec7_t v) {
    return sqrtf(v.x*v.x + v.y*v.y + v.z*v.z + v.w*v.w + v.v*v.v + v.u*v.u + v.t*v.t);
}

static float hs_sq_dist(hs_vec7_t a, hs_vec7_t b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    float dw = a.w - b.w, dv = a.v - b.v, du = a.u - b.u, dt = a.t - b.t;
    return dx*dx + dy*dy + dz*dz + dw*dw + dv*dv + du*du + dt*dt;
}

/* ===================== 全局语义索引 ===================== */
static hs_doc_vec_t  hs_docs[HS_MAX_DOCS];
static int           hs_doc_count = 0;

int hs_index_entry(uint32_t key_hash, const char *text) {
    if (!text || !text[0]) return -1;
    hs_token_t tokens[HS_MAX_TOKENS];
    int n = hs_tokenize(text, tokens, HS_MAX_TOKENS);
    if (n <= 0) return -1;
    hs_vec7_t v = hs_encode(tokens, n);
    int exist = -1;
    for (int i = 0; i < hs_doc_count; i++) {
        if (hs_docs[i].key_hash == key_hash) { exist = i; break; }
    }
    if (exist >= 0) {
        hs_docs[exist].vec = v;
        hs_docs[exist].len = hs_vec_len(v);
    } else {
        if (hs_doc_count >= HS_MAX_DOCS) return -1;
        hs_docs[hs_doc_count].key_hash = key_hash;
        hs_docs[hs_doc_count].vec = v;
        hs_docs[hs_doc_count].len = hs_vec_len(v);
        hs_doc_count++;
    }
    return 0;
}

int hs_deindex_entry(uint32_t key_hash) {
    for (int i = 0; i < hs_doc_count; i++) {
        if (hs_docs[i].key_hash == key_hash) {
            hs_docs[i] = hs_docs[hs_doc_count - 1];
            hs_doc_count--;
            return 0;
        }
    }
    return -1;
}

int hs_search(const char *query, int top_k, hs_search_result_t results[]) {
    if (!query || !query[0] || hs_doc_count == 0) return 0;
    hs_token_t tokens[HS_MAX_TOKENS];
    int n = hs_tokenize(query, tokens, HS_MAX_TOKENS);
    if (n <= 0) return 0;
    hs_vec7_t qv = hs_encode(tokens, n);
    if (top_k > HS_TOP_K) top_k = HS_TOP_K;

    typedef struct { uint32_t kh; float dist; } dist_t;
    dist_t all[HS_MAX_DOCS];
    int cnt = 0;
    for (int i = 0; i < hs_doc_count; i++) {
        float d = hs_sq_dist(qv, hs_docs[i].vec);
        if (d > HS_DIST_THRESHOLD) continue;
        all[cnt].kh = hs_docs[i].key_hash;
        all[cnt].dist = d;
        cnt++;
    }

    int rc = 0;
    for (int i = 0; i < cnt && rc < top_k; i++) {
        int j = rc - 1;
        while (j >= 0 && all[i].dist < results[j].distance) {
            results[j + 1] = results[j];
            j--;
        }
        results[j + 1].key_hash = all[i].kh;
        results[j + 1].distance = all[i].dist;
        rc++;
    }
    return rc;
}

/* ===================== 知识图谱重排序 ===================== */
/* 在 hs_search 之后调用, 用知识图谱的关系距离提升相关结果 */
void hs_kg_rerank(const char *query_text,
                  hs_search_result_t *results, int *count,
                  MemoryCtx *ctx) {
    if (!ctx->kg || !query_text || *count <= 0) return;

    /* 1. 找出查询文本中在知识图谱中的实体 */
    uint32_t query_entities[16];
    int nq = 0;
    const char *p = query_text;
    while (*p && nq < 16) {
        if (*p == ' ' || *p == '\t') { p++; continue; }
        for (uint32_t i = 0; i < ctx->kg->node_count; i++) {
            size_t nl = strlen(ctx->kg->nodes[i].name);
            if (strncmp(p, ctx->kg->nodes[i].name, nl) == 0) {
                int dup = 0;
                for (int d = 0; d < nq; d++)
                    if (query_entities[d] == i) { dup = 1; break; }
                if (!dup) query_entities[nq++] = i;
                p += nl;
                break;
            }
        }
        p++;
    }
    if (nq == 0) return;

    /* 2. 对每个结果, 计算图谱距离, 调整得分 */
    for (int i = 0; i < *count; i++) {
        float min_dist = 1000.0f;

        for (int b = 0; b < HASHTABLE_SIZE; b++) {
            HashNode *hn = ctx->table.b[b];
            while (hn) {
                if (hn->e && (hn->e->flags & FLAG_ACTIVE) &&
                    hn->e->key_hash == results[i].key_hash) {
                    char *vd = ENTRY_VAL(hn->e);
                    for (uint32_t ni = 0; ni < ctx->kg->node_count; ni++) {
                        if (strstr(vd, ctx->kg->nodes[ni].name)) {
                            for (int qi = 0; qi < nq; qi++) {
                                /* 简单的 BFS 距离 */
                                uint32_t s = query_entities[qi];
                                uint32_t t = ni;
                                if (s == t) { min_dist = 0.0f; goto done; }
                                /* 1 跳: 直接关联 */
                                for (uint32_t k = 0; k < ctx->kg->triple_count; k++) {
                                    KGTriple *tr = &ctx->kg->triples[k];
                                    if ((tr->subject_id == s && tr->object_id == t) ||
                                        (tr->subject_id == t && tr->object_id == s)) {
                                        if (1.0f < min_dist) min_dist = 1.0f;
                                        goto done;
                                    }
                                }
                            }
                        }
                    }
                    goto done;
                }
                hn = hn->next;
            }
        }
done:
        /* 调整距离: 7D×0.4 + KG×0.6 */
        /* KG 距离映射: 0跳=0, 1跳=0.15, 无关联=0.6 */
        { float kg_score;
        if (min_dist > 500.0f)      kg_score = 0.6f;   /* 无关联 */
        else if (min_dist <= 0.5f)  kg_score = 0.0f;   /* 相同实体 */
        else                        kg_score = 0.15f;  /* 1 跳关联 */
        results[i].distance = results[i].distance * 0.4f + kg_score * 0.6f; }
    }

    /* [P0] 重排序: KG 调整了各结果的 distance, 必须按新距离重排数组,
     * 否则 SKILL.md 承诺的"相关项提升、无关项压下"无法体现 ——
     * 旧实现只改 distance 字段不重排, 导致 final 距离已变化但排名顺序不变。 */
    for (int i = 1; i < *count; i++) {
        hs_search_result_t cur = results[i];
        int j = i - 1;
        while (j >= 0 && results[j].distance > cur.distance) {
            results[j + 1] = results[j];
            j--;
        }
        results[j + 1] = cur;
    }
}

#endif /* HIPOOL_ENABLE_SEMANTIC */
