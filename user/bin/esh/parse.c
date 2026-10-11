/*
 * esh: lexer and parser.
 *
 * Words are kept raw (quotes and all) and expanded at run time. The lexer
 * only needs to find where each word ends, which means understanding
 * quotes, $( ), ${ }, ` ` and $(( )) nesting. Running out of input in the
 * middle of a construct sets P_INCOMPLETE, which the interactive loop
 * answers with a PS2 continuation prompt named after the open construct.
 */
#include "esh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum tok {
    T_EOF, T_NL, T_SEMI, T_AMP, T_PIPE, T_PIPEAMP, T_ANDAND, T_OROR, T_DSEMI, T_SEMIAMP,
    T_LPAREN, T_RPAREN, T_REDIR, T_WORD,
};

static const char *const tok_names[] = {
    "end of input", "newline", ";", "&", "|", "|&", "&&", "||", ";;", ";&", "(", ")",
    "redirection", "word",
};

static const char *const keywords[] = {
    "if", "then", "elif", "else", "fi", "do", "done", "case", "esac", "while", "until",
    "for", "in", "function", "time", "{", "}", "!", "[[", "]]", "repeat", 0,
};

int is_keyword(const char *w)
{
    for (int i = 0; keywords[i]; i++)
        if (!strcmp(w, keywords[i]))
            return 1;
    return 0;
}

/* ---- scanning helpers shared with the expander ---- */

static int skip_squote(const char *s, int i, int len)
{
    while (i < len && s[i] != '\'')
        i++;
    return i < len ? i + 1 : -1;
}

/* $'...': backslash escapes a quote */
static int skip_csquote(const char *s, int i, int len)
{
    while (i < len && s[i] != '\'') {
        if (s[i] == '\\' && i + 1 < len)
            i++;
        i++;
    }
    return i < len ? i + 1 : -1;
}

static int skip_bquote(const char *s, int i, int len)
{
    while (i < len && s[i] != '`') {
        if (s[i] == '\\' && i + 1 < len)
            i++;
        i++;
    }
    return i < len ? i + 1 : -1;
}

static int skip_dquote(const char *s, int i, int len);

/* After a '$' at s[i-1]: skip the construct starting at s[i]. */
static int skip_dollar(const char *s, int i, int len)
{
    if (i < len && s[i] == '(')
        return skip_cmdsub(s, i + 1, len);
    if (i < len && s[i] == '{')
        return skip_braces(s, i + 1, len);
    if (i < len && s[i] == '\'')
        return skip_csquote(s, i + 1, len);
    return i;
}

static int skip_dquote(const char *s, int i, int len)
{
    while (i < len && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < len) {
            i += 2;
        } else if (s[i] == '$' && i + 1 < len && (s[i + 1] == '(' || s[i + 1] == '{')) {
            i = skip_dollar(s, i + 1, len);
            if (i < 0)
                return -1;
        } else if (s[i] == '`') {
            i = skip_bquote(s, i + 1, len);
            if (i < 0)
                return -1;
        } else {
            i++;
        }
    }
    return i < len ? i + 1 : -1;
}

static int word_at(const char *s, int i, int len, const char *w)
{
    int n = strlen(w);
    if (i + n > len || strncmp(s + i, w, n))
        return 0;
    if (i > 0 && !strchr(" \t\n;&|(", s[i - 1]))
        return 0;
    return i + n == len || strchr(" \t\n;&|()", s[i + n]);
}

int skip_cmdsub(const char *s, int i, int len)
{
    int depth = 1, in_case = 0;
    while (i < len) {
        char c = s[i];
        if (c == '\\') {
            i += 2;
            continue;
        }
        if (c == '\'') {
            if ((i = skip_squote(s, i + 1, len)) < 0)
                return -1;
            continue;
        }
        if (c == '"') {
            if ((i = skip_dquote(s, i + 1, len)) < 0)
                return -1;
            continue;
        }
        if (c == '`') {
            if ((i = skip_bquote(s, i + 1, len)) < 0)
                return -1;
            continue;
        }
        if (c == '$' && i + 1 < len && (s[i + 1] == '{' || s[i + 1] == '\'')) {
            if ((i = skip_dollar(s, i + 1, len)) < 0)
                return -1;
            continue;
        }
        if (c == '#' && (i == 0 || strchr(" \t\n", s[i - 1]))) {
            while (i < len && s[i] != '\n')
                i++;
            continue;
        }
        if (word_at(s, i, len, "case"))
            in_case++;
        else if (word_at(s, i, len, "esac") && in_case)
            in_case--;
        if (c == '(') {
            depth++;
        } else if (c == ')') {
            if (in_case && depth == 1) {
                i++;
                continue;       /* a case pattern's ) */
            }
            if (--depth == 0)
                return i + 1;
        }
        i++;
    }
    return -1;
}

int skip_braces(const char *s, int i, int len)
{
    int depth = 1;
    while (i < len) {
        char c = s[i];
        if (c == '\\') {
            i += 2;
            continue;
        }
        if (c == '\'') {
            if ((i = skip_squote(s, i + 1, len)) < 0)
                return -1;
            continue;
        }
        if (c == '"') {
            if ((i = skip_dquote(s, i + 1, len)) < 0)
                return -1;
            continue;
        }
        if (c == '`') {
            if ((i = skip_bquote(s, i + 1, len)) < 0)
                return -1;
            continue;
        }
        if (c == '$' && i + 1 < len && (s[i + 1] == '(' || s[i + 1] == '{')) {
            if ((i = skip_dollar(s, i + 1, len)) < 0)
                return -1;
            continue;
        }
        if (c == '{')
            depth++;
        else if (c == '}' && --depth == 0)
            return i + 1;
        i++;
    }
    return -1;
}

/* Is s[0..n) the start of an assignment ("name=", "name+=", "name[i]=")? */
static int assign_prefix(const char *s, int n)
{
    int i = 0;
    if (n < 2 || !is_name_start(s[0]))
        return 0;
    while (i < n && is_name_char(s[i]))
        i++;
    if (i < n && s[i] == '[') {
        int d = 0;
        for (; i < n; i++) {
            if (s[i] == '[')
                d++;
            else if (s[i] == ']' && --d == 0)
                break;
        }
        if (i >= n)
            return 0;
        i++;
    }
    if (i < n && s[i] == '+')
        i++;
    return i < n && s[i] == '=' ? i + 1 : 0;
}

int is_assignment_word(const char *w)
{
    return assign_prefix(w, strlen(w)) > 0;
}

/* ---- lexer ---- */

static void incomplete(struct parser *p, const char *what)
{
    if (p->status == P_OK) {
        p->status = P_INCOMPLETE;
        p->open = what;
    }
}

static void syntax(struct parser *p, const char *fmt, const char *arg)
{
    if (p->status == P_OK) {
        p->status = P_ERROR;
        snprintf(p->err, sizeof(p->err), fmt, arg);
    }
}

static int is_meta(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' || c == '|' || c == '<' ||
           c == '>' || c == '(' || c == ')';
}

static void lex(struct parser *p)
{
    const char *s = p->s;
    int len = p->len, i = p->pos;
    struct ptok *t = &p->tk;
    memset(t, 0, sizeof(*t));
    p->have_tk = 1;

    for (;;) {
        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r'))
            i++;
        if (i + 1 < len && s[i] == '\\' && s[i + 1] == '\n') {
            i += 2;
            continue;
        }
        if (i + 1 == len && s[i] == '\\' && !p->final) {
            incomplete(p, "");
            t->t = T_EOF;
            t->start = t->end = len;
            return;
        }
        if (i < len && s[i] == '#' && opts[OPT_INTERACTIVECOMMENTS]) {
            while (i < len && s[i] != '\n')
                i++;
        }
        break;
    }
    t->start = i;
    if (i >= len) {
        t->t = T_EOF;
        t->end = i;
        return;
    }
    char c = s[i], d = i + 1 < len ? s[i + 1] : 0;
    t->end = i + 1;
    switch (c) {
    case '\n':
        t->t = T_NL;
        return;
    case ';':
        if (d == ';') { t->t = T_DSEMI; t->end++; return; }
        if (d == '&' || d == '|') { t->t = T_SEMIAMP; t->end++; return; }
        t->t = T_SEMI;
        return;
    case '&':
        if (d == '&') { t->t = T_ANDAND; t->end++; return; }
        if (d == '>') {
            t->t = T_REDIR;
            t->rfd = 1;
            if (i + 2 < len && s[i + 2] == '>') {
                t->rop = R_ALLAPP;
                t->end = i + 3;
            } else {
                t->rop = R_ALLOUT;
                t->end = i + 2;
            }
            return;
        }
        if (d == '|' || d == '!') { t->t = T_AMP; t->end++; return; }     /* zsh &| &!: disown */
        t->t = T_AMP;
        return;
    case '|':
        if (d == '|') { t->t = T_OROR; t->end++; return; }
        if (d == '&') { t->t = T_PIPEAMP; t->end++; return; }
        t->t = T_PIPE;
        return;
    case '(':
        t->t = T_LPAREN;
        return;
    case ')':
        t->t = T_RPAREN;
        return;
    }

    /* redirection: [n]op */
    int j = i, fd = -1;
    if (!p->cond_mode) {
        while (j < len && is_digit(s[j]))
            j++;
        if (j > i && j < len && (s[j] == '<' || s[j] == '>') && j - i < 3)
            fd = atoi(s + i);
        else
            j = i;
        if (j < len && (s[j] == '<' || s[j] == '>')) {
            char a = s[j], b = j + 1 < len ? s[j + 1] : 0, e = j + 2 < len ? s[j + 2] : 0;
            t->t = T_REDIR;
            if (a == '<') {
                if (b == '<' && e == '<') { t->rop = R_HERESTR; j += 3; }
                else if (b == '<' && e == '-') { t->rop = R_HEREDOC; t->strip = 1; j += 3; }
                else if (b == '<') { t->rop = R_HEREDOC; j += 2; }
                else if (b == '>') { t->rop = R_RDWR; j += 2; }
                else if (b == '&') { t->rop = R_DUPIN; j += 2; }
                else { t->rop = R_IN; j += 1; }
                t->rfd = fd >= 0 ? fd : 0;
            } else {
                if (b == '>' ) { t->rop = R_APPEND; j += 2; }
                else if (b == '|' || b == '!') { t->rop = R_CLOBBER; j += 2; }
                else if (b == '&') { t->rop = R_DUPOUT; j += 2; }
                else { t->rop = R_OUT; j += 1; }
                if (t->rop == R_DUPOUT && fd < 0)
                    t->strip = 1;           /* >&word: may mean &>word */
                t->rfd = fd >= 0 ? fd : 1;
            }
            t->end = j;
            return;
        }
    }

    /* a word */
    int quoted = 0;
    j = i;
    while (j < len) {
        c = s[j];
        if (is_meta(c)) {
            if (p->cond_mode && (c == '<' || c == '>') && j == i) {
                j++;
                if (j < len && s[j] == '=')
                    j++;
                break;
            }
            if (c == '(' && j > i && s[j - 1] == '=' && assign_prefix(s + i, j - i) == j - i) {
                /* name=( ... ): an array assignment, kept as one word */
                int depth = 1, k = j + 1;
                while (k < len && depth) {
                    if (s[k] == '\\') { k += 2; continue; }
                    if (s[k] == '\'') { k = skip_squote(s, k + 1, len); if (k < 0) break; continue; }
                    if (s[k] == '"') { k = skip_dquote(s, k + 1, len); if (k < 0) break; continue; }
                    if (s[k] == '$' && k + 1 < len && (s[k + 1] == '(' || s[k + 1] == '{')) {
                        k = skip_dollar(s, k + 1, len);
                        if (k < 0) break;
                        continue;
                    }
                    if (s[k] == '(') depth++;
                    else if (s[k] == ')') depth--;
                    k++;
                }
                if (k < 0 || depth) {
                    if (p->final)
                        syntax(p, "parse error: unterminated %s", "array");
                    else
                        incomplete(p, "array");
                    j = len;
                    break;
                }
                j = k;
                continue;
            }
            break;
        }
        int k = j;
        if (c == '\\') {
            quoted = 1;
            if (j + 1 >= len) {
                if (!p->final)
                    incomplete(p, "");
                j = len;
                break;
            }
            j += 2;
            continue;
        }
        if (c == '\'') {
            quoted = 1;
            k = skip_squote(s, j + 1, len);
            if (k < 0) {
                if (p->final)
                    syntax(p, "parse error: unmatched %s", "'");
                incomplete(p, "quote");
            }
        } else if (c == '"') {
            quoted = 1;
            k = skip_dquote(s, j + 1, len);
            if (k < 0) {
                if (p->final)
                    syntax(p, "parse error: unmatched %s", "\"");
                incomplete(p, "dquote");
            }
        } else if (c == '`') {
            k = skip_bquote(s, j + 1, len);
            if (k < 0) {
                if (p->final)
                    syntax(p, "parse error: unmatched %s", "`");
                incomplete(p, "bquote");
            }
        } else if (c == '$' && j + 1 < len && (s[j + 1] == '(' || s[j + 1] == '{' || s[j + 1] == '\'')) {
            if (s[j + 1] == '\'')
                quoted = 1;
            k = skip_dollar(s, j + 1, len);
            if (k < 0) {
                const char *what = s[j + 1] == '{' ? "braceparam"
                                 : s[j + 1] == '\'' ? "quote"
                                 : j + 2 < len && s[j + 2] == '(' ? "mathsubst" : "cmdsubst";
                if (p->final)
                    syntax(p, "parse error: unterminated %s", what);
                incomplete(p, what);
            }
        } else {
            j++;
            continue;
        }
        if (k < 0) {
            j = len;
            break;
        }
        j = k;
    }
    t->t = T_WORD;
    t->end = j;
    t->quoted = quoted;
    t->w = astrndup(s + i, j - i);
}

static struct ptok *peek(struct parser *p)
{
    if (!p->have_tk)
        lex(p);
    return &p->tk;
}

static void read_heredocs(struct parser *p);

static void consume(struct parser *p)
{
    peek(p);
    for (int i = p->pos; i < p->tk.end; i++)
        if (p->s[i] == '\n')
            p->line++;
    p->pos = p->tk.end;
    p->last_end = p->tk.end;
    int nl = p->tk.t == T_NL;
    p->have_tk = 0;
    if (nl && p->nhd)
        read_heredocs(p);
}

/* Remove quoting from a here-document delimiter. */
static char *dequote_delim(const char *w, int *quoted)
{
    char *d = amalloc(strlen(w) + 1), *o = d;
    char q = 0;
    *quoted = 0;
    for (; *w; w++) {
        if (!q && (*w == '\'' || *w == '"')) {
            q = *w;
            *quoted = 1;
            continue;
        }
        if (q && *w == q) {
            q = 0;
            continue;
        }
        if (*w == '\\' && w[1] && q != '\'') {
            *quoted = 1;
            w++;
        }
        *o++ = *w;
    }
    *o = 0;
    return d;
}

static void read_heredocs(struct parser *p)
{
    for (int h = 0; h < p->nhd; h++) {
        struct redir *r = p->hd[h];
        int quoted;
        char *delim = dequote_delim(r->word, &quoted);
        r->hd_quoted = quoted;
        struct sb body = { 0 };
        int found = 0;
        while (p->pos < p->len) {
            int e = p->pos;
            while (e < p->len && p->s[e] != '\n')
                e++;
            int b = p->pos;
            if (p->hd_strip[h])
                while (b < e && p->s[b] == '\t')
                    b++;
            int n = e - b;
            p->pos = e < p->len ? e + 1 : e;
            p->line++;
            if (n == (int)strlen(delim) && !strncmp(p->s + b, delim, n)) {
                found = 1;
                break;
            }
            sb_putn(&body, p->s + b, n);
            sb_putc(&body, '\n');
        }
        if (!found && !p->final) {
            incomplete(p, "heredoc");
            sb_free(&body);
            return;
        }
        r->heredoc = sb_adup(&body);
        sb_free(&body);
    }
    p->nhd = 0;
    p->last_end = p->pos;
}

/* ---- parser ---- */

void parser_init(struct parser *p, const char *src)
{
    memset(p, 0, sizeof(*p));
    p->s = xstrdup(src);
    p->len = strlen(src);
    p->line = 1;
    p->status = P_OK;
}

void parser_free(struct parser *p)
{
    free(p->s);
    p->s = 0;
}

static struct node *mk(struct parser *p, enum ntype t)
{
    struct node *n = amalloc(sizeof(*n));
    memset(n, 0, sizeof(*n));
    n->t = t;
    n->line = p->line;
    return n;
}

static int is_word(struct parser *p, const char *w)
{
    struct ptok *t = peek(p);
    return t->t == T_WORD && !t->quoted && !strcmp(t->w, w);
}

static void unexpected(struct parser *p)
{
    struct ptok *t = peek(p);
    if (p->status != P_OK)
        return;
    if (t->t == T_EOF) {
        incomplete(p, p->open ? p->open : "");
        if (p->final) {
            p->status = P_ERROR;
            snprintf(p->err, sizeof(p->err), "parse error: unexpected end of input");
        }
        return;
    }
    char what[40];
    if (t->t == T_WORD)
        snprintf(what, sizeof(what), "%s", t->w);
    else if (t->t == T_REDIR)
        snprintf(what, sizeof(what), "%.*s", t->end - t->start, p->s + t->start);
    else if (t->t == T_NL)
        snprintf(what, sizeof(what), "\\n");
    else
        snprintf(what, sizeof(what), "%s", tok_names[t->t]);
    syntax(p, "parse error near `%s'", what);
}

static int expect_word(struct parser *p, const char *w, const char *open)
{
    if (is_word(p, w)) {
        consume(p);
        return 1;
    }
    if (peek(p)->t == T_EOF)
        p->open = open;
    unexpected(p);
    return 0;
}

static void skip_newlines(struct parser *p)
{
    while (p->status == P_OK && peek(p)->t == T_NL)
        consume(p);
}

static struct node *parse_and_or(struct parser *p);
static struct node *parse_pipeline(struct parser *p);
static struct node *parse_command_unit(struct parser *p);

/* A list inside a compound command, up to one of the terminators. */
static int at_terminator(struct parser *p, const char *const *terms)
{
    struct ptok *t = peek(p);
    if (t->t == T_EOF || t->t == T_RPAREN || t->t == T_DSEMI || t->t == T_SEMIAMP)
        return 1;
    if (t->t != T_WORD || t->quoted)
        return 0;
    for (int i = 0; terms && terms[i]; i++)
        if (!strcmp(t->w, terms[i]))
            return 1;
    return 0;
}

static struct node *seq(struct parser *p, struct node *a, struct node *b)
{
    if (!a)
        return b;
    if (!b)
        return a;
    struct node *n = mk(p, N_SEQ);
    n->a = a;
    n->b = b;
    n->line = a->line;
    return n;
}

static struct node *parse_list(struct parser *p, const char *const *terms, const char *open)
{
    const char *saved = p->open;
    p->open = open;
    struct node *list = 0;
    for (;;) {
        skip_newlines(p);
        if (p->status != P_OK || at_terminator(p, terms))
            break;
        struct node *n = parse_and_or(p);
        if (!n)
            return 0;
        struct ptok *t = peek(p);
        if (t->t == T_AMP) {
            struct node *bg = mk(p, N_BG);
            bg->a = n;
            bg->text = n->text;
            bg->line = n->line;
            n = bg;
            consume(p);
        } else if (t->t == T_SEMI || t->t == T_NL) {
            consume(p);
        } else if (!at_terminator(p, terms)) {
            unexpected(p);
            return 0;
        }
        list = seq(p, list, n);
    }
    if (p->status != P_OK)
        return 0;
    p->open = saved;
    return list;
}

static struct node *parse_and_or(struct parser *p)
{
    struct node *left = parse_pipeline(p);
    while (left && p->status == P_OK) {
        struct ptok *t = peek(p);
        if (t->t != T_ANDAND && t->t != T_OROR)
            break;
        struct node *n = mk(p, t->t == T_ANDAND ? N_AND : N_OR);
        const char *saved = p->open;
        p->open = t->t == T_ANDAND ? "cmdand" : "cmdor";
        consume(p);
        skip_newlines(p);
        n->a = left;
        n->b = parse_pipeline(p);
        p->open = saved;
        if (!n->b) {
            if (p->status == P_OK)
                unexpected(p);
            return 0;
        }
        n->line = left->line;
        left = n;
    }
    return p->status == P_OK ? left : 0;
}

static struct node *parse_pipeline(struct parser *p)
{
    int negate = 0, timed = 0;
    for (;;) {
        if (is_word(p, "!")) {
            negate = !negate;
            consume(p);
        } else if (is_word(p, "time")) {
            timed = 1;
            consume(p);
        } else {
            break;
        }
    }
    int start = peek(p)->start;     /* job and time texts leave out ! and time */
    struct node *cmds[32];
    int n = 0, stderr_mask = 0;
    if (timed && (peek(p)->t == T_NL || peek(p)->t == T_EOF || peek(p)->t == T_SEMI)) {
        /* bare "time": time the shell itself (zsh prints shell times) */
        struct node *e = mk(p, N_CMD);
        cmds[n++] = e;
    } else {
        for (;;) {
            struct node *c = parse_command_unit(p);
            if (!c) {
                if (p->status == P_OK)
                    unexpected(p);
                return 0;
            }
            if (n == 32) {
                syntax(p, "%s", "pipeline too long");
                return 0;
            }
            cmds[n++] = c;
            struct ptok *t = peek(p);
            if (t->t != T_PIPE && t->t != T_PIPEAMP)
                break;
            if (t->t == T_PIPEAMP)
                stderr_mask |= 1 << (n - 1);
            const char *saved = p->open;
            p->open = "pipe";
            consume(p);
            skip_newlines(p);
            p->open = saved;
        }
    }
    struct node *r;
    if (n == 1) {
        r = cmds[0];
    } else {
        r = mk(p, N_PIPE);
        r->cmds = amalloc(n * sizeof(struct node *));
        memcpy(r->cmds, cmds, n * sizeof(struct node *));
        r->ncmds = n;
        r->pipe_stderr = stderr_mask;
        r->line = cmds[0]->line;
    }
    r->text = astrndup(p->s + start, p->last_end - start);
    if (timed) {
        struct node *t = mk(p, N_TIME);
        t->a = r;
        t->text = r->text;
        r = t;
    }
    if (negate) {
        struct node *t = mk(p, N_NOT);
        t->a = r;
        t->text = r->text;
        r = t;
    }
    return r;
}

/* ---- redirections ---- */

static int parse_redir(struct parser *p, struct redir **list)
{
    struct ptok t = *peek(p);
    consume(p);
    struct ptok *w = peek(p);
    if (w->t != T_WORD) {
        unexpected(p);
        return -1;
    }
    struct redir *r = amalloc(sizeof(*r));
    memset(r, 0, sizeof(*r));
    r->op = t.rop;
    r->fd = t.rfd;
    r->word = w->w;
    if (r->op == R_DUPOUT && t.strip && !is_number(w->w) && strcmp(w->w, "-"))
        r->op = R_ALLOUT;           /* >& file */
    if (r->op == R_HEREDOC) {
        if (p->nhd == 16) {
            syntax(p, "%s", "too many here-documents");
            return -1;
        }
        p->hd_strip[p->nhd] = t.strip;
        p->hd[p->nhd++] = r;
    }
    consume(p);
    struct redir **tail = list;
    while (*tail)
        tail = &(*tail)->next;
    *tail = r;
    return 0;
}

static void parse_trailing_redirs(struct parser *p, struct node *n)
{
    while (p->status == P_OK && peek(p)->t == T_REDIR)
        if (parse_redir(p, &n->redirs) < 0)
            return;
}

/* ---- assignments ---- */

/* Split the inside of name=( ... ) into raw words. */
static char **split_array(struct parser *p, const char *src, int *n)
{
    struct parser sub;
    parser_init(&sub, src);
    sub.final = 1;
    sub.no_alias = 1;
    struct strv v = { 0 };
    for (;;) {
        struct ptok *t = peek(&sub);
        if (t->t == T_NL) {
            consume(&sub);
            continue;
        }
        if (t->t != T_WORD)
            break;
        strv_push(&v, t->w);
        consume(&sub);
    }
    if (peek(&sub)->t != T_EOF || sub.status != P_OK)
        syntax(p, "parse error in array %s", "assignment");
    parser_free(&sub);
    *n = v.n;
    return v.v ? v.v : amalloc(sizeof(char *));
}

static struct assign *parse_assign(struct parser *p, const char *w)
{
    struct assign *a = amalloc(sizeof(*a));
    memset(a, 0, sizeof(*a));
    int i = 0;
    while (is_name_char(w[i]))
        i++;
    a->name = astrndup(w, i);
    if (w[i] == '[') {
        int s = i + 1, d = 0;
        for (; w[i]; i++) {
            if (w[i] == '[')
                d++;
            else if (w[i] == ']' && --d == 0)
                break;
        }
        a->index = astrndup(w + s, i - s);
        i++;
    }
    if (w[i] == '+') {
        a->append = 1;
        i++;
    }
    i++;    /* '=' */
    if (w[i] == '(' && w[strlen(w) - 1] == ')') {
        a->is_array = 1;
        char *inner = astrndup(w + i + 1, strlen(w) - i - 2);
        a->array = split_array(p, inner, &a->narray);
    } else {
        a->value = astrdup(w + i);
    }
    return a;
}

struct assign *parse_assignment(const char *raw)
{
    struct parser p;
    memset(&p, 0, sizeof(p));
    p.status = P_OK;
    struct assign *a = parse_assign(&p, raw);
    if (p.status != P_OK) {
        sh_error("%s", p.err);
        return 0;
    }
    return a;
}

/* ---- aliases ---- */

static int try_alias(struct parser *p)
{
    struct ptok *t = peek(p);
    if (p->no_alias || t->t != T_WORD || t->quoted)
        return 0;
    const char *val = alias_get(t->w);
    if (!val)
        return 0;
    for (int i = 0; i < p->nexpanded; i++)
        if (p->expanded[i].pos == t->start && !strcmp(p->expanded[i].name, t->w))
            return 0;
    if (p->nexpanded == 32)
        return 0;
    p->expanded[p->nexpanded].pos = t->start;
    snprintf(p->expanded[p->nexpanded].name, sizeof(p->expanded[0].name), "%s", t->w);
    p->nexpanded++;
    int vlen = strlen(val);
    int nlen = p->len - (t->end - t->start) + vlen;
    char *ns = xmalloc(nlen + 1);
    memcpy(ns, p->s, t->start);
    memcpy(ns + t->start, val, vlen);
    memcpy(ns + t->start + vlen, p->s + t->end, p->len - t->end);
    ns[nlen] = 0;
    p->alias_next = vlen && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t');
    free(p->s);
    p->s = ns;
    p->len = nlen;
    p->pos = t->start;
    p->have_tk = 0;
    return 1;
}

/* ---- [[ ]] ---- */

static const char *const unary_ops[] = {
    "-a", "-b", "-c", "-d", "-e", "-f", "-g", "-h", "-k", "-n", "-o", "-p", "-r", "-s", "-t",
    "-u", "-w", "-x", "-z", "-L", "-O", "-G", "-S", "-N", 0,
};
static const char *const binary_ops[] = {
    "=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", "-nt", "-ot", "-ef",
    "=~", 0,
};

static int in_list(const char *w, const char *const *l)
{
    for (int i = 0; l[i]; i++)
        if (!strcmp(w, l[i]))
            return 1;
    return 0;
}

static struct cnode *cond_or(struct parser *p);

static struct cnode *cmk(enum cop op)
{
    struct cnode *c = amalloc(sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->op = op;
    return c;
}

static int cond_end(struct parser *p)
{
    struct ptok *t = peek(p);
    return t->t == T_EOF || (t->t == T_WORD && !t->quoted && !strcmp(t->w, "]]"));
}

static struct cnode *cond_primary(struct parser *p)
{
    while (peek(p)->t == T_NL)
        consume(p);
    struct ptok *t = peek(p);
    if (t->t == T_LPAREN) {
        consume(p);
        struct cnode *c = cond_or(p);
        if (!c)
            return 0;
        if (peek(p)->t != T_RPAREN) {
            unexpected(p);
            return 0;
        }
        consume(p);
        return c;
    }
    if (t->t == T_WORD && !t->quoted && !strcmp(t->w, "!")) {
        consume(p);
        struct cnode *c = cmk(C_NOT);
        c->a = cond_primary(p);
        return c->a ? c : 0;
    }
    if (t->t != T_WORD || cond_end(p)) {
        unexpected(p);
        return 0;
    }
    char *w1 = t->w;
    int q1 = t->quoted;
    consume(p);
    t = peek(p);
    if (!q1 && in_list(w1, unary_ops) && t->t == T_WORD && !cond_end(p) &&
        !(!t->quoted && in_list(t->w, binary_ops))) {
        struct cnode *c = cmk(C_UNARY);
        c->opname = w1;
        c->l = t->w;
        consume(p);
        return c;
    }
    if (t->t == T_WORD && !t->quoted && in_list(t->w, binary_ops)) {
        struct cnode *c = cmk(C_BINARY);
        c->opname = t->w;
        c->l = w1;
        consume(p);
        t = peek(p);
        if (t->t != T_WORD || cond_end(p)) {
            unexpected(p);
            return 0;
        }
        c->r = t->w;
        consume(p);
        return c;
    }
    struct cnode *c = cmk(C_WORD);
    c->l = w1;
    return c;
}

static struct cnode *cond_and(struct parser *p)
{
    struct cnode *l = cond_primary(p);
    while (l && peek(p)->t == T_ANDAND) {
        consume(p);
        struct cnode *c = cmk(C_AND);
        c->a = l;
        c->b = cond_primary(p);
        if (!c->b)
            return 0;
        l = c;
    }
    return l;
}

static struct cnode *cond_or(struct parser *p)
{
    struct cnode *l = cond_and(p);
    while (l && peek(p)->t == T_OROR) {
        consume(p);
        struct cnode *c = cmk(C_OR);
        c->a = l;
        c->b = cond_and(p);
        if (!c->b)
            return 0;
        l = c;
    }
    return l;
}

static struct node *parse_cond(struct parser *p)
{
    struct node *n = mk(p, N_COND);
    const char *saved = p->open;
    p->open = "cond";
    consume(p);                         /* [[ */
    p->cond_mode = 1;
    p->have_tk = 0;
    n->cn = cond_or(p);
    p->have_tk = 0;
    if (n->cn && !is_word(p, "]]"))
        unexpected(p);
    p->cond_mode = 0;
    if (p->status != P_OK)
        return 0;
    p->have_tk = 0;
    consume(p);
    p->open = saved;
    return n;
}

/* ---- compound commands ---- */

static const char *const T_THEN[] = { "then", 0 };
static const char *const T_IFBODY[] = { "elif", "else", "fi", 0 };
static const char *const T_FI[] = { "fi", 0 };
static const char *const T_DO[] = { "do", 0 };
static const char *const T_DONE[] = { "done", 0 };
static const char *const T_ESAC[] = { "esac", 0 };
static const char *const T_BRACE[] = { "}", 0 };

static struct node *parse_if(struct parser *p)
{
    struct node *n = mk(p, N_IF);
    consume(p);     /* if / elif */
    n->a = parse_list(p, T_THEN, "if");
    if (p->status != P_OK)
        return 0;
    if (!n->a) {
        p->open = "if";
        unexpected(p);
        return 0;
    }
    if (!expect_word(p, "then", "if"))
        return 0;
    n->b = parse_list(p, T_IFBODY, "then");
    if (p->status != P_OK)
        return 0;
    if (is_word(p, "elif")) {
        n->c = parse_if(p);
        return n->c ? n : 0;
    }
    if (is_word(p, "else")) {
        consume(p);
        n->c = parse_list(p, T_FI, "else");
        if (p->status != P_OK)
            return 0;
    }
    return expect_word(p, "fi", "then") ? n : 0;
}

static struct node *parse_do_done(struct parser *p, const char *open)
{
    skip_newlines(p);
    if (is_word(p, "{")) {              /* zsh: for x in a b; { ... } */
        return parse_command_unit(p);
    }
    if (!expect_word(p, "do", open))
        return 0;
    struct node *body = parse_list(p, T_DONE, open);
    if (p->status != P_OK)
        return 0;
    if (!expect_word(p, "done", open))
        return 0;
    return body ? body : mk(p, N_CMD);
}

static struct node *parse_while(struct parser *p, int until)
{
    struct node *n = mk(p, until ? N_UNTIL : N_WHILE);
    const char *open = until ? "until" : "while";
    consume(p);
    n->a = parse_list(p, T_DO, open);
    if (p->status != P_OK)
        return 0;
    if (!n->a) {
        p->open = open;
        unexpected(p);
        return 0;
    }
    n->b = parse_do_done(p, open);
    return n->b ? n : 0;
}

static char *read_arith(struct parser *p, int from);

static struct node *parse_for(struct parser *p)
{
    struct node *n = mk(p, N_FOR);
    consume(p);
    struct ptok *t = peek(p);
    if (t->t == T_LPAREN && p->pos < p->len && p->s[t->start + 1] == '(') {
        /* for (( init; cond; step )) */
        char *text = read_arith(p, t->start);
        if (!text)
            return 0;
        n->t = N_ARITHFOR;
        char *parts[3] = { "", "", "" };
        int k = 0;
        char *s = text, *st = text;
        int depth = 0;
        for (; *s && k < 2; s++) {
            if (*s == '(') depth++;
            else if (*s == ')') depth--;
            else if (*s == ';' && !depth) {
                parts[k++] = astrndup(st, s - st);
                st = s + 1;
            }
        }
        if (k != 2) {
            syntax(p, "%s", "parse error in for (( ; ; ))");
            return 0;
        }
        parts[2] = astrdup(st);
        n->init = parts[0];
        n->cond = parts[1];
        n->step = parts[2];
        if (peek(p)->t == T_SEMI)
            consume(p);
        n->b = parse_do_done(p, "for");
        return n->b ? n : 0;
    }
    if (t->t != T_WORD || !is_name(t->w)) {
        unexpected(p);
        return 0;
    }
    n->var = t->w;
    consume(p);
    skip_newlines(p);
    if (is_word(p, "in")) {
        consume(p);
        struct strv v = { 0 };
        while (peek(p)->t == T_WORD) {
            strv_push(&v, peek(p)->w);
            consume(p);
        }
        n->list = v.v ? v.v : amalloc(sizeof(char *));
        n->nlist = v.n;
        if (peek(p)->t == T_SEMI || peek(p)->t == T_NL)
            consume(p);
        else if (p->status == P_OK && peek(p)->t != T_EOF) {
            unexpected(p);
            return 0;
        }
    } else if (peek(p)->t == T_LPAREN) {   /* zsh: for x (a b c) */
        consume(p);
        struct strv v = { 0 };
        while (peek(p)->t == T_WORD) {
            strv_push(&v, peek(p)->w);
            consume(p);
        }
        if (peek(p)->t != T_RPAREN) {
            unexpected(p);
            return 0;
        }
        consume(p);
        n->list = v.v ? v.v : amalloc(sizeof(char *));
        n->nlist = v.n;
    } else if (peek(p)->t == T_SEMI) {
        consume(p);
    }
    n->b = parse_do_done(p, "for");
    return n->b ? n : 0;
}

static struct node *parse_repeat(struct parser *p)
{
    struct node *n = mk(p, N_REPEAT);
    consume(p);
    if (peek(p)->t != T_WORD) {
        unexpected(p);
        return 0;
    }
    n->var = peek(p)->w;
    consume(p);
    if (peek(p)->t == T_SEMI || peek(p)->t == T_NL) {
        consume(p);
        n->b = parse_do_done(p, "repeat");
    } else if (is_word(p, "do") || is_word(p, "{")) {
        n->b = parse_do_done(p, "repeat");
    } else {
        n->b = parse_pipeline(p);
    }
    return n->b ? n : 0;
}

static struct node *parse_case(struct parser *p)
{
    struct node *n = mk(p, N_CASE);
    consume(p);
    if (peek(p)->t != T_WORD) {
        unexpected(p);
        return 0;
    }
    n->subject = peek(p)->w;
    consume(p);
    skip_newlines(p);
    if (!expect_word(p, "in", "case"))
        return 0;
    struct casei **tail = &n->cases;
    for (;;) {
        skip_newlines(p);
        if (p->status != P_OK)
            return 0;
        if (is_word(p, "esac")) {
            consume(p);
            break;
        }
        if (peek(p)->t == T_EOF) {
            p->open = "case";
            unexpected(p);
            return 0;
        }
        struct casei *ci = amalloc(sizeof(*ci));
        memset(ci, 0, sizeof(*ci));
        if (peek(p)->t == T_LPAREN)
            consume(p);
        struct strv pats = { 0 };
        for (;;) {
            struct ptok *t = peek(p);
            if (t->t != T_WORD) {
                unexpected(p);
                return 0;
            }
            strv_push(&pats, t->w);
            consume(p);
            if (peek(p)->t == T_PIPE) {
                consume(p);
                continue;
            }
            if (peek(p)->t == T_RPAREN) {
                consume(p);
                break;
            }
            unexpected(p);
            return 0;
        }
        ci->pats = pats.v;
        ci->npats = pats.n;
        ci->body = parse_list(p, T_ESAC, "case");
        if (p->status != P_OK)
            return 0;
        struct ptok *t = peek(p);
        if (t->t == T_DSEMI) {
            consume(p);
        } else if (t->t == T_SEMIAMP) {
            ci->fallthrough = 1;
            consume(p);
        } else if (!is_word(p, "esac")) {
            p->open = "case";
            unexpected(p);
            return 0;
        }
        *tail = ci;
        tail = &ci->next;
    }
    return n;
}

/* Read (( ... )) starting at s[from]; returns the inner text. */
static char *read_arith(struct parser *p, int from)
{
    int i = from + 2, depth = 0;
    while (i < p->len) {
        char c = p->s[i];
        if (c == '(') {
            depth++;
        } else if (c == ')') {
            if (depth == 0) {
                if (i + 1 < p->len && p->s[i + 1] == ')') {
                    char *text = astrndup(p->s + from + 2, i - from - 2);
                    p->have_tk = 0;
                    for (int k = p->pos; k < i + 2; k++)
                        if (p->s[k] == '\n')
                            p->line++;
                    p->pos = i + 2;
                    p->last_end = p->pos;
                    return text;
                }
                break;
            }
            depth--;
        } else if (c == '\'' || c == '"') {
            int k = c == '\'' ? skip_squote(p->s, i + 1, p->len) : skip_dquote(p->s, i + 1, p->len);
            if (k < 0)
                break;
            i = k;
            continue;
        }
        i++;
    }
    if (i >= p->len && !p->final)
        incomplete(p, "math");
    else
        syntax(p, "%s", "parse error in (( ))");
    return 0;
}

static struct node *parse_function_body(struct parser *p, struct node *f)
{
    skip_newlines(p);
    const char *saved = p->open;
    p->open = "function";
    int start = peek(p)->start;
    struct node *body = parse_command_unit(p);
    p->open = saved;
    if (!body) {
        if (p->status == P_OK)
            unexpected(p);
        return 0;
    }
    f->b = body;
    f->init = astrndup(p->s + start, p->last_end - start);     /* the body's source */
    return f;
}

static struct node *parse_simple(struct parser *p);

static struct node *parse_command_unit(struct parser *p)
{
    if (p->status != P_OK)
        return 0;
    while (try_alias(p))
        ;
    struct ptok *t = peek(p);
    struct node *n = 0;
    if (t->t == T_LPAREN) {
        if (t->start + 1 < p->len && p->s[t->start + 1] == '(') {
            int start = t->start;
            char *text = read_arith(p, start);
            if (!text)
                return 0;
            n = mk(p, N_ARITH);
            n->var = text;
            n->text = astrndup(p->s + start, p->last_end - start);
        } else {
            consume(p);
            n = mk(p, N_SUBSHELL);
            n->a = parse_list(p, 0, "subsh");
            if (p->status != P_OK)
                return 0;
            if (peek(p)->t != T_RPAREN) {
                p->open = "subsh";
                unexpected(p);
                return 0;
            }
            consume(p);
        }
    } else if (t->t == T_WORD && !t->quoted) {
        const char *w = t->w;
        if (!strcmp(w, "{")) {
            consume(p);
            n = mk(p, N_GROUP);
            p->brace_depth++;
            n->a = parse_list(p, T_BRACE, "cursh");
            p->brace_depth--;
            if (p->status != P_OK || !expect_word(p, "}", "cursh"))
                return 0;
        } else if (!strcmp(w, "if")) {
            n = parse_if(p);
        } else if (!strcmp(w, "while") || !strcmp(w, "until")) {
            n = parse_while(p, w[0] == 'u');
        } else if (!strcmp(w, "for")) {
            n = parse_for(p);
        } else if (!strcmp(w, "case")) {
            n = parse_case(p);
        } else if (!strcmp(w, "repeat")) {
            n = parse_repeat(p);
        } else if (!strcmp(w, "[[")) {
            n = parse_cond(p);
        } else if (!strcmp(w, "function")) {
            consume(p);
            if (peek(p)->t != T_WORD) {
                unexpected(p);
                return 0;
            }
            n = mk(p, N_FUNC);
            n->var = peek(p)->w;
            consume(p);
            if (peek(p)->t == T_LPAREN) {
                consume(p);
                if (peek(p)->t != T_RPAREN) {
                    unexpected(p);
                    return 0;
                }
                consume(p);
            }
            return parse_function_body(p, n);
        } else if (!strcmp(w, "then") || !strcmp(w, "elif") || !strcmp(w, "else") ||
                   !strcmp(w, "fi") || !strcmp(w, "do") || !strcmp(w, "done") ||
                   !strcmp(w, "esac") || !strcmp(w, "}") || !strcmp(w, "in") ||
                   !strcmp(w, "]]")) {
            unexpected(p);
            return 0;
        } else {
            return parse_simple(p);
        }
        if (!n)
            return 0;
        parse_trailing_redirs(p, n);
        return p->status == P_OK ? n : 0;
    } else if (t->t == T_WORD || t->t == T_REDIR) {
        return parse_simple(p);
    }
    return n;
}

static struct node *parse_simple(struct parser *p)
{
    struct node *n = mk(p, N_CMD);
    int start = peek(p)->start;
    struct strv words = { 0 };
    struct assign **atail = &n->assigns;
    int cmd_pos = 1, check = 1, follow = 0;
    for (;;) {
        if (p->status != P_OK)
            return 0;
        if (check && try_alias(p)) {
            if (p->alias_next)
                follow = 1;         /* alias ending in a space: check the next word too */
            continue;
        }
        struct ptok *t = peek(p);
        if (t->t == T_REDIR) {
            if (parse_redir(p, &n->redirs) < 0)
                return 0;
            continue;
        }
        if (t->t != T_WORD)
            break;
        if (!t->quoted && p->brace_depth && !strcmp(t->w, "}") )
            break;
        if (cmd_pos && is_assignment_word(t->w)) {
            *atail = parse_assign(p, t->w);
            atail = &(*atail)->next;
            consume(p);
            continue;
        }
        if (words.n == 0 && peek(p)->t == T_WORD && !t->quoted) {
            /* name () body: a function definition */
            char *name = t->w;
            int save_pos = p->pos;
            consume(p);
            if (peek(p)->t == T_LPAREN) {
                int lp = peek(p)->start;
                int k = lp + 1;
                while (k < p->len && (p->s[k] == ' ' || p->s[k] == '\t'))
                    k++;
                if (k < p->len && p->s[k] == ')') {
                    p->pos = k + 1;
                    p->have_tk = 0;
                    p->last_end = p->pos;
                    struct node *f = mk(p, N_FUNC);
                    f->var = name;
                    return parse_function_body(p, f);
                }
            }
            (void)save_pos;
            strv_push(&words, name);
            cmd_pos = 0;
            check = follow;
            follow = 0;
            continue;
        }
        strv_push(&words, t->w);
        consume(p);
        cmd_pos = 0;
        check = follow;
        follow = 0;
    }
    n->words = words.v;
    n->nwords = words.n;
    if (!n->nwords && !n->assigns && !n->redirs)
        return 0;
    n->text = astrndup(p->s + start, p->last_end - start);
    return n;
}

/* One complete command: everything up to an unquoted newline. */
struct node *parse_command(struct parser *p)
{
    if (p->status != P_OK)
        return 0;
    p->nexpanded = 0;
    skip_newlines(p);
    if (p->status != P_OK)
        return 0;
    if (peek(p)->t == T_EOF) {
        p->status = P_EOF;
        return 0;
    }
    struct node *list = 0;
    for (;;) {
        struct node *n = parse_and_or(p);
        if (!n) {
            if (p->status == P_OK)
                unexpected(p);
            return 0;
        }
        struct ptok *t = peek(p);
        int sep = 0;
        if (t->t == T_AMP) {
            struct node *bg = mk(p, N_BG);
            bg->a = n;
            bg->text = n->text;
            bg->line = n->line;
            n = bg;
            consume(p);
            sep = 1;
        } else if (t->t == T_SEMI) {
            consume(p);
            sep = 1;
        }
        list = seq(p, list, n);
        if (p->status != P_OK)
            return 0;
        t = peek(p);
        if (t->t == T_NL) {
            consume(p);
            break;
        }
        if (t->t == T_EOF)
            break;
        if (!sep) {
            unexpected(p);
            return 0;
        }
    }
    if (p->nhd && p->status == P_OK) {
        if (p->final)
            read_heredocs(p);
        else
            incomplete(p, "heredoc");
    }
    return p->status == P_OK ? list : 0;
}

enum pstatus parse_check(const char *src, const char **what, char *err, int errlen)
{
    struct amark m = amark();
    struct parser p;
    parser_init(&p, src);
    while (parse_command(&p))
        ;
    enum pstatus st = p.status;
    if (what)
        *what = p.open;
    if (err && errlen)
        snprintf(err, errlen, "%s", p.err);
    parser_free(&p);
    arelease(m);
    return st == P_EOF ? P_OK : st;
}
