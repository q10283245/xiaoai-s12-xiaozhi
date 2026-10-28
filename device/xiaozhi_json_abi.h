/* Small public json-c 0.12 API subset. Only opaque objects are used.
 * Reference: https://github.com/json-c/json-c/tree/json-c-0.12-20140410
 */
#ifndef XIAOZHI_JSON_ABI_H
#define XIAOZHI_JSON_ABI_H
struct json_object;
struct json_tokener;
enum json_type { json_type_null, json_type_boolean, json_type_double,
                 json_type_int, json_type_object, json_type_array, json_type_string };
extern struct json_tokener *json_tokener_new(void);
extern void json_tokener_free(struct json_tokener *);
extern struct json_object *json_tokener_parse_ex(struct json_tokener *, const char *, int);
extern int json_tokener_get_error(struct json_tokener *);
extern void json_tokener_set_flags(struct json_tokener *, int);
extern struct json_object *json_object_new_object(void);
extern struct json_object *json_object_new_string(const char *);
extern struct json_object *json_object_new_int(int);
extern void json_object_object_add(struct json_object *, const char *, struct json_object *);
extern int json_object_object_get_ex(const struct json_object *, const char *, struct json_object **);
extern int json_object_is_type(const struct json_object *, enum json_type);
extern const char *json_object_get_string(struct json_object *);
extern int json_object_get_string_len(const struct json_object *);
extern int json_object_get_int(const struct json_object *);
extern const char *json_object_to_json_string_ext(struct json_object *, int);
extern int json_object_put(struct json_object *);
#endif
