/*
 * json.c — JSON 序列化/反序列化工具
 *
 * 轻量级 JSON 处理, 专为 hipool 的 key/value/tags 序列化设计。
 * 不依赖任何 JSON 库。
 */
#include "memory.h"
#include <errno.h>

void json_esc(const char *s, char *d, size_t dm) {
    size_t di = 0;
    for (size_t i = 0; s[i] && di < dm - 2; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"')      { d[di++] = '\\'; d[di++] = '"'; }
        else if (c == '\\'){ d[di++] = '\\'; d[di++] = '\\'; }
        else if (c == '\n'){ d[di++] = '\\'; d[di++] = 'n'; }
        else if (c == '\r'){ d[di++] = '\\'; d[di++] = 'r'; }
        else if (c == '\t'){ d[di++] = '\\'; d[di++] = 't'; }
        else d[di++] = c;
    }
    d[di] = '\0';
}

/* [P1-10] 定位 "key":" 的起始位置, 但跳过出现在某个 value 内部的伪匹配。
 * 旧实现直接 strstr, 若某 value 里恰好含字面量 "k":" 会错位匹配。
 * 改进: 只有当该 "key" 前面是 { 或 , (即作为 JSON 字段名, 而非字符串内容)
 * 才认为是合法字段。这是对手写 JSON 解析器最小侵入的加固, 不引入完整解析器。 */
static const char *json_find_field(const char *s, const char *key) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = s;
    while ((p = strstr(p, search)) != NULL) {
        /* 向前回溯跳过空白, 检查紧邻的前一个非空白字符是否为 { 或 ,
         * (字段分隔符)。注意 q 从 p-1 开始 (p 指向 "k":" 的首引号)。 */
        const char *q = p;
        while (q > s) { q--; if (*q != ' ' && *q != '\t') break; }
        if (q == s || *q == '{' || *q == ',') return p;
        p += strlen(search);   /* 继续向后找 */
    }
    return NULL;
}

int json_get_str(const char *s, const char *key, char *out, size_t om) {
    const char *p = json_find_field(s, key);
    if (!p) return -1;
    p += strlen(key) + 4;   /* 跳过 "key":" */
    size_t i = 0;
    while (*p && *p != '"' && i < om - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            if      (*p == 'n') out[i++] = '\n';
            else if (*p == 'r') out[i++] = '\r';
            else if (*p == 't') out[i++] = '\t';
            else out[i++] = *p;
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return 0;
}

int json_get_u64(const char *s, const char *key, uint64_t *v) {
    /* 数值字段定位: "key": 后直接跟数字 (非字符串), 同样需校验字段名上下文。 */
    char search[256];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = s;
    while ((p = strstr(p, search)) != NULL) {
        const char *q = p;
        while (q > s) { q--; if (*q != ' ' && *q != '\t') break; }
        if (q == s || *q == '{' || *q == ',') break;
        p += strlen(search);
    }
    if (!p) return -1;
    const char *start = p + strlen(search);
    while (*start == ' ' || *start == '\t') start++;
    errno = 0;
    char *end = NULL;
    unsigned long long val = strtoull(start, &end, 10);
    /* [P1-10] 旧实现不检查 strtoull 返回, 畸形输入静默返回 0。
     * 现校验: 无字符被消费 或 溢出 视为失败。 */
    if (end == start || errno == ERANGE) return -1;
    *v = (uint64_t)val;
    return 0;
}

int json_get_tags(const char *s, char tags[][MAX_TAG_LEN], int max) {
    const char *p = strstr(s, "\"tags\":[");
    if (!p) return 0;
    p += 8;
    int cnt = 0;
    while (*p && *p != ']' && cnt < max) {
        while (*p == ',' || *p == ' ') p++;
        if (*p == '"') {
            p++;
            size_t i = 0;
            /* [P1-10] 增加转义感知, 避免 value 内 \" 提前结束导致解析错位 */
            while (*p && i < MAX_TAG_LEN - 1) {
                if (*p == '\\' && *(p + 1)) {
                    p++;
                    if      (*p == 'n') tags[cnt][i++] = '\n';
                    else if (*p == 'r') tags[cnt][i++] = '\r';
                    else if (*p == 't') tags[cnt][i++] = '\t';
                    else tags[cnt][i++] = *p;
                    p++;
                } else if (*p == '"') {
                    break;
                } else {
                    tags[cnt][i++] = *p++;
                }
            }
            tags[cnt][i] = '\0';
            if (*p == '"') p++;
            cnt++;
        } else if (*p) {
            p++;   /* 跳过非预期字符, 避免死循环 */
        }
    }
    return cnt;
}
