/* SPDX-License-Identifier: MIT
 *
 * kiln_dict.c — see kiln_dict.h for the model.
 */

#include "kiln_dict.h"

#include <libdragon.h>
#include <string.h>

#define KEY_TABLE_SIZE 256
#define STRING_TABLE_SIZE 256
#define STRING_BYTES (KEY_TABLE_SIZE * 32)

static char      g_key_table[KEY_TABLE_SIZE][32];
static uint16_t  g_key_count;

static char      g_str_table[STRING_TABLE_SIZE][32];
static uint16_t  g_str_count;

/* First-use init. Module-global tables are allocated once and never freed. */
static void ensure_tables(void)
{
    if (g_key_count == 0) {
        g_key_table[0][0] = '\0';
        g_key_count = 1;
    }
    if (g_str_count == 0) {
        g_str_table[0][0] = '\0';
        g_str_count = 1;
    }
}

/* Intern a key string into the key table, returning its id. */
static uint16_t intern_key(const char *key)
{
    ensure_tables();
    for (uint16_t i = 1; i < g_key_count; i++) {
        if (strcmp(g_key_table[i], key) == 0) return i;
    }
    if (g_key_count >= KEY_TABLE_SIZE) {
        debugf("kiln_dict: key table full, dropping key '%s'\n", key);
        return 0;
    }
    size_t n = strlen(key);
    if (n >= 31) n = 31;
    memcpy(g_key_table[g_key_count], key, n);
    g_key_table[g_key_count][n] = '\0';
    return g_key_count++;
}

/* Look up a key id without interning. Returns 0 if the key is not in the
 * table, which is the same id intern_key returns for a table-full miss.
 * This prevents get_* calls from filling the intern table on every miss. */
static uint16_t lookup_key(const char *key)
{
    ensure_tables();
    for (uint16_t i = 1; i < g_key_count; i++) {
        if (strcmp(g_key_table[i], key) == 0) return i;
    }
    return 0;
}

/* Intern a value string into the string table, returning its id. */
static uint16_t intern_str(const char *str)
{
    ensure_tables();
    if (str == NULL || str[0] == '\0') return 0;
    for (uint16_t i = 1; i < g_str_count; i++) {
        if (strcmp(g_str_table[i], str) == 0) return i;
    }
    if (g_str_count >= STRING_TABLE_SIZE) {
        debugf("kiln_dict: string table full, dropping string '%s'\n", str);
        return 0;
    }
    size_t n = strlen(str);
    if (n >= 31) n = 31;
    memcpy(g_str_table[g_str_count], str, n);
    g_str_table[g_str_count][n] = '\0';
    return g_str_count++;
}

static const char *str_by_id(uint16_t id)
{
    if (id == 0 || id >= g_str_count) return "";
    return g_str_table[id];
}

static int find_key(const KilnDict *d, uint16_t key_id)
{
    for (int i = 0; i < d->count; i++) {
        if (d->entries[i].key_id == key_id) return i;
    }
    return -1;
}

static int find_or_append(KilnDict *d, uint16_t key_id, KilnDictType type)
{
    int idx = find_key(d, key_id);
    if (idx >= 0) {
        if (d->entries[idx].type != (uint8_t)type) {
            debugf("kiln_dict: key '%s' overwritten with different type\n",
                   g_key_table[key_id]);
        }
        return idx;
    }
    if (d->count >= KILN_DICT_MAX_KEYS) {
        debugf("kiln_dict: dict full, cannot add key '%s'\n", g_key_table[key_id]);
        return -1;
    }
    idx = d->count++;
    d->entries[idx].key_id = key_id;
    d->entries[idx].type   = (uint8_t)type;
    return idx;
}

void kiln_dict_init(KilnDict *d)
{
    d->count = 0;
    for (int i = 0; i < KILN_DICT_MAX_KEYS; i++) {
        d->entries[i].key_id = 0;
        d->entries[i].type = 0;
        d->entries[i].u.i = 0;
    }
}

void kiln_dict_set_int(KilnDict *d, const char *key, int v)
{
    uint16_t kid = intern_key(key);
    int idx = find_or_append(d, kid, KILN_DICT_INT);
    if (idx >= 0) d->entries[idx].u.i = v;
}

void kiln_dict_set_float(KilnDict *d, const char *key, float v)
{
    uint16_t kid = intern_key(key);
    int idx = find_or_append(d, kid, KILN_DICT_FLOAT);
    if (idx >= 0) d->entries[idx].u.f = v;
}

void kiln_dict_set_vec3(KilnDict *d, const char *key, fm_vec3_t v)
{
    uint16_t kid = intern_key(key);
    int idx = find_or_append(d, kid, KILN_DICT_VEC3);
    if (idx >= 0) d->entries[idx].u.v = v;
}

void kiln_dict_set_str(KilnDict *d, const char *key, const char *v)
{
    uint16_t kid = intern_key(key);
    int idx = find_or_append(d, kid, KILN_DICT_STRING);
    if (idx >= 0) d->entries[idx].u.s_id = intern_str(v);
}

int kiln_dict_get_int(const KilnDict *d, const char *key, int def)
{
    uint16_t kid = lookup_key(key);
    int idx = find_key(d, kid);
    if (idx < 0 || d->entries[idx].type != KILN_DICT_INT) return def;
    return d->entries[idx].u.i;
}

float kiln_dict_get_float(const KilnDict *d, const char *key, float def)
{
    uint16_t kid = lookup_key(key);
    int idx = find_key(d, kid);
    if (idx < 0 || d->entries[idx].type != KILN_DICT_FLOAT) return def;
    return d->entries[idx].u.f;
}

fm_vec3_t kiln_dict_get_vec3(const KilnDict *d, const char *key, fm_vec3_t def)
{
    uint16_t kid = lookup_key(key);
    int idx = find_key(d, kid);
    if (idx < 0 || d->entries[idx].type != KILN_DICT_VEC3) return def;
    return d->entries[idx].u.v;
}

const char *kiln_dict_get_str(const KilnDict *d, const char *key, const char *def)
{
    uint16_t kid = lookup_key(key);
    int idx = find_key(d, kid);
    if (idx < 0 || d->entries[idx].type != KILN_DICT_STRING) return def;
    return str_by_id(d->entries[idx].u.s_id);
}

int kiln_dict_has_int(const KilnDict *d, const char *key)
{
    uint16_t kid = lookup_key(key);
    int idx = find_key(d, kid);
    return idx >= 0 && d->entries[idx].type == KILN_DICT_INT;
}

int kiln_dict_has_float(const KilnDict *d, const char *key)
{
    uint16_t kid = lookup_key(key);
    int idx = find_key(d, kid);
    return idx >= 0 && d->entries[idx].type == KILN_DICT_FLOAT;
}

int kiln_dict_has_vec3(const KilnDict *d, const char *key)
{
    uint16_t kid = lookup_key(key);
    int idx = find_key(d, kid);
    return idx >= 0 && d->entries[idx].type == KILN_DICT_VEC3;
}

int kiln_dict_has_str(const KilnDict *d, const char *key)
{
    uint16_t kid = lookup_key(key);
    int idx = find_key(d, kid);
    return idx >= 0 && d->entries[idx].type == KILN_DICT_STRING;
}

int kiln_dict_parse_int(const char *v)
{
    if (!v) return 0;
    return (int)atoi(v);
}

float kiln_dict_parse_float(const char *v)
{
    if (!v) return 0.0f;
    return (float)atof(v);
}

fm_vec3_t kiln_dict_parse_vec3(const char *v)
{
    fm_vec3_t r = {{ 0, 0, 0 }};
    if (!v) return r;
    /* TrenchBroom: "x y z" with any whitespace. */
    float x, y, z;
    if (sscanf(v, "%f %f %f", &x, &y, &z) == 3) {
        r.v[0] = x; r.v[1] = y; r.v[2] = z;
    }
    return r;
}

void kiln_dict_set_auto(KilnDict *d, const char *key, const char *v)
{
    if (!v || v[0] == '\0') {
        kiln_dict_set_str(d, key, v);
        return;
    }

    /* Try three floats first. */
    float x, y, z;
    char *endptr;

    x = (float)strtod(v, &endptr);
    if (endptr == v) {
        kiln_dict_set_str(d, key, v);
        return;
    }
    /* strtod accepts "inf"/"infinity"/"nan" as a prefix even when not
     * followed by a valid number (e.g. "info_player_start" parses as
     * +Inf with endptr past "inf"). The int cast below would trap on
     * +Inf. -ffast-math folds isfinite() to true (it assumes no inf/nan),
     * so inspect the IEEE 754 bits directly: exponent all-ones = inf/nan. */
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    if ((bits & 0x7F800000u) == 0x7F800000u) {
        kiln_dict_set_str(d, key, v);
        return;
    }
    int is_float0 = (strchr(v, '.') != NULL);

    const char *p1 = endptr;
    y = (float)strtod(p1, &endptr);
    if (endptr == p1) {
        /* Exactly one number. */
        if (is_float0) kiln_dict_set_float(d, key, x);
        else           kiln_dict_set_int(d, key, (int)x);
        return;
    }

    const char *p2 = endptr;
    z = (float)strtod(p2, &endptr);
    if (endptr == p2) {
        /* Two numbers — ambiguous; store as string. */
        kiln_dict_set_str(d, key, v);
        return;
    }

    /* If there's extra non-whitespace after the third number, it's a
     * string that starts with a number (e.g. "1 red"); store as string. */
    const char *tail = endptr;
    while (*tail == ' ' || *tail == '\t' || *tail == '\n' || *tail == '\r')
        tail++;
    if (*tail != '\0') {
        kiln_dict_set_str(d, key, v);
        return;
    }

    kiln_dict_set_vec3(d, key, (fm_vec3_t){{ x, y, z }});
}