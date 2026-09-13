/* SPDX-License-Identifier: MIT
 *
 * camlint — kiln_camlint over a camera shot written as JSON.
 *
 *     camlint <shot.json> [--json]
 *
 * kiln_camlint has run in two places: natively inside nix checks against tables
 * compiled into the check, and on console. Neither lets someone ask "is THIS
 * shot valid" about a shot that exists as data — one exported from Forge's CAM
 * mode, drafted by an agent, or pasted from a cutscene table — and CLAUDE.md
 * has long pointed at a `./dev cine-lint` that did not exist. This is it: the
 * engine's own validator, linked natively, reading a file.
 *
 * Input: one shot, or {"shots": [ ... ]}, each
 *     {"name": "open", "duration": 9, "near_z": 4, "far_z": 360, "loop": false,
 *      "bounds": {"mins": [x,y,z], "maxs": [x,y,z]},          (optional)
 *      "keys": [{"t": 0, "eye": [x,y,z], "look": [x,y,z]}, ...]}
 *
 * Output: a human report, or with --json a tools/schema/report.schema.json
 * report. Exit 0 when no shot has a hard failure, 1 when one does, 2 when the
 * input cannot be read. NOTE_ measurements never change the exit code — see
 * kiln_camlint.h for why.
 *
 * The JSON reader below handles exactly JSON and nothing more, into a fixed
 * node pool; a shot file is small, and a parser that fails loudly on anything
 * it does not understand is the right one for a validator.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kiln_camlint.h"

/* ── a small JSON reader ──────────────────────────────────────────────── */
enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct {
    int type;
    double num;
    const char *str;     /* J_STR: NUL-terminated, owned by the pool buffer */
    int first, next;     /* children list (J_ARR/J_OBJ), sibling link       */
    const char *key;     /* when a member of an object                      */
} JNode;

#define MAX_NODES 65536
static JNode g_nodes[MAX_NODES];
static int g_count;
static char *g_p;
static const char *g_err;

static int node(int type)
{
    if (g_count >= MAX_NODES) { g_err = "too many JSON values"; return -1; }
    g_nodes[g_count] = (JNode){ .type = type, .first = -1, .next = -1 };
    return g_count++;
}

static void ws(void) { while (*g_p && isspace((unsigned char)*g_p)) g_p++; }

static int parse_value(void);

static char *parse_string(void)
{
    if (*g_p != '"') { g_err = "expected a string"; return NULL; }
    char *out = ++g_p, *w = g_p;
    while (*g_p && *g_p != '"') {
        if (*g_p == '\\') {
            g_p++;
            switch (*g_p) {
            case 'n': *w++ = '\n'; break;
            case 't': *w++ = '\t'; break;
            case 'r': *w++ = '\r'; break;
            case 'b': *w++ = '\b'; break;
            case 'f': *w++ = '\f'; break;
            case 'u': /* names are ASCII here; keep the escape visible rather than guess */
                *w++ = '?'; for (int i = 0; i < 4 && g_p[1]; i++) g_p++; break;
            case '\0': g_err = "unterminated string"; return NULL;
            default: *w++ = *g_p; break;
            }
            g_p++;
        } else {
            *w++ = *g_p++;
        }
    }
    if (*g_p != '"') { g_err = "unterminated string"; return NULL; }
    g_p++;
    *w = '\0';
    return out;
}

static int parse_container(int type, char close)
{
    int n = node(type), last = -1;
    if (n < 0) return -1;
    g_p++;
    ws();
    if (*g_p == close) { g_p++; return n; }
    for (;;) {
        ws();
        char *key = NULL;
        if (type == J_OBJ) {
            key = parse_string();
            if (!key) return -1;
            ws();
            if (*g_p != ':') { g_err = "expected ':'"; return -1; }
            g_p++;
        }
        int c = parse_value();
        if (c < 0) return -1;
        g_nodes[c].key = key;
        if (last < 0) g_nodes[n].first = c; else g_nodes[last].next = c;
        last = c;
        ws();
        if (*g_p == ',') { g_p++; continue; }
        if (*g_p == close) { g_p++; return n; }
        g_err = type == J_OBJ ? "expected ',' or '}'" : "expected ',' or ']'";
        return -1;
    }
}

static int parse_value(void)
{
    ws();
    if (*g_p == '{') return parse_container(J_OBJ, '}');
    if (*g_p == '[') return parse_container(J_ARR, ']');
    if (*g_p == '"') {
        int n = node(J_STR);
        if (n < 0) return -1;
        const char *s = parse_string();
        if (!s) return -1;
        g_nodes[n].str = s;
        return n;
    }
    if (!strncmp(g_p, "true", 4) || !strncmp(g_p, "false", 5)) {
        int n = node(J_BOOL);
        if (n < 0) return -1;
        g_nodes[n].num = *g_p == 't';
        g_p += *g_p == 't' ? 4 : 5;
        return n;
    }
    if (!strncmp(g_p, "null", 4)) { g_p += 4; return node(J_NULL); }
    char *end;
    double v = strtod(g_p, &end);
    if (end == g_p) { g_err = "unexpected character"; return -1; }
    int n = node(J_NUM);
    if (n < 0) return -1;
    g_nodes[n].num = v;
    g_p = end;
    return n;
}

static int member(int obj, const char *key)
{
    if (obj < 0 || g_nodes[obj].type != J_OBJ) return -1;
    for (int c = g_nodes[obj].first; c >= 0; c = g_nodes[c].next)
        if (g_nodes[c].key && !strcmp(g_nodes[c].key, key)) return c;
    return -1;
}

static int count(int arr)
{
    int k = 0;
    for (int c = g_nodes[arr].first; c >= 0; c = g_nodes[c].next) k++;
    return k;
}

/* ── shots ────────────────────────────────────────────────────────────── */
static const char *g_input_err;

static float num_or(int obj, const char *key, float dflt, int required)
{
    int n = member(obj, key);
    if (n < 0) {
        if (required && !g_input_err) g_input_err = key;
        return dflt;
    }
    if (g_nodes[n].type == J_BOOL) return (float)g_nodes[n].num;
    if (g_nodes[n].type != J_NUM) { if (!g_input_err) g_input_err = key; return dflt; }
    return (float)g_nodes[n].num;
}

static int vec3(int arr, fm_vec3_t *out)
{
    if (arr < 0 || g_nodes[arr].type != J_ARR || count(arr) != 3) return 0;
    int i = 0;
    for (int c = g_nodes[arr].first; c >= 0; c = g_nodes[c].next, i++) {
        if (g_nodes[c].type != J_NUM) return 0;
        out->v[i] = (float)g_nodes[c].num;
    }
    return 1;
}

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fprintf(f, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", *s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

static void code_of(const char *prefix, const char *name, char *out, size_t n)
{
    /* "time-order" -> "ERR_TIME_ORDER" */
    size_t k = (size_t)snprintf(out, n, "%s", prefix);
    for (; *name && k + 1 < n; name++)
        out[k++] = (*name == '-' || *name == ' ') ? '_' : (char)toupper((unsigned char)*name);
    out[k] = '\0';
}

typedef struct { int first; } Emit;

static void finding(FILE *f, Emit *e, const char *code, const char *msg, const char *where)
{
    fprintf(f, "%s\n  {\"code\": ", e->first ? "" : ",");
    json_str(f, code);
    fprintf(f, ", \"msg\": ");
    json_str(f, msg);
    fprintf(f, ", \"where\": ");
    json_str(f, where);
    fputc('}', f);
    e->first = 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int as_json = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) as_json = 1;
        else if (!path) path = argv[i];
        else path = NULL, i = argc;
    }
    if (!path) {
        fprintf(stderr, "usage: camlint <shot.json> [--json]\n");
        return 2;
    }

    FILE *in = fopen(path, "rb");
    if (!in) { perror(path); return 2; }
    static char buf[1 << 22];
    size_t len = fread(buf, 1, sizeof buf - 1, in);
    fclose(in);
    buf[len] = '\0';
    g_p = buf;
    int root = parse_value();
    ws();
    if (root < 0 || *g_p) {
        fprintf(stderr, "camlint: %s: %s\n", path, g_err ? g_err : "trailing data after the JSON value");
        return 2;
    }

    /* One shot, or {"shots": [...]}. */
    int shots[256], nshots = 0;
    int list = member(root, "shots");
    if (list >= 0 && g_nodes[list].type == J_ARR) {
        for (int c = g_nodes[list].first; c >= 0 && nshots < 256; c = g_nodes[c].next) shots[nshots++] = c;
    } else {
        shots[nshots++] = root;
    }

    static char errbuf[1 << 16], notebuf[1 << 16], metbuf[1 << 16];
    FILE *errs = fmemopen(errbuf, sizeof errbuf, "w");
    FILE *notes = fmemopen(notebuf, sizeof notebuf, "w");
    FILE *mets = fmemopen(metbuf, sizeof metbuf, "w");
    Emit ee = { 1 }, ne = { 1 };
    int bad = 0;

    for (int s = 0; s < nshots; s++) {
        int shot = shots[s];
        int nm = member(shot, "name");
        char name[64];
        if (nm >= 0 && g_nodes[nm].type == J_STR) snprintf(name, sizeof name, "%s", g_nodes[nm].str);
        else snprintf(name, sizeof name, "shot %d", s);

        g_input_err = NULL;
        int keys_n = member(shot, "keys");
        static KilnCamKey keys[1024];
        int kc = 0;
        if (keys_n < 0 || g_nodes[keys_n].type != J_ARR) g_input_err = "keys";
        else {
            for (int c = g_nodes[keys_n].first; c >= 0 && kc < 1024; c = g_nodes[c].next, kc++) {
                keys[kc].t = num_or(c, "t", 0, 1);
                if (!vec3(member(c, "eye"), &keys[kc].eye) || !vec3(member(c, "look"), &keys[kc].look))
                    if (!g_input_err) g_input_err = "eye/look";
            }
        }
        KilnCamShot cs = {
            .keys = keys, .key_count = kc,
            .duration = num_or(shot, "duration", 0, 1),
            .near_z = num_or(shot, "near_z", 0, 1),
            .far_z = num_or(shot, "far_z", 0, 1),
            .loop = (int)num_or(shot, "loop", 0, 0),
        };
        KilnCamBounds bounds = { .valid = 0 };
        int b = member(shot, "bounds");
        if (b >= 0) bounds.valid = vec3(member(b, "mins"), &bounds.mins) && vec3(member(b, "maxs"), &bounds.maxs);
        if (g_input_err) {
            fprintf(stderr, "camlint: %s: %s: missing or malformed '%s'\n", path, name, g_input_err);
            return 2;
        }

        KilnCamReport rep;
        uint32_t err = kiln_camlint(&cs, bounds.valid ? &bounds : NULL, &rep);
        bad |= err != 0;

        if (!as_json) {
            printf("%-16s %s", name, err ? "FAIL" : "ok  ");
            for (int bit = 0; bit < 32; bit++)
                if (err & (1u << bit)) printf("  %s", kiln_camlint_err_name(1u << bit));
            for (int bit = 0; bit < 32; bit++)
                if (rep.note & (1u << bit)) printf("  (%s)", kiln_camlint_note_name(1u << bit));
            printf("\n                 keys %d  overshoot %.2f  speed ratio %.2f  subject %.1f  tail %.2fs\n",
                   kc, rep.overshoot, rep.speed_ratio, rep.subject_dist, rep.tail);
            continue;
        }

        char code[64], msg[256], where[128];
        for (int bit = 0; bit < 32; bit++) {
            if (!(err & (1u << bit))) continue;
            code_of("ERR_", kiln_camlint_err_name(1u << bit), code, sizeof code);
            snprintf(msg, sizeof msg, "%s: %s", name, kiln_camlint_err_name(1u << bit));
            if (rep.bad_key >= 0) snprintf(where, sizeof where, "%s key %d", name, rep.bad_key);
            else snprintf(where, sizeof where, "%s", name);
            finding(errs, &ee, code, msg, where);
        }
        for (int bit = 0; bit < 32; bit++) {
            if (!(rep.note & (1u << bit))) continue;
            code_of("NOTE_", kiln_camlint_note_name(1u << bit), code, sizeof code);
            snprintf(msg, sizeof msg, "%s: %s", name, kiln_camlint_note_name(1u << bit));
            snprintf(where, sizeof where, "%s", name);
            finding(notes, &ne, code, msg, where);
        }
        fprintf(mets, "%s\n  ", s ? "," : "");
        json_str(mets, name);
        fprintf(mets, ": {\"keys\": %d, \"overshoot\": %.4f, \"speed_ratio\": %.4f, \"subject_dist\": %.4f, "
                      "\"tail\": %.4f, \"bad_key\": %d, \"outside_key\": %d}",
                kc, rep.overshoot, rep.speed_ratio, rep.subject_dist, rep.tail, rep.bad_key, rep.outside_key);
    }

    fclose(errs);
    fclose(notes);
    fclose(mets);
    if (as_json) {
        printf("{\"tool\": \"camlint\", \"version\": 1, \"ok\": %s, \"subject\": ", bad ? "false" : "true");
        json_str(stdout, path);
        printf(",\n\"errors\": [%s\n],\n\"notes\": [%s\n],\n\"metrics\": {%s\n}}\n", errbuf, notebuf, metbuf);
    }
    return bad ? 1 : 0;
}
