/*
 * esh: variables.
 *
 * A hash table of the visible variables; a function's `local` pushes a new
 * variable that shadows the outer one until the function returns. Arrays
 * are 1-based, as in zsh. A few specials are computed when read.
 */
#include "esh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NBUCKETS 128

static struct var *tab[NBUCKETS];
struct posargs pos;

static unsigned hash(const char *s)
{
    unsigned h = 5381;
    while (*s)
        h = h * 33 + (unsigned char)*s++;
    return h % NBUCKETS;
}

struct var *var_lookup(const char *name)
{
    for (struct var *v = tab[hash(name)]; v; v = v->next)
        if (!strcmp(v->name, name))
            return v;
    return 0;
}

static struct var *var_new(const char *name, int level)
{
    struct var *v = xmalloc(sizeof(*v));
    memset(v, 0, sizeof(*v));
    v->name = xstrdup(name);
    v->level = level;
    unsigned h = hash(name);
    /* a local shadows: insert in front and remember the outer one */
    struct var **pp = &tab[h];
    for (; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->name, name)) {
            v->shadow = *pp;
            v->next = (*pp)->next;
            *pp = v;
            return v;
        }
    v->next = tab[h];
    tab[h] = v;
    return v;
}

static void var_clear_value(struct var *v)
{
    free(v->val);
    v->val = 0;
    for (int i = 0; i < v->narr; i++)
        free(v->arr[i]);
    free(v->arr);
    v->arr = 0;
    v->narr = 0;
}

static void var_destroy(struct var *v)
{
    var_clear_value(v);
    free(v->name);
    free(v);
}

/* ---- specials ---- */

static unsigned long long rnd_state;

static const char *special_get(struct var *v)
{
    static char buf[32];
    if (!strcmp(v->name, "RANDOM")) {
        if (!rnd_state)
            rnd_state = (unsigned long long)now_ms() * 2654435761ull + getpid();
        rnd_state = rnd_state * 6364136223846793005ull + 1442695040888963407ull;
        snprintf(buf, sizeof(buf), "%d", (int)((rnd_state >> 33) % 32768));
        return buf;
    }
    if (!strcmp(v->name, "SECONDS")) {
        snprintf(buf, sizeof(buf), "%lld", (now_ms() - sh.start_ms) / 1000);
        return buf;
    }
    if (!strcmp(v->name, "EPOCHSECONDS")) {
        snprintf(buf, sizeof(buf), "%lld", (long long)time(0));
        return buf;
    }
    if (!strcmp(v->name, "LINENO")) {
        snprintf(buf, sizeof(buf), "%d", sh.lineno);
        return buf;
    }
    if (!strcmp(v->name, "COLUMNS")) {
        snprintf(buf, sizeof(buf), "%d", sh.cols);
        return buf;
    }
    if (!strcmp(v->name, "LINES")) {
        snprintf(buf, sizeof(buf), "%d", sh.rows);
        return buf;
    }
    return v->val;
}

static void special_set(struct var *v, const char *val)
{
    if (!strcmp(v->name, "RANDOM"))
        rnd_state = strtol(val, 0, 10) + 1;
    else if (!strcmp(v->name, "SECONDS"))
        sh.start_ms = now_ms() - strtol(val, 0, 10) * 1000;
    else if (!strcmp(v->name, "COLUMNS") && atoi(val) > 0)
        sh.cols = atoi(val);
    else if (!strcmp(v->name, "LINES") && atoi(val) > 0)
        sh.rows = atoi(val);
}

/* ---- get and set ---- */

const char *var_get(const char *name)
{
    struct var *v = var_lookup(name);
    if (!v || (v->flags & V_UNSET))
        return 0;
    if (v->flags & V_SPECIAL)
        return special_get(v);
    if (v->flags & V_ARRAY)
        return v->narr ? v->arr[0] : "";
    return v->val ? v->val : "";
}

static int check_ro(struct var *v)
{
    if (v && (v->flags & V_READONLY)) {
        sh_error("read-only variable: %s", v->name);
        return -1;
    }
    return 0;
}

static char *convert(struct var *v, const char *val)
{
    char *s = xstrdup(val);
    if (v->flags & V_INTEGER) {
        long long n;
        if (arith_eval(val, &n) == 0) {
            free(s);
            char buf[24];
            snprintf(buf, sizeof(buf), "%lld", n);
            s = xstrdup(buf);
        }
    }
    for (char *p = s; *p; p++) {
        if (v->flags & V_LOWER)
            *p = to_lower(*p);
        else if (v->flags & V_UPPER)
            *p = to_upper(*p);
    }
    return s;
}

int var_set(const char *name, const char *val, int flags)
{
    struct var *v = var_lookup(name);
    if (check_ro(v) < 0)
        return -1;
    if (!v)
        v = var_new(name, 0);
    if (v->flags & V_SPECIAL) {
        special_set(v, val);
    }
    if (v->flags & V_ARRAY) {
        var_clear_value(v);
        v->flags &= ~V_ARRAY;
    }
    char *s = convert(v, val);
    free(v->val);
    v->val = s;
    v->flags = (v->flags & ~V_UNSET) | flags;
    if (!strcmp(name, "PATH"))
        path_rehash();
    return 0;
}

int var_set_array(const char *name, char **elems, int n, int append)
{
    struct var *v = var_lookup(name);
    if (check_ro(v) < 0)
        return -1;
    if (!v)
        v = var_new(name, 0);
    if (!(v->flags & V_ARRAY)) {
        char *old = append && v->val && !(v->flags & V_UNSET) ? xstrdup(v->val) : 0;
        var_clear_value(v);
        v->flags |= V_ARRAY;
        if (old) {
            v->arr = xmalloc(sizeof(char *));
            v->arr[0] = old;
            v->narr = 1;
        }
    } else if (!append) {
        var_clear_value(v);
    }
    v->flags &= ~V_UNSET;
    v->arr = xrealloc(v->arr, (v->narr + n + 1) * sizeof(char *));
    for (int i = 0; i < n; i++)
        v->arr[v->narr++] = convert(v, elems[i]);
    v->arr[v->narr] = 0;
    return 0;
}

int var_set_elem(const char *name, long idx, const char *val)
{
    struct var *v = var_lookup(name);
    if (check_ro(v) < 0)
        return -1;
    if (!v || !(v->flags & V_ARRAY)) {
        if (var_set_array(name, 0, 0, v && v->val && !(v->flags & V_UNSET)) < 0)
            return -1;
        v = var_lookup(name);
    }
    if (idx < 0)
        idx += v->narr + 1;
    if (idx < 1) {
        sh_error("%s: subscript out of range", name);
        return -1;
    }
    if (idx > v->narr) {
        v->arr = xrealloc(v->arr, (idx + 1) * sizeof(char *));
        while (v->narr < idx)
            v->arr[v->narr++] = xstrdup("");
        v->arr[v->narr] = 0;
    }
    free(v->arr[idx - 1]);
    v->arr[idx - 1] = convert(v, val);
    return 0;
}

int var_unset(const char *name)
{
    struct var *v = var_lookup(name);
    if (!v)
        return 0;
    if (check_ro(v) < 0)
        return -1;
    if (v->level > 0 && v->level == sh.func_depth) {
        /* a local: keep the slot (it still shadows) but mark it unset */
        var_clear_value(v);
        v->flags = V_UNSET;
        return 0;
    }
    unsigned h = hash(name);
    for (struct var **pp = &tab[h]; *pp; pp = &(*pp)->next)
        if (*pp == v) {
            if (v->shadow) {
                /* unsetting an outer variable through a dead local: drop just this one */
                v->shadow->next = v->next;
                *pp = v->shadow;
            } else {
                *pp = v->next;
            }
            break;
        }
    var_destroy(v);
    if (!strcmp(name, "PATH"))
        path_rehash();
    return 0;
}

int var_local(const char *name)
{
    struct var *v = var_lookup(name);
    if (v && v->level == sh.func_depth)
        return 0;
    struct var *n = var_new(name, sh.func_depth);
    n->flags = V_UNSET;
    if (v) {
        /* inherit export status; specials stay special */
        n->flags |= v->flags & (V_EXPORT | V_SPECIAL);
    }
    return 0;
}

void vars_pop_level(int level)
{
    for (int h = 0; h < NBUCKETS; h++) {
        struct var **pp = &tab[h];
        while (*pp) {
            struct var *v = *pp;
            if (v->level >= level && v->level > 0) {
                if (v->shadow) {
                    v->shadow->next = v->next;
                    *pp = v->shadow;
                } else {
                    *pp = v->next;
                }
                var_destroy(v);
                continue;
            }
            pp = &v->next;
        }
    }
}

void var_export(const char *name)
{
    struct var *v = var_lookup(name);
    if (!v) {
        v = var_new(name, 0);
        v->flags = V_UNSET;
    }
    v->flags |= V_EXPORT;
}

/* ---- environment ---- */

char **var_environ(void)
{
    struct strv env = { 0 };
    for (int h = 0; h < NBUCKETS; h++)
        for (struct var *v = tab[h]; v; v = v->next) {
            if (!(v->flags & V_EXPORT) || (v->flags & V_UNSET))
                continue;
            struct sb b = { 0 };
            sb_puts(&b, v->name);
            sb_putc(&b, '=');
            if (v->flags & V_ARRAY) {
                for (int i = 0; i < v->narr; i++) {
                    if (i)
                        sb_putc(&b, ':');
                    sb_puts(&b, v->arr[i]);
                }
            } else {
                const char *val = var_get(v->name);
                sb_puts(&b, val ? val : "");
            }
            strv_push(&env, sb_adup(&b));
            sb_free(&b);
        }
    if (!env.v)
        strv_push(&env, 0), env.n = 0;
    return env.v;
}

void vars_list(int (*filter)(struct var *), int export_form)
{
    struct strv names = { 0 };
    for (int h = 0; h < NBUCKETS; h++)
        for (struct var *v = tab[h]; v; v = v->next)
            if (!(v->flags & V_UNSET) && (!filter || filter(v)))
                strv_push(&names, v->name);
    sort_strs(names.v, names.n);
    for (int i = 0; i < names.n; i++) {
        struct var *v = var_lookup(names.v[i]);
        if (export_form)
            printf("%s ", (v->flags & V_READONLY) ? "typeset -r" : "export");
        if (v->flags & V_ARRAY) {
            printf("%s=(", v->name);
            for (int k = 0; k < v->narr; k++)
                printf("%s%s", k ? " " : "", quote_word(v->arr[k]));
            printf(")\n");
        } else {
            const char *val = var_get(v->name);
            printf("%s=%s\n", v->name, quote_word(val ? val : ""));
        }
    }
}

int var_names(const char *prefix, struct strv *out)
{
    int n = 0, pl = strlen(prefix);
    for (int h = 0; h < NBUCKETS; h++)
        for (struct var *v = tab[h]; v; v = v->next)
            if (!(v->flags & V_UNSET) && !strncmp(v->name, prefix, pl)) {
                strv_push(out, astrdup(v->name));
                n++;
            }
    return n;
}

/* ---- positional parameters ---- */

void pos_set(char **v, int n)
{
    char **nv = xmalloc((n + 1) * sizeof(char *));
    for (int i = 0; i < n; i++)
        nv[i] = xstrdup(v[i]);
    nv[n] = 0;
    for (int i = 0; i < pos.n; i++)
        free(pos.v[i]);
    free(pos.v);
    pos.v = nv;
    pos.n = n;
}

struct posargs pos_save(void)
{
    struct posargs p = pos;
    pos.v = 0;
    pos.n = 0;
    pos_set(p.v, p.n);
    return p;
}

void pos_restore(struct posargs p)
{
    for (int i = 0; i < pos.n; i++)
        free(pos.v[i]);
    free(pos.v);
    pos = p;
}

void vars_init(char **envp)
{
    for (char **e = envp; e && *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq || eq == *e)
            continue;
        char *name = xstrndup(*e, eq - *e);
        if (is_name(name))
            var_set(name, eq + 1, V_EXPORT);
        free(name);
    }
    static const char *const specials[] = { "RANDOM", "SECONDS", "EPOCHSECONDS", "LINENO",
                                            "COLUMNS", "LINES", 0 };
    for (int i = 0; specials[i]; i++) {
        struct var *v = var_lookup(specials[i]);
        if (!v)
            v = var_new(specials[i], 0);
        v->flags |= V_SPECIAL;
        if (!v->val)
            v->val = xstrdup("");
    }
    if (!var_get("PATH"))
        var_set("PATH", "/bin", V_EXPORT);
    if (!var_get("IFS"))
        var_set("IFS", " \t\n", 0);
}
