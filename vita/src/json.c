#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char *skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static JsonNode *new_node(JsonType type) {
    JsonNode *n = (JsonNode *)calloc(1, sizeof(JsonNode));
    if (n) n->type = type;
    return n;
}

static char *parse_string_raw(const char **pp) {
    const char *p = *pp;
    if (*p != '"') return NULL;
    p++;
    size_t cap = 64, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;

    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            switch (*p) {
                case '"':  c = '"'; p++; break;
                case '\\': c = '\\'; p++; break;
                case '/':  c = '/'; p++; break;
                case 'b':  c = '\b'; p++; break;
                case 'f':  c = '\f'; p++; break;
                case 'n':  c = '\n'; p++; break;
                case 'r':  c = '\r'; p++; break;
                case 't':  c = '\t'; p++; break;
                case 'u': {
                    unsigned int cp = 0;
                    p++;
                    for (int i = 0; i < 4 && *p; i++, p++) {
                        cp <<= 4;
                        if (*p >= '0' && *p <= '9') cp |= *p - '0';
                        else if (*p >= 'a' && *p <= 'f') cp |= *p - 'a' + 10;
                        else if (*p >= 'A' && *p <= 'F') cp |= *p - 'A' + 10;
                    }
                    if (cp < 0x80) {
                        c = (char)cp;
                    } else {
                        if (len + 3 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
                        if (cp < 0x800) {
                            out[len++] = (char)(0xC0 | (cp >> 6));
                            out[len++] = (char)(0x80 | (cp & 0x3F));
                        } else {
                            out[len++] = (char)(0xE0 | (cp >> 12));
                            out[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[len++] = (char)(0x80 | (cp & 0x3F));
                        }
                        continue;
                    }
                    break;
                }
                default: p++; break;
            }
        }
        if (len + 1 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
        out[len++] = c;
    }
    if (*p == '"') p++;
    out[len] = '\0';
    *pp = p;
    return out;
}

static JsonNode *parse_value(const char **pp);

static JsonNode *parse_object(const char **pp) {
    const char *p = *pp;
    if (*p != '{') return NULL;
    p++;
    JsonNode *obj = new_node(JSON_OBJECT);
    JsonNode *tail = NULL;

    p = skip_ws(p);
    while (*p && *p != '}') {
        p = skip_ws(p);
        char *key = parse_string_raw(&p);
        if (!key) break;

        p = skip_ws(p);
        if (*p == ':') p++;
        p = skip_ws(p);

        JsonNode *val = parse_value(&p);
        if (!val) { free(key); break; }
        val->key = key;

        if (!obj->child) obj->child = val;
        else tail->next = val;
        tail = val;

        p = skip_ws(p);
        if (*p == ',') p++;
    }
    if (*p == '}') p++;
    *pp = p;
    return obj;
}

static JsonNode *parse_array(const char **pp) {
    const char *p = *pp;
    if (*p != '[') return NULL;
    p++;
    JsonNode *arr = new_node(JSON_ARRAY);
    JsonNode *tail = NULL;

    p = skip_ws(p);
    while (*p && *p != ']') {
        JsonNode *val = parse_value(&p);
        if (!val) break;

        if (!arr->child) arr->child = val;
        else tail->next = val;
        tail = val;

        p = skip_ws(p);
        if (*p == ',') p++;
        p = skip_ws(p);
    }
    if (*p == ']') p++;
    *pp = p;
    return arr;
}

static JsonNode *parse_value(const char **pp) {
    const char *p = skip_ws(*pp);

    if (*p == '"') {
        JsonNode *n = new_node(JSON_STRING);
        n->str_val = parse_string_raw(&p);
        *pp = p;
        return n;
    }
    if (*p == '{') return parse_object(pp);
    if (*p == '[') return parse_array(pp);

    if (strncmp(p, "true", 4) == 0) {
        *pp = p + 4;
        JsonNode *n = new_node(JSON_BOOL);
        n->bool_val = 1;
        return n;
    }
    if (strncmp(p, "false", 5) == 0) {
        *pp = p + 5;
        JsonNode *n = new_node(JSON_BOOL);
        n->bool_val = 0;
        return n;
    }
    if (strncmp(p, "null", 4) == 0) {
        *pp = p + 4;
        return new_node(JSON_NULL);
    }

    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        char *end;
        double val = strtod(p, &end);
        if (end > p) {
            JsonNode *n = new_node(JSON_NUMBER);
            n->num_val = val;
            *pp = end;
            return n;
        }
    }

    return NULL;
}

JsonNode *json_parse(const char *text) {
    if (!text) return NULL;
    const char *p = text;
    return parse_value(&p);
}

void json_free(JsonNode *node) {
    if (!node) return;
    json_free(node->child);
    json_free(node->next);
    free(node->key);
    free(node->str_val);
    free(node);
}

JsonNode *json_get(JsonNode *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT || !key) return NULL;
    for (JsonNode *c = obj->child; c; c = c->next) {
        if (c->key && strcmp(c->key, key) == 0) return c;
    }
    return NULL;
}

const char *json_get_string(JsonNode *obj, const char *key, const char *def) {
    JsonNode *n = json_get(obj, key);
    return (n && n->type == JSON_STRING && n->str_val) ? n->str_val : def;
}

int json_get_int(JsonNode *obj, const char *key, int def) {
    JsonNode *n = json_get(obj, key);
    return (n && n->type == JSON_NUMBER) ? (int)n->num_val : def;
}

double json_get_number(JsonNode *obj, const char *key, double def) {
    JsonNode *n = json_get(obj, key);
    return (n && n->type == JSON_NUMBER) ? n->num_val : def;
}

int json_get_bool(JsonNode *obj, const char *key, int def) {
    JsonNode *n = json_get(obj, key);
    return (n && n->type == JSON_BOOL) ? n->bool_val : def;
}

int json_array_size(JsonNode *arr) {
    if (!arr || arr->type != JSON_ARRAY) return 0;
    int count = 0;
    for (JsonNode *c = arr->child; c; c = c->next) count++;
    return count;
}

JsonNode *json_array_get(JsonNode *arr, int index) {
    if (!arr || arr->type != JSON_ARRAY) return NULL;
    JsonNode *c = arr->child;
    for (int i = 0; c && i < index; i++) c = c->next;
    return c;
}

JsonNode *json_new_object(void) { return new_node(JSON_OBJECT); }

JsonNode *json_new_array(void) { return new_node(JSON_ARRAY); }

void json_array_add(JsonNode *arr, JsonNode *val) {
    if (!arr || arr->type != JSON_ARRAY || !val) return;
    val->next = NULL;
    if (!arr->child) {
        arr->child = val;
    } else {
        JsonNode *tail = arr->child;
        while (tail->next) tail = tail->next;
        tail->next = val;
    }
}

JsonNode *json_new_string(const char *val) {
    JsonNode *n = new_node(JSON_STRING);
    n->str_val = val ? strdup(val) : strdup("");
    return n;
}

JsonNode *json_new_int(int val) {
    JsonNode *n = new_node(JSON_NUMBER);
    n->num_val = val;
    return n;
}

JsonNode *json_new_number(double val) {
    JsonNode *n = new_node(JSON_NUMBER);
    n->num_val = val;
    return n;
}

JsonNode *json_new_bool(int val) {
    JsonNode *n = new_node(JSON_BOOL);
    n->bool_val = val ? 1 : 0;
    return n;
}

void json_object_add(JsonNode *obj, const char *key, JsonNode *val) {
    if (!obj || obj->type != JSON_OBJECT || !val) return;
    val->key = strdup(key);
    val->next = NULL;
    if (!obj->child) {
        obj->child = val;
    } else {
        JsonNode *tail = obj->child;
        while (tail->next) tail = tail->next;
        tail->next = val;
    }
}

static void append_str(char **buf, size_t *len, size_t *cap, const char *s) {
    size_t sl = strlen(s);
    while (*len + sl + 1 > *cap) { *cap *= 2; *buf = (char *)realloc(*buf, *cap); }
    memcpy(*buf + *len, s, sl);
    *len += sl;
    (*buf)[*len] = '\0';
}

static void append_char(char **buf, size_t *len, size_t *cap, char c) {
    if (*len + 2 > *cap) { *cap *= 2; *buf = (char *)realloc(*buf, *cap); }
    (*buf)[(*len)++] = c;
    (*buf)[*len] = '\0';
}

static void serialize_string(char **buf, size_t *len, size_t *cap, const char *s) {
    append_char(buf, len, cap, '"');
    if (s) {
        for (const char *p = s; *p; p++) {
            switch (*p) {
                case '"':  append_str(buf, len, cap, "\\\""); break;
                case '\\': append_str(buf, len, cap, "\\\\"); break;
                case '\n': append_str(buf, len, cap, "\\n"); break;
                case '\r': append_str(buf, len, cap, "\\r"); break;
                case '\t': append_str(buf, len, cap, "\\t"); break;
                default:
                    if ((unsigned char)*p < 0x20) {
                        char esc[8];
                        snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*p);
                        append_str(buf, len, cap, esc);
                    } else {
                        append_char(buf, len, cap, *p);
                    }
            }
        }
    }
    append_char(buf, len, cap, '"');
}

static void serialize_node(JsonNode *n, char **buf, size_t *len, size_t *cap) {
    if (!n) { append_str(buf, len, cap, "null"); return; }

    switch (n->type) {
        case JSON_NULL:
            append_str(buf, len, cap, "null");
            break;
        case JSON_BOOL:
            append_str(buf, len, cap, n->bool_val ? "true" : "false");
            break;
        case JSON_NUMBER: {
            char tmp[64];
            if (n->num_val == (int)n->num_val)
                snprintf(tmp, sizeof(tmp), "%d", (int)n->num_val);
            else
                snprintf(tmp, sizeof(tmp), "%.6g", n->num_val);
            append_str(buf, len, cap, tmp);
            break;
        }
        case JSON_STRING:
            serialize_string(buf, len, cap, n->str_val);
            break;
        case JSON_OBJECT: {
            append_char(buf, len, cap, '{');
            int first = 1;
            for (JsonNode *c = n->child; c; c = c->next) {
                if (!first) append_char(buf, len, cap, ',');
                serialize_string(buf, len, cap, c->key ? c->key : "");
                append_char(buf, len, cap, ':');
                serialize_node(c, buf, len, cap);
                first = 0;
            }
            append_char(buf, len, cap, '}');
            break;
        }
        case JSON_ARRAY: {
            append_char(buf, len, cap, '[');
            int first = 1;
            for (JsonNode *c = n->child; c; c = c->next) {
                if (!first) append_char(buf, len, cap, ',');
                serialize_node(c, buf, len, cap);
                first = 0;
            }
            append_char(buf, len, cap, ']');
            break;
        }
    }
}

char *json_serialize(JsonNode *node) {
    size_t cap = 256, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';
    serialize_node(node, &buf, &len, &cap);
    return buf;
}
