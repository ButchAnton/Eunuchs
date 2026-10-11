/*
 * esh: a forgiving scanner over the line being edited, for syntax
 * highlighting and for finding the completion context. Unlike the parser
 * it never fails: unterminated quotes simply run to the end.
 */
#include "esh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { T_WORD, T_OP, T_REDIR, T_COMMENT };

struct htok {
    int start, end;
    int type;
    int cmdpos;         /* a word in command position */
    int redir_target;   /* a word right after a redirection */
    int argi;           /* argument index within its command (0 = the command) */
    int cmd;            /* index of this command's first word token, or -1 */
};

static const char *const precommands[] = {
    "command", "builtin", "exec", "noglob", "nocorrect", "time", "strace", "nohup", "sudo",
    "env", 0,
};

static const char *const cmd_keywords[] = {
    "if", "then", "else", "elif", "do", "while", "until", "!", "{", "time", "repeat", 0,
};

static int in_list(const char *w, const char *const *l)
{
    for (int i = 0; l[i]; i++)
        if (!strcmp(w, l[i]))
            return 1;
    return 0;
}

static int word_end(const char *s, int i, int len)
{
    while (i < len) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' || c == '|' || c == '<' ||
            c == '>' || c == '(' || c == ')')
            return i;
        if (c == '\\') {
            i += 2;
            continue;
        }
        int k;
        if (c == '\'') {
            k = i + 1;
            while (k < len && s[k] != '\'')
                k++;
            i = k + 1;
            continue;
        }
        if (c == '"') {
            k = i + 1;
            while (k < len && s[k] != '"') {
                if (s[k] == '\\')
                    k++;
                k++;
            }
            i = k + 1;
            continue;
        }
        if (c == '`') {
            k = i + 1;
            while (k < len && s[k] != '`')
                k++;
            i = k + 1;
            continue;
        }
        if (c == '$' && i + 1 < len && (s[i + 1] == '(' || s[i + 1] == '{')) {
            k = s[i + 1] == '(' ? skip_cmdsub(s, i + 2, len) : skip_braces(s, i + 2, len);
            i = k < 0 ? len : k;
            continue;
        }
        i++;
    }
    return len;
}

static char *plain_word(const char *s, int n)
{
    /* the word without quoting, or NULL if it has expansions */
    char *d = amalloc(n + 1), *o = d;
    char q = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (!q && (c == '$' || c == '`'))
            return 0;
        if (q == '"' && (c == '$' || c == '`'))
            return 0;
        if (!q && (c == '\'' || c == '"')) {
            q = c;
            continue;
        }
        if (q && c == q) {
            q = 0;
            continue;
        }
        if (c == '\\' && q != '\'' && i + 1 < n) {
            *o++ = s[++i];
            continue;
        }
        *o++ = c;
    }
    *o = 0;
    return d;
}

static int scan(const char *s, int len, struct htok *t, int max)
{
    int n = 0, i = 0, cmdpos = 1, redir = 0, argi = 0, cmd = -1, after_pre = 0;
    while (i < len && n < max) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r') {
            i++;
            continue;
        }
        if (c == '\n') {
            cmdpos = 1;
            argi = 0;
            cmd = -1;
            i++;
            continue;
        }
        struct htok *k = &t[n];
        memset(k, 0, sizeof(*k));
        k->start = i;
        k->cmd = cmd;
        if (c == '#' && (i == 0 || strchr(" \t\n;&|(", s[i - 1])) && opts[OPT_INTERACTIVECOMMENTS]) {
            while (i < len && s[i] != '\n')
                i++;
            k->type = T_COMMENT;
            k->end = i;
            n++;
            continue;
        }
        if (strchr(";&|()", c)) {
            int j = i + 1;
            if (c == '&' && j < len && s[j] == '>') {
                j++;
                if (j < len && s[j] == '>')
                    j++;
                k->type = T_REDIR;
                redir = 1;
            } else {
                if (j < len && (s[j] == c || (c == '|' && s[j] == '&') || (c == ';' && s[j] == '&')))
                    j++;
                k->type = T_OP;
                cmdpos = c != ')';
                argi = 0;
                cmd = -1;
                after_pre = 0;
            }
            k->end = j;
            i = j;
            n++;
            continue;
        }
        int j = i;
        while (j < len && is_digit(s[j]))
            j++;
        if (j < len && (s[j] == '<' || s[j] == '>') && (j == i || j - i < 3)) {
            char a = s[j];
            j++;
            while (j < len && (s[j] == a || s[j] == '&' || s[j] == '|' || s[j] == '!' || s[j] == '-' ||
                               (a == '<' && s[j] == '>')) && j - i < 5)
                j++;
            k->type = T_REDIR;
            k->end = j;
            redir = 1;
            i = j;
            n++;
            continue;
        }
        j = word_end(s, i, len);
        if (j == i)
            j = i + 1;
        k->type = T_WORD;
        k->end = j;
        if (redir) {
            k->redir_target = 1;
            redir = 0;
        } else {
            char *w = astrndup(s + i, j - i);
            k->cmdpos = cmdpos;
            k->argi = cmdpos ? 0 : argi;
            if (cmdpos) {
                if (is_assignment_word(w)) {
                    /* stays in command position */
                } else if (in_list(w, cmd_keywords)) {
                    cmdpos = 1;
                } else if (is_keyword(w)) {
                    cmdpos = 0;
                    argi = 1;
                    cmd = n;
                } else if (after_pre && w[0] == '-') {
                    k->cmdpos = 0;      /* an option of a precommand */
                } else {
                    cmd = n;
                    k->cmd = n;
                    after_pre = in_list(w, precommands);
                    cmdpos = after_pre;
                    argi = 1;
                }
            } else {
                argi++;
            }
        }
        i = j;
        n++;
    }
    return n;
}

/* ---- highlighting ---- */

static int classify_command(const char *w)
{
    if (!w)
        return H_CMD;
    if (is_keyword(w))
        return H_KEYWORD;
    if (alias_get(w))
        return H_ALIAS;
    if (func_lookup(w))
        return H_FUNC;
    if (builtin_lookup(w))
        return H_BUILTIN;
    if (!*w)
        return H_NONE;
    if (path_lookup(w))
        return H_CMD;
    struct stat st;
    if (opts[OPT_AUTOCD] && stat(w, &st) == 0 && S_ISDIR(st.st_mode))
        return H_CMD;
    return H_BADCMD;
}

static void mark(unsigned char *attr, int a, int b, int h)
{
    for (int i = a; i < b; i++)
        attr[i] = h;
}

/* Quotes, variables and substitutions inside a word. */
static void mark_inner(const char *s, int a, int b, unsigned char *attr)
{
    for (int i = a; i < b;) {
        char c = s[i];
        if (c == '\\') {
            mark(attr, i, i + 2 < b ? i + 2 : b, H_QUOTE);
            i += 2;
            continue;
        }
        if (c == '\'') {
            int k = i + 1;
            while (k < b && s[k] != '\'')
                k++;
            mark(attr, i, k < b ? k + 1 : b, H_QUOTE);
            i = k + 1;
            continue;
        }
        if (c == '"') {
            int k = i + 1;
            while (k < b && s[k] != '"') {
                if (s[k] == '\\')
                    k++;
                k++;
            }
            int e = k < b ? k + 1 : b;
            mark(attr, i, e, H_QUOTE);
            for (int m = i + 1; m < e; m++)
                if (s[m] == '$') {
                    int q = m + 1;
                    if (q < e && s[q] == '{') {
                        while (q < e && s[q] != '}')
                            q++;
                        q++;
                    } else {
                        while (q < e && (is_name_char(s[q]) || (q == m + 1 && strchr("?$!#@*-", s[q]))))
                            q++;
                    }
                    mark(attr, m, q < e ? q : e, H_VAR);
                    m = q - 1;
                }
            i = e;
            continue;
        }
        if (c == '$') {
            int k;
            if (i + 1 < b && s[i + 1] == '(') {
                k = skip_cmdsub(s, i + 2, b);
                k = k < 0 ? b : k;
                mark(attr, i, k, H_SUBST);
            } else if (i + 1 < b && s[i + 1] == '{') {
                k = skip_braces(s, i + 2, b);
                k = k < 0 ? b : k;
                mark(attr, i, k, H_VAR);
            } else if (i + 1 < b && s[i + 1] == '\'') {
                k = i + 2;
                while (k < b && s[k] != '\'') {
                    if (s[k] == '\\')
                        k++;
                    k++;
                }
                k = k < b ? k + 1 : b;
                mark(attr, i, k, H_QUOTE);
            } else {
                k = i + 1;
                while (k < b && (is_name_char(s[k]) || (k == i + 1 && strchr("?$!#@*-0123456789", s[k]))))
                    k++;
                mark(attr, i, k, H_VAR);
            }
            i = k;
            continue;
        }
        if (c == '`') {
            int k = i + 1;
            while (k < b && s[k] != '`')
                k++;
            k = k < b ? k + 1 : b;
            mark(attr, i, k, H_SUBST);
            i = k;
            continue;
        }
        if (c == '*' || c == '?' || c == '[' || c == ']' || c == '{' || c == '}' || c == '~')
            attr[i] = H_GLOB;
        i++;
    }
}

void highlight(const char *s, int len, unsigned char *attr)
{
    struct htok t[256];
    int n = scan(s, len, t, 256);
    for (int i = 0; i < n; i++) {
        struct htok *k = &t[i];
        if (k->type == T_COMMENT) {
            mark(attr, k->start, k->end, H_COMMENT);
            continue;
        }
        if (k->type == T_OP || k->type == T_REDIR) {
            mark(attr, k->start, k->end, H_OP);
            continue;
        }
        const char *w = s + k->start;
        int wl = k->end - k->start;
        char *plain = plain_word(w, wl);
        if (k->cmdpos) {
            char *raw = astrndup(w, wl);
            if (is_assignment_word(raw)) {
                char *eq = strchr(raw, '=');
                mark(attr, k->start, k->start + (eq - raw) + 1, H_ASSIGN);
                mark_inner(s, k->start + (eq - raw) + 1, k->end, attr);
                continue;
            }
            int h = plain ? classify_command(plain) : H_CMD;
            /* while the cursor is still in the word, a prefix of a command is fine */
            if (h == H_BADCMD && k->end == len && plain) {
                struct strv v = { 0 };
                if (path_commands(plain, &v) || builtin_names(plain, &v) || alias_names(plain, &v) ||
                    func_names(plain, &v))
                    h = H_NONE;
            }
            mark(attr, k->start, k->end, h);
            if (!plain)
                mark_inner(s, k->start, k->end, attr);
            continue;
        }
        if (w[0] == '-' && !k->redir_target)
            mark(attr, k->start, k->end, H_OPTION);
        else if (plain && *plain && wl < 200) {
            struct stat st;
            char *p = plain;
            if (p[0] == '~') {
                const char *home = var_get("HOME");
                if ((p[1] == '/' || !p[1]) && home) {
                    struct sb b = { 0 };
                    sb_puts(&b, home);
                    sb_puts(&b, p + 1);
                    p = sb_adup(&b);
                    sb_free(&b);
                }
            }
            if (!pattern_has_magic(p) && stat(p, &st) == 0)
                mark(attr, k->start, k->end, H_PATH);
        }
        mark_inner(s, k->start, k->end, attr);
    }
}

const char *highlight_sgr(int h)
{
    switch (h) {
    case H_CMD: case H_BUILTIN: case H_ALIAS: case H_FUNC: return "\033[0;32m";
    case H_BADCMD: return "\033[0;1;31m";
    case H_KEYWORD: return "\033[0;33m";
    case H_PATH: return "\033[0;4m";
    case H_QUOTE: return "\033[0;33m";
    case H_VAR: return "\033[0;36m";
    case H_COMMENT: return "\033[0;90m";
    case H_GLOB: return "\033[0;34m";
    case H_SUBST: return "\033[0;35m";
    case H_ASSIGN: case H_OP: case H_OPTION: case H_NONE: default: return "\033[0m";
    }
}

/* ---- completion context ---- */

/* Returns 1 if the word at pos is in command position; *cmd is the
 * command this word is an argument of; *argi its index (-1: a
 * redirection target). */
int command_context(const char *buf, int pos, char **cmd, int *argi)
{
    struct htok t[256];
    int len = strlen(buf);
    int n = scan(buf, len, t, 256);
    *cmd = 0;
    *argi = 0;
    /* the token containing pos (a word ending at pos counts) */
    int hit = -1, prev = -1;
    for (int i = 0; i < n; i++) {
        if (t[i].start <= pos && pos <= t[i].end && t[i].type == T_WORD) {
            hit = i;
            break;
        }
        if (t[i].end <= pos)
            prev = i;
    }
    int cmdpos;
    int cmdtok;
    if (hit >= 0) {
        cmdpos = t[hit].cmdpos;
        cmdtok = t[hit].cmd;
        *argi = t[hit].redir_target ? -1 : t[hit].argi;
    } else {
        /* a new word at pos: decide from what came before */
        if (prev < 0) {
            return 1;
        }
        struct htok *p = &t[prev];
        if (p->type == T_REDIR) {
            *argi = -1;
            cmdtok = p->cmd;
            cmdpos = 0;
        } else if (p->type == T_OP) {
            const char *o = buf + p->start;
            return *o != ')';
        } else if (p->type == T_COMMENT) {
            return 0;
        } else {
            /* after a word */
            char *w = astrndup(buf + p->start, p->end - p->start);
            if (p->cmdpos && (in_list(w, cmd_keywords) || in_list(w, precommands) ||
                              is_assignment_word(w)))
                return 1;
            if (p->cmdpos && !p->redir_target && !is_keyword(w)) {
                cmdtok = prev;
                *argi = 1;
            } else {
                cmdtok = p->cmd;
                *argi = p->redir_target ? 1 : p->argi + 1;
            }
            cmdpos = 0;
        }
    }
    if (cmdtok >= 0)
        *cmd = plain_word(buf + t[cmdtok].start, t[cmdtok].end - t[cmdtok].start);
    return cmdpos;
}
