/*
 * dict_distill.c — CC-CEDICT 词典蒸馏工具 v2
 *
 * 读取 CC-CEDICT (cedict_ts.u8) + 分类规则 → 输出 lexicon_generated.h
 * 纯 C, 零外部依赖.
 *
 * 编译: gcc -O2 dict_distill.c -o dict_distill
 * 运行: ./dict_distill cedict_raw/cedict_ts.u8 > lexicon_generated.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_WORD_LEN   64
#define MAX_ENGLISH    512
#define MAX_ENTRIES    20000
#define LINE_BUF       8192

/* ========== 分类规则表 ========== */
typedef struct { int entity_id; const char *keyword; } rule_t;

static const rule_t rules[] = {
    /* 动词 (entity_id=10) — 优先级最高 */
    {10, " verb "},       {10, " verb;"},
    /* 形容词 (entity_id=17) */
    {17, " adjective "},  {17, " adjective;"},
    /* 数词/量词 (entity_id=11) — 非常具体的优先 */
    {11, "numeral"},      {11, "measure word"}, {11, "classifier"},
    {11, " numeral "},    {11, "numeral;"},
    /* 颜色 (entity_id=15) */
    {15, "color"},        {15, "colour"},
    /* 食品/饮料 (entity_id=6) — 放在水产/走兽之前 */
    {6, "food"},          {6, "beverage"},     {6, "drink"},
    {6, "dish"},          {6, "soup"},         {6, "sauce"},
    {6, "bread"},         {6, "cake"},         {6, "rice"},
    {6, "wine"},          {6, "beer"},         {6, "tea"},
    {6, "coffee"},        {6, "milk"},         {6, "cheese"},
    {6, "chocolate"},     {6, "noodle"},       {6, "dumpling"},
    {6, "cookie"},        {6, "candy"},        {6, "honey"},
    {6, "jam"},           {6, "butter"},
    /* 水果 (entity_id=1) */
    {1, "fruit"},
    /* 蔬菜 (entity_id=5) */
    {5, "vegetable"},
    /* 水产 (entity_id=2) */
    {2, "fish"},          {2, "shellfish"},    {2, "seafood"},
    /* 禽鸟 (entity_id=3) */
    {3, "bird"},          {3, "poultry"},
    /* 走兽 (entity_id=4) */
    {4, "mammal"},        {4, "animal"},
    /* 家具 (entity_id=7) */
    {7, "furniture"},
    /* 电器 (entity_id=8) */
    {8, "electric"},      {8, "computer"},     {8, "device"},
    {8, "machine"},       {8, "appliance"},
    /* 衣物 (entity_id=9) */
    {9, "clothing"},      {9, "garment"},
    /* 交通 (entity_id=12) */
    {12, "vehicle"},      {12, "car"},         {12, "train"},
    {12, "airplane"},     {12, "ship"},
    /* 建筑 (entity_id=13) */
    {13, "building"},     {13, "house"},       {13, "room"},
    {13, "school"},       {13, "hospital"},    {13, "factory"},
    {13, "station"},      {13, "library"},     {13, "museum"},
    {13, "temple"},       {13, "church"},      {13, "airport"},
    /* 身体 (entity_id=14) */
    {14, "body"},         {14, "organ"},       {14, "bone"},
    /* 天气/自然 (entity_id=16) */
    {16, "weather"},      {16, "rain"},        {16, "snow"},
    {16, "wind"},         {16, "cloud"},       {16, "star"},
    {16, "mountain"},     {16, "river"},       {16, "forest"},
};
#define NRULES ((int)(sizeof(rules)/sizeof(rules[0])))

/* entity 名称表 (仅用于注释) */
static const char *eid_name(int eid) {
    switch (eid) {
        case 1: return "水果"; case 2: return "水产"; case 3: return "禽鸟";
        case 4: return "走兽"; case 5: return "蔬菜"; case 6: return "食品";
        case 7: return "家具"; case 8: return "电器"; case 9: return "衣物";
        case 10: return "动词"; case 11: return "数词"; case 12: return "交通";
        case 13: return "建筑"; case 14: return "身体"; case 15: return "颜色";
        case 16: return "自然"; case 17: return "形容词";
        default: return "?";
    }
}

/* tag 从 entity 映射 */
static int tag_from_eid(int eid) {
    if (eid == 10) return 1;  /* 动词 */
    if (eid == 11) return 3;  /* 数词 */
    if (eid == 17) return 2;  /* 形容词 */
    return 0;                 /* 名词 */
}
static const char *tag_name(int t) {
    switch (t) { case 0: return "M"; case 1: return "D"; case 2: return "X";
                 case 3: return "S"; default: return "M"; }
}

/* ========== 已分类词典条目 ========== */
typedef struct { char word[MAX_WORD_LEN]; int tag; int entity_id; } entry_t;

static entry_t entries[MAX_ENTRIES];
static int nentries = 0;

static void add_entry(const char *word, int tag, int entity_id) {
    if (nentries >= MAX_ENTRIES) return;
    /* 去重 */
    for (int i = 0; i < nentries; i++)
        if (strcmp(entries[i].word, word) == 0 &&
            entries[i].entity_id == entity_id) return;
    strncpy(entries[nentries].word, word, MAX_WORD_LEN - 1);
    entries[nentries].tag = tag;
    entries[nentries].entity_id = entity_id;
    nentries++;
}

/* ========== 英文释义分类 ========== */
/* 在英文释义中搜索关键词 (严格词边界: 前后只能是 / ; ( ) 空格 或标点) */
static int has_kw(const char *english, const char *kw) {
    const char *p = english;
    size_t klen = strlen(kw);
    while ((p = strstr(p, kw)) != NULL) {
        /* 前边界: 关键词在开头 或 前面是分隔符 */
        if (p > english) {
            char prev = p[-1];
            if (isalpha((unsigned char)prev)) { p++; continue; }
        }
        /* 后边界: 关键词在结尾 或 后面是分隔符 */
        char next = p[klen];
        if (next && isalpha((unsigned char)next)) { p++; continue; }
        /* 排除特定误匹配: 如 "school" 在 "school uniform" / "school of" 中不算建筑 */
        if (klen >= 4) {
            /* 对于有歧义的词, 检查后词 */
            const char *after = p + klen;
            while (*after == ' ') after++;
            if (strncmp(kw, "school", 6) == 0) {
                if (strncmp(after, "uniform", 7) == 0) { p++; continue; }
                if (strncmp(after, "of", 2) == 0) { p++; continue; }
                if (strncmp(after, "girl", 4) == 0) { p++; continue; }
                if (strncmp(after, "boy", 3) == 0) { p++; continue; }
            }
        }
        return 1;
    }
    return 0;
}

/* 返回 entity_id, -1 表示未分类 */
static int classify(const char *english) {
    char buf[MAX_ENGLISH];
    size_t el = strlen(english);
    size_t bl = el < MAX_ENGLISH - 1 ? el : MAX_ENGLISH - 1;

    /* 加前后空格方便词边界匹配 */
    buf[0] = ' ';
    for (size_t i = 0; i < bl; i++) buf[i+1] = (char)tolower((unsigned char)english[i]);
    buf[bl+1] = ' ';
    buf[bl+2] = '\0';

    for (int i = 0; i < NRULES; i++)
        if (has_kw(buf, rules[i].keyword)) return rules[i].entity_id;
    return -1;
}

/* ========== CC-CEDICT 行解析 ========== */
/* 格式: 传统 简体 [拼音] /英文释义1/英文释义2/ */
static void parse_line(const char *line) {
    /* 跳过注释行 */
    if (line[0] == '#') return;

    const char *simp = NULL, *eng = NULL;
    size_t simp_len = 0;

    /* 找到第一个空格后的简体词 */
    const char *p = line;
    /* 跳过传统词 (直到空格) */
    while (*p && *p != ' ') p++;
    if (!*p) return;
    p++; /* 跳过空格 */
    simp = p;
    /* 简体词到下一个空格 */
    while (*p && *p != ' ') p++;
    if (!*p) return;
    simp_len = (size_t)(p - simp);
    if (simp_len == 0 || simp_len >= MAX_WORD_LEN) return;

    /* 跳过拼音 [ ... ] */
    while (*p && *p != '[') p++;
    if (!*p) return;
    p++;
    while (*p && *p != ']') p++;
    if (!*p) return;
    p++; /* 跳过 ] */

    /* 跳过空格到 / */
    while (*p && *p != '/') p++;
    if (!*p) return;
    p++; /* 跳过第一个 / */

    eng = p;
    size_t eng_len = 0;
    /* 英文到行尾或 / */
    while (p[eng_len] && p[eng_len] != '\n' && p[eng_len] != '\r') eng_len++;
    if (eng_len >= MAX_ENGLISH) eng_len = MAX_ENGLISH - 1;

    char english[MAX_ENGLISH];
    strncpy(english, eng, eng_len);
    english[eng_len] = '\0';

    /* 截断: 多次出现的释义用 / 分隔, 只取第一段 */
    char *slash = strchr(english, '/');
    if (slash) *slash = '\0';

    int eid = classify(english);
    if (eid < 0) return;  /* 不收录未分类词 */

    /* 取简体词 */
    char word[MAX_WORD_LEN];
    strncpy(word, simp, simp_len);
    word[simp_len] = '\0';

    int tag = tag_from_eid(eid);
    add_entry(word, tag, eid);
}

/* ========== 主函数 ========== */
int main(int argc, char **argv) {
    const char *filepath;
    if (argc > 1) {
        filepath = argv[1];
    } else {
        /* 默认路径 */
#ifdef _WIN32
        filepath = "cedict_raw\\cedict_ts.u8";
#else
        filepath = "cedict_raw/cedict_ts.u8";
#endif
    }

    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        fprintf(stderr, "Error: cannot open %s\n", filepath);
        fprintf(stderr, "Usage: %s <cedict_ts.u8> > lexicon_generated.h\n", argv[0]);
        return 1;
    }

    char line[LINE_BUF];
    long total_lines = 0, parsed = 0;
    while (fgets(line, sizeof(line), fp)) {
        total_lines++;
        parse_line(line);
        /* 进度 */
        if (total_lines % 10000 == 0) {
            fprintf(stderr, "\r  读取: %ld 行 | 已分类: %d 词", total_lines, nentries);
            fflush(stderr);
        }
    }
    fclose(fp);

    fprintf(stderr, "\r  读取: %ld 行 | 已分类: %d 词\n", total_lines, nentries);

    /* ========== 输出 C 头文件 ========== */
    printf("/*\n");
    printf(" * lexicon_generated.h — CC-CEDICT 蒸馏自动生成\n");
    printf(" * 源文件: %s (%ld entries)\n", filepath, total_lines);
    printf(" * 已分类: %d 词条\n", nentries);
    printf(" */\n\n");

    printf("static const hs_dict_entry_t hs_dict_generated[] = {\n");

    /* 输出前按拼音排序? 不排序, 保持分类分组 */
    for (int i = 0; i < nentries; i++) {
        printf("    {\"%s\", HS_TAG_%s, %d},  /* %s */\n",
               entries[i].word,
               tag_name(entries[i].tag),
               entries[i].entity_id,
               eid_name(entries[i].entity_id));
    }

    printf("};\n");
    printf("#define HS_DICT_GENERATED_ENTRIES (int)(sizeof(hs_dict_generated)/sizeof(hs_dict_generated[0]))\n");

    /* 统计 */
    fprintf(stderr, "\n=== 分类统计 ===\n");
    int counts[18] = {0};
    for (int i = 0; i < nentries; i++) {
        int e = entries[i].entity_id;
        if (e >= 0 && e <= 17) counts[e]++;
    }
    for (int e = 1; e <= 17; e++) {
        if (counts[e] > 0)
            fprintf(stderr, "  %s: %d\n", eid_name(e), counts[e]);
    }
    fprintf(stderr, "  总计: %d\n", nentries);

    return 0;
}
