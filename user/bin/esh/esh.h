/*
 * esh: the Eunuchs shell, in the spirit of zsh.
 *
 * Modules:
 *   main.c      start-up, rc files, the read-eval loop, scripts and -c
 *   util.c      memory (heap helpers and a per-command arena), string
 *               buffers and vectors, character classes, sorting, patterns
 *   parse.c     lexer and recursive-descent parser into an AST (aliases,
 *               here-documents, incomplete-input detection for PS2)
 *   expand.c    brace, tilde, parameter, command and arithmetic expansion,
 *               field splitting, globbing (with **), quote removal
 *   arith.c     $(( )) and (( )): C-like 64-bit integer expressions
 *   exec.c      runs the AST: pipelines, redirections, functions, builtins,
 *               external commands, [[ ]], xtrace, traps
 *   jobs.c      job control: process groups, the terminal, notifications
 *   vars.c      variables, arrays, locals, special parameters, environment
 *   builtins.c  the built-in commands
 *   history.c   the history list, ~/.esh_history, !-expansion
 *   prompt.c    prompt escapes (%n %~ %F{red} %(?..) ...)
 *   zle.c       the line editor: keys, rendering, search, suggestions
 *   complete.c  tab completion with listings and a menu
 *   highlight.c syntax highlighting for the line editor
 */
#ifndef ESH_H
#define ESH_H

#include <stddef.h>
#include <stdint.h>
#include <termios.h>
#include <unistd.h>

#define ESH_VERSION "1.0"

/* ---------------- util.c ---------------- */

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* The arena holds everything that lives for one command line (AST, words,
 * expansion results); it is released after the line runs. */
struct amark {
    void *chunk;
    size_t used;
};
void *amalloc(size_t n);
char *astrdup(const char *s);
char *astrndup(const char *s, size_t n);
struct amark amark(void);
void arelease(struct amark m);

/* Growable string (heap). */
struct sb {
    char *s;
    int len, cap;
};
void sb_putc(struct sb *b, int c);
void sb_putn(struct sb *b, const char *s, int n);
void sb_puts(struct sb *b, const char *s);
void sb_printf(struct sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_reset(struct sb *b);
void sb_free(struct sb *b);
char *sb_str(struct sb *b);             /* NUL-terminated, never NULL */
char *sb_adup(struct sb *b);            /* copy into the arena */

/* Growable vector of strings (arena), NULL-terminated. */
struct strv {
    char **v;
    int n, cap;
};
void strv_push(struct strv *sv, char *s);

static inline int is_digit(int c) { return c >= '0' && c <= '9'; }
static inline int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline int is_alnum(int c) { return is_alpha(c) || is_digit(c); }
static inline int is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
static inline int is_blank(int c) { return c == ' ' || c == '\t'; }
static inline int is_name_start(int c) { return is_alpha(c) || c == '_'; }
static inline int is_name_char(int c) { return is_alnum(c) || c == '_'; }
static inline int to_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static inline int to_upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
int is_name(const char *s);             /* a valid variable name */
int is_number(const char *s);           /* optional '-' and digits */

void sort_strs(char **v, int n);
int strcasecmp_(const char *a, const char *b);
int strncasecmp_(const char *a, const char *b, size_t n);
int str_width(const char *s, int n);    /* display columns (UTF-8, no escapes) */
int utf8_prev(const char *s, int pos);  /* start of the character before pos */
int utf8_next(const char *s, int len, int pos);
int edit_distance(const char *a, const char *b);
char *read_file(const char *path, int *len);    /* heap, NUL-terminated */
const char *signame(int sig);           /* "INT" */
int sig_by_name(const char *s);         /* "INT", "SIGINT", "2" -> 2; -1 if unknown */
const char *sigdesc(int sig);           /* "segmentation fault" (zsh style, lower case) */
long long now_ms(void);                 /* monotonic */

/* Patterns (glob syntax: * ? [...] [!...], with \ escaping one character). */
#define PM_CASEFOLD 1
int pmatch(const char *pat, const char *s, int flags);
int pattern_has_magic(const char *pat);
/* Longest/shortest prefix/suffix match lengths; -1 if none. */
int pmatch_prefix(const char *pat, const char *s, int longest);
int pmatch_suffix(const char *pat, const char *s, int longest);
char *unescape_pattern(const char *pat);    /* drop the backslashes (arena) */

/* ---------------- options ---------------- */

enum {
    OPT_AUTOCD, OPT_AUTOPUSHD, OPT_PUSHDIGNOREDUPS, OPT_PUSHDSILENT, OPT_CORRECT,
    OPT_NOMATCH, OPT_GLOB, OPT_GLOBDOTS, OPT_NOCASEGLOB, OPT_HISTIGNOREDUPS,
    OPT_HISTIGNORESPACE, OPT_NOTIFY, OPT_XTRACE, OPT_ERREXIT, OPT_NOUNSET,
    OPT_INTERACTIVECOMMENTS, OPT_IGNOREEOF, OPT_SHWORDSPLIT, OPT_PROMPTSP,
    OPT_AUTOSUGGEST, OPT_HIGHLIGHT, OPT_PROMPTMARKS, OPT_BEEP, OPT_EXTENDEDHIST,
    OPT_CHECKJOBS, OPT_HUP, OPT_AUTOLIST, OPT_AUTOMENU, OPT_BANGHIST, OPT_VERBOSE,
    OPT_INTERACTIVE, OPT_MONITOR, OPT_LOGIN,
    NOPTS
};
extern char opts[NOPTS];
extern const char *const opt_names[NOPTS];
int opt_find(const char *name);         /* ignores case and '_'; "no" prefix handled by callers */

/* ---------------- global shell state ---------------- */

struct sh {
    int last_status;
    unsigned cmdsubs;           /* command substitutions run (x=$(cmd) takes their status) */
    int interactive;            /* reading commands from a terminal */
    int tty_fd;                 /* the terminal (job control, zle), or -1 */
    pid_t pgid;                 /* the shell's own process group */
    pid_t last_bg;
    int in_child;               /* we are a forked subshell */
    int func_depth;
    int loop_depth;
    int breaking, continuing;   /* pending break/continue levels */
    int returning;              /* return was executed */
    int exiting;                /* exit was executed */
    int exit_status;
    int errexit_ok;             /* inside if/while conditions, && ||, ! */
    long long start_ms;
    const char *script_name;    /* $0 */
    int lineno;
    int term_ok;                /* the terminal answers escape queries */
    int cols, rows;
    char *home;
};
extern struct sh sh;

const char *err_prefix(void);   /* "esh" or "script:12" */
void sh_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sh_exit(int status) __attribute__((noreturn));    /* runs the EXIT trap, saves history */
void out_flush(void);           /* flush the stdio buffer (before fork, fd juggling) */

/* ---------------- parse.c ---------------- */

enum ntype {
    N_CMD, N_PIPE, N_AND, N_OR, N_NOT, N_SEQ, N_BG, N_SUBSHELL, N_GROUP, N_IF,
    N_WHILE, N_UNTIL, N_FOR, N_ARITHFOR, N_CASE, N_FUNC, N_ARITH, N_COND, N_TIME,
    N_REPEAT,
};

enum redir_op {
    R_IN,       /* <  */
    R_OUT,      /* >  */
    R_CLOBBER,  /* >| */
    R_APPEND,   /* >> */
    R_RDWR,     /* <> */
    R_DUPIN,    /* <& */
    R_DUPOUT,   /* >& */
    R_HEREDOC,  /* << <<- */
    R_HERESTR,  /* <<< */
    R_ALLOUT,   /* &> (also >& word) */
    R_ALLAPP,   /* &>> */
};

struct redir {
    enum redir_op op;
    int fd;                 /* target descriptor */
    char *word;             /* raw word (file, fd number, here-string) */
    char *heredoc;          /* here-document body */
    int hd_quoted;          /* delimiter was quoted: no expansion */
    struct redir *next;
};

struct assign {
    char *name;
    char *index;            /* raw subscript for a[i]=v, or NULL */
    int append;             /* += */
    char *value;            /* raw word, or NULL for arrays */
    char **array;           /* raw words of name=(...) */
    int narray;
    int is_array;
    struct assign *next;
};

/* Condition trees for [[ ]]. */
enum cop { C_AND, C_OR, C_NOT, C_UNARY, C_BINARY, C_WORD };
struct cnode {
    enum cop op;
    char *opname;           /* "-f", "==", ... */
    char *l, *r;            /* raw words */
    struct cnode *a, *b;
};

struct casei {
    char **pats;            /* raw pattern words */
    int npats;
    struct node *body;
    int fallthrough;        /* ;& */
    struct casei *next;
};

struct node {
    enum ntype t;
    int line;
    char *text;             /* source text (job names, function bodies) */
    struct redir *redirs;
    /* N_CMD */
    char **words;
    int nwords;
    struct assign *assigns;
    /* children: AND/OR/SEQ (a,b), NOT/BG/SUBSHELL/GROUP/TIME (a),
     * IF (a cond, b then, c else), WHILE/UNTIL (a cond, b body),
     * FOR (b body), FUNC (b body) */
    struct node *a, *b, *c;
    /* N_PIPE */
    struct node **cmds;
    int ncmds;
    int pipe_stderr;        /* bitmask: stage i uses |& */
    /* N_FOR / N_FUNC / N_ARITH / N_ARITHFOR / N_REPEAT */
    char *var;              /* for variable, function name, arithmetic text, repeat count */
    char **list;            /* for words (NULL: "$@") */
    int nlist;
    char *init, *cond, *step;  /* for (( ; ; )) */
    struct casei *cases;
    char *subject;          /* case word */
    struct cnode *cn;
};

enum pstatus { P_OK, P_EOF, P_ERROR, P_INCOMPLETE };

struct parser {
    char *s;                /* source (heap; alias expansion may replace it) */
    int len, pos;
    int line;
    enum pstatus status;
    const char *open;       /* innermost open construct, for PS2 ("for", "dquote") */
    char err[160];
    /* pending here-documents */
    struct redir *hd[16];
    int hd_strip[16];
    int nhd;
    /* aliases already expanded at a position (no recursion) */
    struct {
        int pos;
        char name[32];
    } expanded[32];
    int nexpanded;
    int alias_next;         /* the last alias ended in a space */
    int no_alias;
    int final;              /* no more input will come (scripts, -c) */
    /* one token of lookahead */
    struct ptok {
        int t, start, end;
        char *w;            /* word text (arena) */
        int quoted;
        int rop, rfd, strip;
    } tk;
    int have_tk;
    int cond_mode;          /* inside [[ ]]: < and > are words */
    int brace_depth;        /* open { } groups: an unquoted } word closes one */
    int last_end;           /* end of the last consumed token */
};

void parser_init(struct parser *p, const char *src);
void parser_free(struct parser *p);
/* Parse one complete command (up to an unquoted newline or the end).
 * Returns NULL with status P_EOF at the end, or on error/incomplete. */
struct node *parse_command(struct parser *p);
/* Check whether src is complete; *what gets the open construct for PS2. */
enum pstatus parse_check(const char *src, const char **what, char *err, int errlen);
int is_keyword(const char *w);
int is_assignment_word(const char *w);
struct assign *parse_assignment(const char *raw);  /* NULL on error */
int builtin_declare(char **raw, int n);             /* local/typeset/export/readonly */
int is_declare_builtin(const char *name);
int assign_do(struct assign *a, int flags);         /* flags: V_EXPORT, ... ; -1 on error */
/* Find the end of a $( ) / ` ` / ${ } starting at s[i] (the char after the opener). */
int skip_cmdsub(const char *s, int i, int len);
int skip_braces(const char *s, int i, int len);

/* ---------------- expand.c ---------------- */

#define EX_SPLIT    1       /* field splitting of $(...) (and $x with shwordsplit) */
#define EX_GLOB     2       /* filename generation */
#define EX_TILDE    4
#define EX_PATTERN  8       /* produce a pattern: quoted metacharacters escaped */
#define EX_ASSIGN   16      /* ~ after : and = too */
#define EX_NOBRACE  32
#define EX_FULL     (EX_SPLIT | EX_GLOB | EX_TILDE)
#define EX_WORD     (EX_TILDE)

/* Expand raw words into fields; returns -1 on error (message printed). */
int expand_words(char **raw, int n, struct strv *out, int flags);
/* Expand one word into a single string (no splitting or globbing). */
char *expand_str(const char *raw, int flags);       /* NULL on error */
char *expand_heredoc(const char *body);
int brace_expand(const char *w, struct strv *out);
int glob_expand(const char *pattern, struct strv *out);  /* number of matches */
char *tilde_expand_name(const char *user);          /* "~user" home or NULL */
char *run_cmdsub(const char *src, int *status);     /* output, trailing newlines removed */
char *quote_word(const char *s);                    /* minimal quoting for re-input */
char *escape_for_completion(const char *s);         /* backslash-escape specials */
extern int expand_error;

/* ---------------- arith.c ---------------- */

int arith_eval(const char *expr, long long *result);   /* -1 on error (printed) */

/* ---------------- vars.c ---------------- */

#define V_EXPORT   1
#define V_READONLY 2
#define V_ARRAY    4
#define V_INTEGER  8
#define V_LOWER    16
#define V_UPPER    32
#define V_SPECIAL  64       /* computed on read */
#define V_UNSET    128      /* declared local but unset */

struct var {
    char *name;
    char *val;              /* scalar value */
    char **arr;             /* array elements (heap) */
    int narr;
    int flags;
    int level;              /* function depth that owns it */
    struct var *shadow;     /* outer variable hidden by a local */
    struct var *next;       /* hash chain */
};

void vars_init(char **envp);
struct var *var_lookup(const char *name);
const char *var_get(const char *name);          /* NULL if unset */
int var_set(const char *name, const char *val, int flags);   /* -1 if readonly */
int var_set_array(const char *name, char **elems, int n, int append);
int var_set_elem(const char *name, long idx, const char *val);
int var_unset(const char *name);
int var_local(const char *name);                /* create at the current function level */
void vars_pop_level(int level);
char **var_environ(void);                       /* arena */
void vars_list(int (*filter)(struct var *), int export_form);
int var_names(const char *prefix, struct strv *out);
void var_export(const char *name);
/* Positional parameters. */
struct posargs {
    char **v;
    int n;
};
extern struct posargs pos;
void pos_set(char **v, int n);                  /* copies */
struct posargs pos_save(void);
void pos_restore(struct posargs p);

/* ---------------- exec.c ---------------- */

#define EF_FORKED 1     /* already in a child: exec without another fork */
#define EF_BG     2     /* run in the background */
int exec_node(struct node *n, int flags);
int run_string(const char *src, const char *name);  /* parse and run, command by command */
int run_file(const char *path, int must_exist);
int run_function(const char *name, char **argv, int argc);
struct func {
    char *name;
    char *body;             /* source text of the body */
    struct func *next;
};
struct func *func_lookup(const char *name);
void func_define(const char *name, const char *body);
int func_remove(const char *name);
void funcs_list(void);
int func_names(const char *prefix, struct strv *out);
char *path_lookup(const char *cmd);             /* arena; NULL if not found */
int path_commands(const char *prefix, struct strv *out);
void path_rehash(void);
pid_t sh_fork(void);                            /* flushes stdio first */
void child_reset_signals(void);
void xtrace(char **argv, int argc, struct assign *as);
const char *suggest_command(const char *name);  /* "did you mean" */
extern int pending_traps;
void run_traps(void);
int trap_set(int sig, const char *action);      /* sig 0: EXIT */
void trap_list(void);
const char *trap_get(int sig);
void signals_init_interactive(void);
extern volatile int got_sigint, got_sigchld, got_sigwinch;

/* ---------------- jobs.c ---------------- */

#define MAXPROCS 16
struct job {
    int id;
    pid_t pgid;
    pid_t pids[MAXPROCS];
    int status[MAXPROCS];
    char done[MAXPROCS];
    int nprocs;
    int stopped;            /* all live processes stopped */
    int notified;
    int fg;
    char *text;             /* heap */
    long long started;
    struct termios tmodes;  /* saved when it stops */
    int has_tmodes;
    int disowned;
};
struct job *job_create(const char *text);
void job_add_proc(struct job *j, pid_t pid);
int job_wait_fg(struct job *j);                 /* returns the status for $? */
void job_launch_done(struct job *j, int bg);
int jobs_reap(int report);                      /* nonblocking; returns notifications printed */
int jobs_count(void);
int jobs_pending_report(void);
struct job *job_find(const char *spec);         /* %n %+ %- %str %?str; NULL: current */
struct job *job_by_index(int i);
int job_continue(struct job *j, int fg);
void job_print(struct job *j, int longfmt, int pids);
void job_free(struct job *j);
int jobs_any_stopped(void);
void jobs_hup_all(void);
void term_take(void);                           /* give the terminal back to the shell */
void term_save_shell_modes(void);
const char *job_state_text(struct job *j);
int job_index(struct job *j);
#define MAXJOBS 32
extern struct job *jobtab[MAXJOBS];
extern int cur_job, prev_job;                   /* indexes or -1 */

/* ---------------- builtins.c ---------------- */

typedef int (*builtin_fn)(int argc, char **argv);
struct builtin {
    const char *name;
    builtin_fn fn;
    const char *help;
};
const struct builtin *builtin_lookup(const char *name);
extern const struct builtin builtins[];
int builtin_names(const char *prefix, struct strv *out);
struct alias {
    char *name, *value;
    struct alias *next;
};
const char *alias_get(const char *name);
int alias_names(const char *prefix, struct strv *out);
int do_cd(const char *dir, int print);
char *pwd_tilde(const char *path);              /* /home/guest/x -> ~/x (arena) */

/* ---------------- history.c ---------------- */

struct hist_ent {
    char *line;
    long long when;         /* seconds since the epoch */
};
void hist_init(void);
void hist_add(const char *line);
int hist_count(void);
int hist_base(void);                            /* number of the first entry */
const char *hist_get(int i);                    /* 0-based index into the list */
int hist_expand(const char *line, char **out);  /* 0 none, 1 expanded, -1 error */
void hist_save(void);
void hist_clear(void);
const char *hist_last_word(int back, int nth);  /* for Alt-. (arena) */

/* ---------------- prompt.c ---------------- */

/* Expand prompt escapes; *width gets the display width of the last line.
 * Escape sequences are omitted when color is 0. Result is in the arena. */
char *prompt_expand(const char *fmt, int *width, int color);
const char *user_name(void);
const char *host_name(void);
int user_home(const char *user, char *buf, int len);    /* from /etc/passwd */
int passwd_users(const char *prefix, struct strv *out);

/* ---------------- zle.c ---------------- */

/* Read a line with editing; returns heap string without the newline,
 * NULL on EOF. ps is the expanded prompt. */
char *zle_read(const char *ps, int pw, const char *rps, int rpw);
int zle_init(void);                             /* raw-mode capable terminal? */
void term_query_size(void);
void zle_print_above(const char *s);            /* used by notifications */
extern int zle_active;
extern int zle_cmd_ran;                         /* for the OSC 133 D mark */
void tty_raw(void);
void tty_cooked(void);

/* ---------------- complete.c / highlight.c ---------------- */

struct cand {
    char *word;             /* inserted text (already escaped) */
    char *disp;             /* shown in listings */
    char *desc;             /* option descriptions */
    int kind;               /* for colors */
    int suffix;             /* character appended on a unique match (0: none) */
};
enum { K_PLAIN, K_DIR, K_EXEC, K_BUILTIN, K_ALIAS, K_FUNC, K_KEYWORD, K_VAR, K_OPT, K_JOB, K_USER };
struct cands {
    struct cand *v;
    int n, cap;
    int start, end;         /* the word being completed, in the buffer */
    char *prefix;           /* the word as typed (unescaped) */
};
void complete_collect(const char *buf, int len, int cursor, struct cands *out);
/* Per-byte attributes for the buffer: written into attr[0..len). */
enum { H_NONE, H_CMD, H_BADCMD, H_KEYWORD, H_BUILTIN, H_ALIAS, H_FUNC, H_PATH, H_QUOTE,
       H_VAR, H_COMMENT, H_OP, H_OPTION, H_GLOB, H_ASSIGN, H_SUBST };
void highlight(const char *buf, int len, unsigned char *attr);
const char *highlight_sgr(int h);
/* Scan for the command word around position pos: returns 1 if pos is in
 * command position; *cmd gets the current command name (arena) or NULL. */
int command_context(const char *buf, int pos, char **cmd, int *argi);

#endif
