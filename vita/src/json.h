#ifndef SNAPVITA_JSON_H
#define SNAPVITA_JSON_H

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonNode {
    JsonType type;
    char *key;
    struct JsonNode *next;
    struct JsonNode *child;
    char *str_val;
    double num_val;
    int bool_val;
} JsonNode;

JsonNode *json_parse(const char *text);
void      json_free(JsonNode *node);

JsonNode   *json_get(JsonNode *obj, const char *key);
const char *json_get_string(JsonNode *obj, const char *key, const char *def);
int         json_get_int(JsonNode *obj, const char *key, int def);
double      json_get_number(JsonNode *obj, const char *key, double def);
int         json_get_bool(JsonNode *obj, const char *key, int def);
int         json_array_size(JsonNode *arr);
JsonNode   *json_array_get(JsonNode *arr, int index);

JsonNode *json_new_object(void);
JsonNode *json_new_string(const char *val);
JsonNode *json_new_int(int val);
JsonNode *json_new_number(double val);
JsonNode *json_new_bool(int val);
void      json_object_add(JsonNode *obj, const char *key, JsonNode *val);

char *json_serialize(JsonNode *node);

#endif /* SNAPVITA_JSON_H */
