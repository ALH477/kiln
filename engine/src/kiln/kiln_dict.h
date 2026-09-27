/* SPDX-License-Identifier: MIT
 *
 * kiln_dict.h — the idDict analogue: small key/value stores for actor and map
 * entity spawn arguments. See CLAUDE.md's Phase C notes for what was and
 * wasn't carried over from id Tech 4's idDict.
 *
 * ── Flat, typed, interned ──────────────────────────────────────────────
 * Doom 3's idDict is a flat key→string container used for entity spawn args,
 * map entity epairs, and primitive epairs. We keep the "flat" part but add
 * typed accessors so game code doesn't reparse strings every spawn. Each
 * `FigDict` holds up to FIG_DICT_MAX_KEYS (16) entries of type int, float,
 * vec3, or string; keys are interned into a module-global string table so
 * per-instance storage is an index, not a duplicated string pointer.
 *
 * ── Why a global string table ───────────────────────────────────────────
 * Storing a `const char*` per entry would make each spawn args block pay for
 * the string bytes (plus the source file's lifetime issues). Instead the
 * module owns one fixed string table allocated at first use (256 entries,
 * ~4 KB). Keys are compared with strcmp only at insert; after that every dict
 * stores a `uint16_t key_id` and lookups are O(keys-in-dict) comparisons of
 * those ids. This is the right trade for N64: bounded RAM, one-time cost.
 *
 * ── Why FigDict is on the SPAWN TEMPLATE, not the live actor ─────────────
 * Spawn args are read once in the actor's init callback and copied into the
 * type's own state struct if needed. Keeping the dict on `FigRoomSpawn` (see
 * kiln_room.h) avoids the overhead on every live actor instance in a room full
 * of identical enemies. The profile's init function is the only code that
 * should call `fig_dict_get_*`; update/draw should not hold a dict pointer.
 */
#ifndef FIG_DICT_H
#define FIG_DICT_H

#include <stdint.h>
#include <t3d/t3dmath.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FIG_DICT_MAX_KEYS 16

/** Value types. Stored as a uint8_t in each entry. */
typedef enum {
    FIG_DICT_INT = 0,
    FIG_DICT_FLOAT,
    FIG_DICT_VEC3,
    FIG_DICT_STRING,
} FigDictType;

/** One key/value pair. The union is large enough to hold a vec3; smaller
 *  types leave unused bytes. That's acceptable because the key block is
 *  tiny (16 entries) and alignment is deliberately compact. */
typedef struct {
    uint16_t  key_id;
    uint8_t   type;
    uint8_t   _pad;
    union {
        int       i;
        float     f;
        fm_vec3_t v;
        uint16_t  s_id;   /**< index into the module-global string table     */
    } u;
} FigDictEntry;

/** A flat associative array. Zeroing a struct is a valid empty dict. */
typedef struct {
    uint8_t      count;
    uint8_t      _pad[3];
    FigDictEntry entries[FIG_DICT_MAX_KEYS];
} FigDict;

/** Zero the dict. Does not touch the module-global key table. */
void fig_dict_init(FigDict *d);

/** Setters. Overwrite an existing key of the SAME type or append a new one.
 *  If the dict is full, the set is a no-op (debugf) — spawn args are authored
 *  content and should fit; running out of slots is a content bug. */
void fig_dict_set_int  (FigDict *d, const char *key, int v);
void fig_dict_set_float(FigDict *d, const char *key, float v);
void fig_dict_set_vec3 (FigDict *d, const char *key, fm_vec3_t v);
void fig_dict_set_str  (FigDict *d, const char *key, const char *v);

/** Getters. `def` is returned when the key is missing or the stored type does
 *  not match the requested getter (a mismatch is a content bug; debugf). */
int         fig_dict_get_int  (const FigDict *d, const char *key, int def);
float       fig_dict_get_float(const FigDict *d, const char *key, float def);
fm_vec3_t   fig_dict_get_vec3 (const FigDict *d, const char *key, fm_vec3_t def);
const char* fig_dict_get_str  (const FigDict *d, const char *key, const char *def);

/** Typed check: returns 1 if the key exists AND has the expected type. */
int fig_dict_has_int  (const FigDict *d, const char *key);
int fig_dict_has_float(const FigDict *d, const char *key);
int fig_dict_has_vec3 (const FigDict *d, const char *key);
int fig_dict_has_str  (const FigDict *d, const char *key);

/** Parse helpers used by fig_map. "0 0 0" → vec3; "5" → int; "5.5" → float;
 *  anything else → string. This is the mapping from Quake .map strings to
 *  typed spawn args. */
int         fig_dict_parse_int  (const char *v);
float       fig_dict_parse_float(const char *v);
fm_vec3_t   fig_dict_parse_vec3 (const char *v);

/** For map loading: set a key using the parse helpers above. If `v` looks like
 *  three numbers separated by whitespace, store a vec3; if one number with a
 *  '.', store a float; if one integer, store an int; otherwise store a string.
 *  This matches how TrenchBroom writes "origin" vs "health" vs "name". */
void fig_dict_set_auto(FigDict *d, const char *key, const char *v);

#ifdef __cplusplus
}
#endif

#endif /* FIG_DICT_H */