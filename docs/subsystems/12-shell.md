# The shell: esh

`esh` (`user/bin/esh/`) is the Eunuchs shell, modelled on zsh: zsh's syntax and expansion rules, with a line editor in the style of ZLE plus zsh-autosuggestions and zsh-syntax-highlighting. It is `/bin/esh`, and `/bin/sh` is a hard link to it. `init` starts it as a login shell on the console, `su` runs the shell named in `/etc/passwd` (esh for both users), and `rshd` runs `esh -i` on each connection.

The first half of this document is a user guide (what you can type). The second half is about how esh is built, for people changing it.

## Responsibilities and scope

What esh does:

- **The language.**
  - Simple commands, pipelines (`|`, `|&`) and lists (`;`, `&`, `&&`, `||`).
  - Subshells and groups.
  - `if`, `while`, `until`, `for` (both forms), `case`, `repeat`, functions, `[[ ]]`, `(( ))` and `time`.
  - Here-documents and here-strings, aliases, arrays and local variables.
- **Expansion, as zsh does it.**
  - Brace, tilde, parameter (with zsh's `${(flags)...}`), command and arithmetic expansion, then globbing (including `**`).
  - Unquoted `$var` is *not* split into words. Unquoted expansions that come out empty disappear.
- **Job control.** Process groups, the terminal's foreground group, `^Z`/`fg`/`bg`/`jobs`/`kill %n`, and zsh-style status messages. Notifications are printed as soon as a background job finishes, even while you are typing.
- **Interactive use.** Emacs-style editing, multi-line buffers, history, and other features:
  - history: shared via `~/.esh_history`, with prefix search on Up/Down, incremental search, and `!`-expansion;
  - gray autosuggestions from history;
  - syntax highlighting;
  - context-aware Tab completion with listings and a selectable menu;
  - prompts with zsh's `%` escapes and a right prompt;
  - spelling suggestions for unknown commands, `autocd`, and a directory stack.
- **Scripts.** `esh file args`, `esh -c 'cmds'`, commands from a pipe, and `sh` compatibility mode.

What it does not do (see Limitations for the details):

- No regular expressions (`[[ =~ ]]`), no extended globbing (`^`, `#`, `(a|b)`), no glob qualifiers.
- No `select`, coprocesses or process substitution (`<(...)`).
- No vi mode or `bindkey`.
- No floating-point arithmetic.

## Using esh

### Starting it

| Invocation | Meaning |
|---|---|
| `esh` | Interactive if stdin and stderr are terminals; otherwise read commands from stdin, line by line. |
| `esh -c 'commands' [name [args...]]` | Run a string. `name` becomes `$0`, `args` the positional parameters. |
| `esh file [args...]` | Run a script. |
| `esh -i` | Interactive even without a terminal: prompts, but no job control or editor (`rshd` uses this). |
| `esh -l`, or `argv[0]` starting with `-` | Login shell: also reads `/etc/esh_profile` and `~/.esh_profile`. |
| `-x -e -u -v -f`, `-o option` | As with `set` (trace, errexit, nounset, verbose, no globbing; any option by name). |
| `esh --version` | Print the version. |

Invoked as `sh`, esh turns on `shwordsplit`, so POSIX scripts that rely on `$var` splitting behave as expected.

Interactive shells read `/etc/eshrc` and then `~/.eshrc` (for root, `/.eshrc`). The shipped `/etc/eshrc` turns on `autocd autopushd pushdsilent pushdignoredups correct histignoredups histignorespace`, sets `PROMPT='%F{green}%n@%m%f:%F{blue}%~%f%# '`, sets `RPROMPT` to show a non-zero exit status in red, and defines a few aliases (`ls` with `--color=auto`, `ll`, `la`, `l`, `..`, `...`, `h`, `j`, `d`).

### Syntax

```
list       := and-or ((';' | '&' | newline) and-or)*
and-or     := pipeline (('&&' | '||') newline* pipeline)*
pipeline   := ['!'] ['time'] command (('|' | '|&') newline* command)*
command    := simple | compound redirection* | function
simple     := (assignment | redirection)* word (word | redirection)*
compound   := '{' list '}'  |  '(' list ')'
            | if list then list (elif list then list)* [else list] fi
            | while list do list done  |  until list do list done
            | for name [in word...] ; do list done  |  for name (word...) do list done
            | for (( expr ; expr ; expr )) do list done
            | case word in [(] pattern (| pattern)* ) list (;; | ;&) ... esac
            | repeat count do list done  |  repeat count command
            | (( arithmetic ))  |  [[ condition ]]
function   := name () command  |  function name [()] command
assignment := name=word | name+=word | name[index]=word | name=( word... ) | name+=( word... )
```

- **Pipelines.** As in zsh, the last stage of a pipeline runs in the shell itself when it is a builtin, a function or a compound command. So `echo a b | read x y` and `cmd | while read l; do n=$((n+1)); done` leave `x`, `y` and `n` set afterwards.
- **Newlines and continuation.** A newline ends a command except where more is obviously needed: after `|`, `&&`, `||`, inside quotes, open braces and keywords. Interactively, esh then asks for more with `PS2` (default `%_> `, so you see `for> `, `quote> `, `heredoc> `, `cmdsubst> ` and so on).
- **Braces.** `{ echo }` works without a semicolon before `}`, as in zsh.
- **`&|` and `&!`** run in the background, like `&`.
- **`;&`** in `case` falls through into the next branch.
- **Comments.** `#` starts a comment at the beginning of a word, interactively too.

#### Redirections

| Form | Meaning |
|---|---|
| `[n]< file`, `[n]> file`, `[n]>> file`, `[n]<> file` | Read; write (truncate); append; read-write. |
| `[n]>\| file`, `[n]>! file` | Write, even when clobbering (there is no `noclobber`; same as `>`). |
| `[n]>& m`, `[n]<& m`, `[n]>&-` | Duplicate or close a descriptor. |
| `&> file`, `>& file`, `&>> file` | Both stdout and stderr. |
| `<< WORD`, `<<- WORD` | Here-document; `<<-` strips leading tabs. Quoting any part of `WORD` turns off expansion in the body. |
| `<<< word` | Here-string (the word plus a newline). |

Redirections apply to builtins and functions too (they are undone afterwards). `exec >file` with no command redirects the shell itself.

#### Conditions: `[[ ... ]]`

- **Unary.**
  - file tests: `-e -f -d -c -p -s -r -w -x -u -g -O -G -N`;
  - `-n` and `-z` for strings;
  - `-t fd`;
  - `-o option` (true if the option is on);
  - `-v name` (true if the variable is set).
- **Binary.**
  - `==`, `=` and `!=` match against a *pattern* (`[[ $f == *.c ]]`); quote the right side to compare literally;
  - `<` and `>` compare strings;
  - `-eq -ne -lt -le -gt -ge` compare arithmetic expressions;
  - `-nt`, `-ot` and `-ef` compare files.
- **Combining.** `!`, `&&`, `||` and parentheses. Words inside `[[ ]]` are not split or globbed.

`test` and `[` are also builtins, with the POSIX rules (`-a`, `-o`, no patterns).

### Expansion

Each word is expanded in this order:

1. **Brace expansion.**
   - Lists: `{a,b,c}`, nesting allowed.
   - Sequences: `{1..10}`, `{01..10}` (zero-padded), `{a..e}`, `{10..1..2}`.
   - A brace with neither a comma nor `..` stays as it is.
2. **Tilde.**
   - `~` is `$HOME`, `~user` that user's home from `/etc/passwd`.
   - `~+` is `$PWD`, `~-` is `$OLDPWD`.
   - In assignments it also works after `=` and `:`.
3. **Parameter, command and arithmetic expansion**, left to right (described below).
4. **Word splitting.**
   - Only the results of unquoted `$(...)` and backquotes are split on `IFS`.
   - Unquoted `$var` is split only with `setopt shwordsplit` (on when invoked as `sh`).
   - Unquoted arrays produce one word per element.
5. **Filename generation.**
   - `*`, `?`, `[...]` and `[!...]`/`[^...]`, plus `**/` for any number of directories.
   - Dot-files match only if the pattern starts with `.`, or with `globdots`.
   - Results are sorted.
   - A pattern that matches nothing is an error: `esh: no matches found: *.xyz`, and the command does not run. With `unsetopt nomatch` the pattern stays as it is.
   - Characters that came from a variable or a substitution are never treated as wildcards.
6. **Quote removal.** `'...'` is literal. `"..."` allows `$`, `` ` `` and `\` escapes. `$'...'` understands `\n \t \e \xHH \uHHHH \0NNN \cX`.

An unquoted expansion that comes out empty is removed: `echo $unset x` passes one argument. `""` and `"$unset"` are kept as empty arguments.

#### Parameters

| Form | Value |
|---|---|
| `$name`, `${name}` | The value (for an array, its elements). |
| `$name[i]`, `${name[i]}` | Element `i` (1-based; negative counts from the end). For a scalar, the `i`th character. |
| `${name[i,j]}` | A slice. |
| `${name[@]}`, `${name[*]}`, `"$@"`, `"$*"` | All elements; inside quotes `@` keeps them separate words, `*` joins them with the first `IFS` character. |
| `${#name}`, `$#name` | Length: characters of a scalar, elements of an array. |
| `${+name}` | 1 if set, 0 if not. |
| `${name:-word}` `${name-word}` | `word` if unset (or empty, with `:`). |
| `${name:=word}` `${name=word}` | Same, and assign it. |
| `${name:+word}` `${name+word}` | `word` if set (and not empty). |
| `${name:?msg}` | Error (and exit a script) if unset or empty. |
| `${name#pat}` `${name##pat}` | Remove the shortest / longest matching prefix. |
| `${name%pat}` `${name%%pat}` | Remove the shortest / longest matching suffix. |
| `${name/pat/rep}` `${name//pat/rep}` | Replace the first / every match; `/#pat` anchors at the start, `/%pat` at the end. |
| `${name:off}` `${name:off:len}` | Substring (0-based); for arrays, elements. |
| `${(flags)name}` | `U` upper case, `L` lower, `C` capitalize words, `j:sep:` join, `s:sep:` split, `f` split on newlines, `o`/`O` sort ascending / descending, `u` unique, `q` quote for re-input, `@` keep elements separate inside quotes. |
| `${$(cmd)...}`, `${${x#a}%b}`, `${"..."...}` | A nested expansion can take the place of the name (as in `${(s:,:)$(cmd)}`). |

The operators apply to each element of an array.

Special parameters:

- **Status and processes.** `$?` is the last status: `128 + signal` for a killed command, 148 for a stopped one. `$$` is the shell's pid and `$!` the last background pid. `$pipestatus` is an array with every stage's status.
- **Arguments and options.** `$#`, `$1`... and `$@`/`$*` are the positional parameters, `$0` the script or shell name. `$-` holds the single-letter options.
- **Shell variables.**
  - computed on every read: `RANDOM` (0-32767), `SECONDS` (since start-up; assignable), `EPOCHSECONDS`, `LINENO`;
  - the terminal size: `COLUMNS` and `LINES`;
  - set at start-up: `PPID`, `UID`, `EUID`, `SHLVL` (incremented in each nested shell), `HOST` (from `/etc/hostname`), and `ESH_VERSION`.

#### Command substitution and arithmetic

- **Command substitution.** `$(commands)` and `` `commands` `` run in a subshell; trailing newlines are removed. Inside double quotes the result stays one word.
- **Arithmetic.** `$(( expr ))`, `(( expr ))` (status 0 if non-zero), `let`, array subscripts and `for ((;;))` all evaluate C-like 64-bit integer expressions:
  - operators: `+ - * / % **`, the bitwise and logical operators, comparisons, `?:`, the comma operator, `++`/`--`, and assignment operators (`= += -=` ...);
  - numbers: decimal, `0x` hex and `base#digits` (as in `2#1010`);
  - variables are read by name, without `$`.

### Variables

- **Assignment.** `name=value`, `name+=more` (append), `arr=(a b c)`, `arr+=(d)`, `arr[3]=x`. Assignments before a command (`LANG=C cmd`) apply to that command only.
- **Declarations.** `typeset`/`declare`/`local` declare variables with attributes:
  - `-x` export, `-r` read-only, `-i` integer (values are evaluated arithmetically), `-a` array, `-l`/`-u` lower/upper case;
  - `-g` makes a declaration global; `-p` prints.
- **Scope.** Inside a function, `typeset` and `local` create function-local variables. Scoping is dynamic, as in zsh: functions called from there see the local.
- **Exporting.** `export` and `readonly` act globally. `integer` is `typeset -i`.
- **Unsetting.** `unset name` removes a variable; `unset -f name` removes a function.
- **Environment.** Exported arrays reach programs joined with `:`.

### Functions, aliases and lookup

- **Functions.** `name() { ... }` or `function name { ... }`. Arguments are `$1`... and `return [n]` sets the status. Functions may recurse up to 100 deep. `functions` lists them and `unfunction` removes them.
- **Hook functions**, as in zsh:
  - `precmd` runs before each prompt;
  - `preexec` runs before each command line, with the line as `$1`;
  - `chpwd` runs after the directory changes.
- **Aliases.**
  - `alias name='text'` replaces `name` when it is the first word of a command; `alias` with no arguments lists them.
  - An alias whose value ends in a space also expands the next word.
  - Aliases are expanded when a line is parsed, so one defined on a line takes effect from the next line.
- **Lookup order** for a command name: functions, then builtins, then `$PATH`. `command name` skips functions; `builtin name` runs only a builtin; `noglob cmd` turns off globbing for that command; `exec cmd` replaces the shell.
- **Describing names.** `whence`/`type`/`which` say what a name is, in zsh's three styles: `type ll` prints `ll is an alias for ls -l`; `which cd` prints `cd: shell built-in command`; `whence ls` prints `/bin/ls`.
- **Unknown commands.** If a name isn't found you get `esh: command not found: foo`. With `correct` on, esh also prints `esh: did you mean 'foo2'?` when another command is within one or two edits (`sl` → `ls`, `ehco` → `echo`).

### Builtins

`help` lists them all with one-line descriptions (`help name` for one).

| Group | Builtins |
|---|---|
| Directories | `cd [dir \| - \| +n \| -n \| old new]`, `pushd [dir \| +n]`, `popd [+n]`, `dirs [-v] [-c]`, `pwd` |
| Output and input | `echo [-neE]` (interprets escapes, like zsh), `print [-nrlNPs] [-u fd]` (`-P`: prompt escapes, `-l`: one per line, `-s`: into history), `printf format args` (`%s %b %q %c %d %i %u %o %x %X`, widths, `*`; the format is reused for extra arguments), `read [-rs] [-p prompt] [-t secs] [-n n] [-A array] [-d delim] [name?prompt] [names]` |
| Variables and options | `typeset`, `declare`, `local`, `export`, `readonly`, `integer`, `unset`, `set [-euxvfC] [-o opt] [-A arr] [-- args]`, `setopt`, `unsetopt`, `shift [n]`, `getopts` |
| Control | `source`/`.` (searches `$PATH` if the file isn't in the current directory), `eval`, `exec`, `exit`/`logout`/`bye`, `return`, `break [n]`, `continue [n]`, `trap`, `true`, `false`, `:`, `let` |
| Lookup | `alias`, `unalias`, `type`, `whence [-vcpa]`, `which`, `command [-vV]`, `builtin`, `functions`, `unfunction`, `rehash`/`hash` |
| Jobs | `jobs [-lp]`, `fg [%job]`, `bg [%job]`, `wait [%job \| pid]`, `kill [-sig \| -s sig] %job \| pid...`, `kill -l`, `disown [%job]` |
| Conditions | `test`, `[` |
| Other | `history [n \| -c \| -n]`, `umask [-S] [mode]`, `help` |
| zsh compatibility | `autoload`, `compinit`, `zstyle`, `emulate` (accepted and ignored, so zsh start-up files don't fail) |

**Traps.**
- `trap 'commands' SIG...` runs the commands when a signal arrives. Use `EXIT` (or `0`) for exit, and names with or without `SIG` or numbers for signals.
- `trap '' SIG` ignores a signal, `trap - SIG` restores the default, and `trap` alone lists the traps.
- Trap commands run between commands, never in the middle of one, including while you are typing at the prompt.

### Jobs

Interactive shells put every pipeline in its own process group and give it the terminal while it runs in the foreground. Messages follow zsh:

```
% sleep 100
^Z
esh: suspended  sleep 100
% bg
[1]  + continued  sleep 100
% jobs -l
[1]  + 8 running    sleep 100
% kill %1
[1]  + terminated  sleep 100
% segv null
esh: segmentation fault (core dumped)  segv null
```

- **Job specs.** `%n` (job number), `%%`/`%+` (the current job, marked `+`), `%-` (the previous one, marked `-`), `%str` (the job whose command starts with `str`), `%?str` (whose command contains `str`), or a pid.
- **Notifications.** A finished background job is reported immediately (`notify`), even while you are typing. esh then redraws your line under the message.
- **Exiting.** With stopped jobs, the first `exit` (or `^D`) warns `esh: you have suspended jobs.` and the second one exits. When an interactive shell exits, its jobs get `SIGHUP` (`hup`).
- **Interrupts.** `^C` interrupts the foreground job. When a job dies of `SIGINT`, the rest of the command line and any enclosing loop stop too: `while true; do sleep 1; done` ends with one `^C`.

### Line editing

Keys are emacs-style, as in zsh's default keymap:

| Keys | Action |
|---|---|
| Left/Right, `^B`/`^F` | Character left/right. Right, `^F`, End or `^E` at the end of the line accepts the gray suggestion. |
| `^A`/`^E`, Home/End | Start/end of the (current line of the) buffer. (Under `make run`, QEMU's escape key is `^^`, so `^A` reaches esh.) |
| `Alt-B`/`Alt-F`, `^Left`/`^Right` | Word left/right (`Alt-F` at the end accepts one word of the suggestion). Words are letters, digits and the characters in `$WORDCHARS` (default `*?_-.[]~=&;!#$%^(){}<>`, without `/`). |
| Backspace, `^D`/Delete | Delete left / under the cursor. `^D` on an empty line exits (unless `ignoreeof`); at the end of a non-empty line it lists completions. |
| `^W`, `Alt-Backspace` / `Alt-D` | Kill the word left / right. |
| `^K` / `^U` | Kill to the end of the line / the whole line. |
| `^Y`, then `Alt-Y` | Yank the last kill, then cycle through older ones (8 kept; consecutive kills join). |
| `^T`, `Alt-U`/`Alt-L`/`Alt-C` | Transpose characters; upper-case, lower-case or capitalize a word. |
| `^_`, `^X ^U` | Undo. |
| Up/Down | History entries that begin with what is before the cursor (`up-line-or-beginning-search`); in a multi-line buffer, move between lines first. |
| `^P`/`^N`, PageUp/PageDown, `Alt-<`/`Alt->` | Previous/next history entry; first/last entry. |
| `^R` / `^S` | Incremental search backward/forward: type to narrow, `^R` again for the next match, Enter runs it, `^G` cancels, any editing key accepts the match for editing. |
| `Alt-.` | Insert the last word of the previous command; repeat for older commands. |
| Tab, Shift-Tab | Complete (see below). |
| `Alt-Enter` | Insert a newline (edit a multi-line command). |
| `^Q`, `Alt-Q` | Push the line aside: it comes back at the next prompt. |
| `^V` | Insert the next key literally. |
| `^L` | Clear the screen. |
| `^C`, `^G` | Abandon the line (it stays visible) and start a new one. |
| Enter | Run the line (or ask for more with `PS2` if it is incomplete). |

- **Pasting.** Pasted text is inserted as is, newlines included, when the terminal supports bracketed paste.
- **Autosuggestions.** As you type, the most recent history entry that starts with what you typed is shown in gray after the cursor; Right or End takes it.
- **Highlighting.** The line is colored as you type:
  - green: a command, builtin, alias or function that exists; bold red: a name that isn't one (unless it's the start of one);
  - yellow: keywords and quoted strings; cyan: variables; magenta: command substitutions; blue: wildcards; gray: comments;
  - underlined: arguments that are existing files.

  Turn it off with `unsetopt highlight` (suggestions: `unsetopt autosuggest`).

**Completion** looks at where the cursor is:

| Context | Candidates |
|---|---|
| Command position | Builtins, aliases, functions, keywords, programs in `$PATH`, and directories (with `autocd`). A word containing `/` completes to executables and directories. |
| `$name`, `${name` | Variable names. |
| `~name` | User names from `/etc/passwd`. |
| After `<`, `>`, ... | File names. |
| `-x` after a known command | Options with descriptions (`ps`, `ls`, `grep`, `nc`, `ping`, `netstat`, `wc`, `head`, `tail`, `more`/`less`, `vi`, `httpd`, `stress`, `kill`, and the builtins); subcommands of `profile` and `dbg`. The table also describes the `+N`/`+/pat` arguments of `vi`, `view` and `emacs`, but those entries are never offered: options are only completed for a word that starts with `-`. |
| `cd`, `pushd`, `rmdir` | Directories. |
| `fg`, `bg`, `disown`, `wait`, `kill %` | Job specs, described by state and command. |
| `kill -` / `trap ... ` | Signal names. |
| `setopt` / `unsetopt` | Options that are off / on. |
| `unset`, `export`, `local`, `typeset`, `read`, ... | Variable names. |
| `unalias`, `unfunction`, `type`, `which`, `help`, `su`, `strace` | Aliases; functions; commands; builtins; users; commands. |
| Anywhere else | File names (hidden files only if you typed the dot). |

How completion behaves:

- If nothing matches exactly, esh tries again ignoring case.
- A unique match is inserted with a space, or with `/` for a directory. Special characters in file names are escaped with backslashes.
- If there are several matches, Tab inserts their longest common prefix and lists them under the line (`autolist`), colored by kind.
- A second Tab starts the menu (`automenu`):
  - Tab/Shift-Tab and the arrow keys move through the candidates (inserting each);
  - Enter keeps the current one;
  - `^G` restores what you had typed;
  - any other key keeps the candidate and continues editing.
- Listings longer than the screen show a window of rows that follows the selection.

### History

- **Storage.** Each command line is appended to `$HISTFILE` (`~/.esh_history`) as soon as it is entered, so later shells see it and nothing is lost if the system goes down.
- **Size.** The last `$HISTSIZE` (1000) entries are kept in memory. The file is trimmed to `$SAVEHIST` (1000) entries at start-up when it has grown half again past that.
- **Format.** Multi-line commands are stored with backslash-newline, as zsh does. With `setopt extendedhistory`, each entry gets a `: <time>:0;` prefix.
- **Filtering.** `histignoredups` skips a command identical to the previous one, and `histignorespace` skips commands typed with a leading space.
- `history [n]` lists the last 16 (or `n`) entries with their numbers; `history -c` clears the list.

**History expansion** (`banghist`) happens before a line is parsed, and the expanded line is printed before it runs:

- **Events.** `!!` is the previous command, `!n` command number `n`, `!-n` the `n`th previous, `!str` the last command starting with `str`, `!?str?` the last one containing it.
- **Words.** `!$` is the last word of the previous command, `!^` the first argument, `!*` all arguments. `!!:n`, `:n-m` and `:$` select words.
- **Modifiers.** `:h` (dirname), `:t` (basename), `:r` (remove the extension), `:e` (the extension), `:u` and `:l` (case).
- **Quick substitution.** `^old^new` reruns the previous command with `old` replaced by `new`.
- **Quoting.** `!` is literal inside single quotes, before a space, `=` or `(`, and as `\!`.

### Prompts

`PROMPT` (or `PS1`), `RPROMPT` (or `RPS1`, shown at the right edge of the first line), `PS2` and `PS4` (the `xtrace` prefix) use zsh's escapes:

| Escape | Value |
|---|---|
| `%n`, `%m`, `%M` | User; host up to the first `.`; the full host. |
| `%~`, `%/` (or `%d`), `%c` (or `%.`), `%N~` | Current directory with `~` for home; without `~`; last component; last `N` components. |
| `%#` | `#` for root, `%` otherwise. |
| `%?`, `%j`, `%L`, `%h` (or `%!`), `%i`, `%N` | Last status; number of jobs; `$SHLVL`; next history number; line number; script or function name. |
| `%T`, `%*`, `%t` (or `%@`), `%D`, `%w`, `%W`, `%D{fmt}` | Time and date (`fmt` takes `%H %M %S %I %p %Y %y %m %d %e %a %b %k %l`). Times are UTC. |
| `%F{color}` `%f`, `%K{color}` `%k` | Foreground/background color: a name (`black red green yellow blue magenta cyan white default`), a number 0-255, or `#rrggbb`. |
| `%B` `%b`, `%U` `%u`, `%S` `%s` | Bold, underline, standout (reverse) on/off. |
| `%E` | Clear to the end of the line. |
| `%(x.true.false)` | Conditional. `x` is `?` (status is N, default 0), `#` (uid is N, default 0), `j` (at least N jobs, default 1), `L` (SHLVL at least N), `/` (directory depth at least N), or `T`/`t` (the hour or minute is N). |
| `%{...%}` | Raw escape sequences (no width). |
| `%%`, `%)` | Literal `%` and `)`. |

When the previous command's output didn't end with a newline, esh marks the spot with a reverse-video `%` and starts the prompt on a fresh line (`promptsp`).

### Options

`setopt name...` and `unsetopt name...` (case and underscores don't matter; a `no` prefix inverts). `setopt` alone lists the options that are on.

| Option | Default | Meaning |
|---|---|---|
| `autocd` | off (on in `/etc/eshrc`) | A directory name alone as a command `cd`s there. |
| `autopushd`, `pushdsilent`, `pushdignoredups` | off (on in `/etc/eshrc`) | `cd` pushes the old directory on the stack; `pushd`/`popd` don't print the stack; no duplicates on the stack. |
| `correct` | off (on in `/etc/eshrc`) | Suggest a similar command when one isn't found. |
| `nomatch` | on | A glob that matches nothing is an error. |
| `glob`, `globdots`, `nocaseglob` | on, off, off | Filename generation; let wildcards match leading dots; ignore case. |
| `shwordsplit` | off (on as `sh`) | Split unquoted parameter expansions on `IFS`. |
| `histignoredups`, `histignorespace` | off (on in `/etc/eshrc`) | See History. |
| `extendedhistory`, `banghist` | off, on | Time stamps in the history file; `!` expansion. |
| `notify`, `checkjobs`, `hup` | on | Report background jobs at once; warn before exiting with stopped jobs; `SIGHUP` jobs on exit. |
| `autolist`, `automenu` | on | List ambiguous completions; menu on the second Tab. |
| `autosuggest`, `highlight` | on | Gray suggestions; syntax highlighting. |
| `promptsp`, `promptmarks` | on | The `%` partial-line marker; OSC 133 marks (when the terminal answers queries). |
| `beep` | on | Ring the bell on errors in the editor. |
| `ignoreeof` | off | `^D` on an empty line doesn't exit. |
| `interactivecomments` | on | `#` comments at the prompt. |
| `errexit`, `nounset`, `xtrace`, `verbose` | off | `set -e`, `-u`, `-x`, `-v`. |
| `interactive`, `monitor`, `login` | (set at start-up) | Interactive; job control; login shell. |

## Concepts and data structures

The rest of this document is about the implementation.

### Source map

| File | Role |
|---|---|
| `esh.h` | Every shared type and prototype: the arena, `struct sb`/`strv`, the AST, `struct var`, `struct job`, options, the global `struct sh`. |
| `main.c` | Start-up (`main`, `init_vars`, `take_terminal`), rc files, the read-eval loop `repl` (continuation lines, history expansion, hooks), `run_stdin`, `sh_error`/`sh_exit`. |
| `util.c` | `xmalloc` family; the arena (`amalloc`, `amark`, `arelease`); string buffers and vectors; character classes; merge sort; UTF-8 stepping; `edit_distance`; signal names; glob pattern matching (`pmatch`, prefix/suffix variants). |
| `parse.c` | Lexer (`lex`), lookahead (`peek`/`consume`), here-document bodies, alias splicing (`try_alias`), recursive-descent parser (`parse_command` and below), `[[ ]]` conditions, `parse_check` (is the input complete?), the scanning helpers `skip_cmdsub`/`skip_braces` shared with the expander. |
| `expand.c` | `expand_words`/`expand_str`/`expand_heredoc`; brace expansion; tilde; `${...}` with flags and operators; command substitution (`run_cmdsub`); globbing (`glob_expand`, with `**`); `quote_word`, `escape_for_completion`. |
| `arith.c` | `arith_eval`: precedence-climbing evaluator with short-circuit (`noeval`) and assignment. |
| `exec.c` | `exec_node` (the tree walker); simple commands, pipelines, subshells, background jobs; redirections with save/restore; assignments; functions (`run_function`); `[[ ]]` evaluation; `run_string`/`run_file`; `$PATH` lookup and its cache; spelling suggestions; signal handlers and traps. |
| `jobs.c` | The job table, waiting (`job_wait_fg`), reaping and notifications (`jobs_reap`), job specs (`job_find`), `fg`/`bg` (`job_continue`), terminal ownership (`term_take`). |
| `vars.c` | Variable hash table, arrays, attributes, locals (`var_local`, `vars_pop_level`), specials, `var_environ`, positional parameters. |
| `builtins.c` | Options (`opts[]`, `opt_names[]`), aliases, the directory stack and `do_cd`, every builtin, the `builtins[]` table and `help`. |
| `history.c` | The history list, the history file, `!`-expansion. |
| `prompt.c` | `prompt_expand`; user and host names, home directories (`/etc/passwd`, `/etc/hostname`). |
| `zle.c` | Terminal modes, key decoding, terminal queries (`term_query_size`), the editor state `z`, rendering (`refresh_full`), editing commands, suggestions, history search, incremental search, completion listing and menu, `zle_read`. |
| `highlight.c` | A forgiving scanner over the edit buffer (`scan`), syntax highlighting, and `command_context` for completion. |
| `complete.c` | Candidate generation (`complete_collect`) and the option description table. |

Supporting files: `user/etc/eshrc`, `user/etc/hostname`, `user/etc/passwd`. In the `Makefile`, `UMULTI`/`MULTI_RULE` link every `user/bin/NAME/*.c` into `/bin/NAME`, and `FS_FILES` has `/bin/sh=@/bin/esh`.

### Memory: heap and arena

- **The heap** holds what outlives a command: variables, functions (their source text), aliases, jobs, history, the editor's buffers.
- **The arena** holds everything that lives for one command line: tokens, the syntax tree, expanded words, globbing results, completion candidates.
  - It is a stack of 32 KiB chunks (`amalloc`). `amark()` records the top and `arelease()` frees back to it.
  - `repl` and `run_string` mark before each command and release after it.
  - `refresh_full` does the same around each redraw, so typing doesn't grow memory.
  - Nested runs (functions, `eval`, `source`, traps) release only their own allocations, so they never invalidate the caller's.
  - Rule: never keep an arena pointer in a heap structure that outlives the current command. `zle.c`'s `cands_free` clears the candidate vector for this reason.

### Words stay raw until they run

The parser keeps each word exactly as typed, quotes included (`node->words`). Expansion happens when the command runs, so `false; echo $?`, `x=1; echo $x` and loops work naturally.

### The syntax tree

`struct node` has a type (`N_CMD`, `N_PIPE`, `N_AND`, `N_OR`, `N_NOT`, `N_SEQ`, `N_BG`, `N_SUBSHELL`, `N_GROUP`, `N_IF`, `N_WHILE`, `N_UNTIL`, `N_FOR`, `N_ARITHFOR`, `N_CASE`, `N_FUNC`, `N_ARITH`, `N_COND`, `N_TIME`, `N_REPEAT`) and generic fields:

- `a`/`b`/`c` for children;
- `words`, `assigns` and `redirs` for commands;
- `cmds` for pipelines;
- `var`, `list`, `init`, `cond` and `step` for loops;
- `cases` for `case`, `cn` for a `[[ ]]` condition tree.

It also keeps `text`, the source of the node (job names, `verbose`), and `line` (`$LINENO`, error messages). For `N_FUNC`, `init` holds the source text of the body.

**Functions are stored as source text** (`struct func`, on the heap) and parsed again each time they are called. That keeps the tree in the arena, and `which f`/`functions` print the definition.

### Fields: value and pattern

The expander builds each output word as a `struct field` with two buffers:

- the value;
- a pattern copy in which every quoted (or variable-derived) wildcard is escaped with a backslash.

`glob` records whether an unquoted wildcard was seen. `forced` records that quoting was seen, so the word survives even if it ends up empty. `end_field` decides between globbing the pattern, pushing the value, or (for `EX_PATTERN`, used by `case`, `[[ == ]]` and `${x#pat}`) returning the pattern itself.

### Variables

- **Storage.** A 128-bucket hash table of `struct var`: name; a scalar `val` or an array `arr`/`narr`; `flags` (`V_EXPORT`, `V_READONLY`, `V_ARRAY`, `V_INTEGER`, `V_LOWER`, `V_UPPER`, `V_SPECIAL`, `V_UNSET`); `level`; `shadow`.
- **Locals.** `var_local` inserts a new `struct var` in front of the old one in the same chain (`shadow` points to it) at `level = sh.func_depth`. `run_function` calls `vars_pop_level` on return, which removes every variable at that level and puts the shadowed ones back.
- **Specials** (`V_SPECIAL`) are computed by `special_get`: `RANDOM`, `SECONDS`, `EPOCHSECONDS`, `LINENO`, `COLUMNS`, `LINES`.
- **The PATH cache.** Setting `PATH` clears it (`path_rehash`).

### Jobs

`jobtab[32]` holds `struct job`:

- `id` (slot + 1), `pgid`, and up to 16 processes, each with a pid, a raw wait status and `done` (0 running, 1 exited, 2 stopped);
- `stopped`, `notified`, `fg`, `disowned`, the command `text`;
- the terminal modes saved when it stopped (`tmodes`), restored by `fg`.

`cur_job`/`prev_job` are the `+` and `-` jobs. A job lives from launch until it is reaped and reported.

### The global state

`struct sh sh` (in `main.c`) holds:

- `last_status`, `interactive`, `in_child`;
- the terminal: `tty_fd`, `pgid`, `term_ok` (the terminal answers queries), `cols`/`rows`;
- control-flow counters: `func_depth`, `loop_depth`, `breaking`, `continuing`, `returning`, `exit_status`, `errexit_ok`;
- `script_name`, `lineno`.

`opts[]` holds the options.

## How it works

### Start-up

1. **Options.** `set_defaults`. If the program is invoked as `sh`, turn on `shwordsplit`.
2. **Arguments.** Parse the flags.
3. **Variables.** `init_vars`:
   - import the environment and add the specials;
   - increment `SHLVL`;
   - fill in `USER`, `LOGNAME`, `HOME` (from `/etc/passwd` when unset), `SHELL`, `HOST`, `PWD`, `PPID`, `UID`, `EUID`, `PS2`, `PS4`, `TERM`.
4. **Non-interactive input.** `-c` runs the string, a file argument runs the script, and stdin that isn't a terminal goes to `run_stdin`. Each then exits through `sh_exit`.
5. **Interactive set-up.**
   - `take_terminal`: wait until in the foreground (sending `SIGTTIN` to its own group while not), ignore the job-control signals and `SIGTERM`, catch `SIGINT` and `SIGCHLD` without `SA_RESTART`, `setpgid(0, 0)`, take the terminal, and save its modes.
   - `zle_init` queries the terminal size.
6. **Start-up files.** The profile files (login shells), `/etc/eshrc`, `~/.eshrc`, then the history file.
7. **The loop.** `repl`.

### The read-eval loop

For each command (`repl`):

1. Mark the arena.
2. Reap jobs and print notifications.
3. Run pending traps and the `precmd` hook.
4. Expand `PROMPT` and `RPROMPT`, and read a line (`zle_read`; or print the prompt and read plainly without the editor).
5. While `parse_check` says the text is incomplete, read another line with `PS2`, replacing `%_` with the open construct.
6. Apply history expansion (printing the result if it changed anything), add the line to the history, and run `preexec`.
7. `run_string`, then release the arena.

`run_string` parses and runs one complete command (up to a newline) at a time. An alias or function defined on one line is therefore usable on the next line of the same script. It stops early if `return`, `break` or `continue` is pending, or after `^C`.

### Lexing and parsing

**Tokens.** `lex` produces one token of lookahead (`p->tk`):

- operators;
- redirections, with their file descriptor: up to two digits right before `<` or `>`;
- words.

**Words.** A word runs to the next unquoted metacharacter. On the way it skips `'...'`, `"..."`, `` `...` ``, `$(...)`, `$((...))`, `${...}` and `$'...'`, using `skip_cmdsub`/`skip_braces`, which track nesting and quotes. `skip_cmdsub` also counts `case`/`esac` so that a case pattern's `)` does not end the substitution.

- **Arrays.** A word that begins like an assignment and reaches `=(` swallows the parenthesized list, so `a=(x "y z")` stays one word. `parse_assign` splits the list later with a sub-parser.
- **Inside `[[`.** `cond_mode` makes `<` and `>` words.
- **`((`.** In command position, `read_arith` reads up to the matching `))`.

**Incomplete input.** Running out of input inside any construct sets `P_INCOMPLETE` with `p->open` naming it (`quote`, `dquote`, `cmdsubst`, `if`, `then`, `for`, `heredoc`, `pipe`, `cmdand`...). Interactively that leads to a `PS2` prompt. For scripts and `-c` (`final`), the parser reports a syntax error instead (`esh: parse error near `fi'`).

**Here-documents.** A `<<` redirection is queued (`p->hd`). When `consume` passes the next newline token, `read_heredocs` takes the following lines as the body, up to the delimiter (with leading tabs stripped for `<<-`). A quoted delimiter sets `hd_quoted`.

**Aliases.** `try_alias` replaces a word in command position with its value, by rebuilding the parser's buffer (`p->s`).

- Positions before the word stay valid, so recorded text spans still work.
- `p->expanded` remembers which alias was expanded at which position, so `alias ls='ls -F'` doesn't loop.

**Grammar.** The parser is plain recursive descent: `parse_command` → `parse_and_or` → `parse_pipeline` → `parse_command_unit` (compound commands and keywords) → `parse_simple`.

- `parse_simple` collects assignments (before the first word), redirections and words.
- It recognizes `name ()` as a function definition.
- It stops at an unquoted `}` when a group is open (`brace_depth`).

### Expansion

`expand_words` brace-expands each raw word, then runs `expand_part` over it and `end_field` at the end. `expand_part` walks the characters:

- **Quoting.** Quote characters switch modes: unquoted, `DQ` (inside `"..."`) and `HD` (a here-document body).
- **Backslashes.** In the quoted modes, `\` escapes only `$`, `` ` ``, `"` (in `DQ`), `\` and newline.
- **Expansions.** `$` goes to `expand_dollar`. `$(` with `((` runs arithmetic; otherwise it is a command substitution. `${` goes to `expand_braced`, `` ` `` to command substitution, and a leading `~` to tilde expansion.

`expand_braced` parses, in order:

1. `(flags)`;
2. `#` (length) or `+` (is set);
3. the name, a special, or a nested expansion;
4. a `[subscript]`, evaluated arithmetically (with `,` for slices);
5. one operator.

Values go through `emit_values`. Inside quotes, arrays are joined, or kept apart for `@`. Unquoted, arrays become separate fields, and scalars are inserted without splitting (or split with `shwordsplit`).

**Command substitution** (`run_cmdsub`):

1. Fork, with the child's stdout connected to a pipe.
2. The child marks itself `in_child`, drops interactivity and job control, resets signals, clears its traps, and runs `run_string`.
3. The parent reads everything, waits, and sets `$?`.

**Globbing** (`glob_expand`) splits the pattern into path components and recurses (`glob_rec`):

- components without wildcards are checked with `stat`;
- `**` tries the rest at this level and in every subdirectory;
- other components are matched against `readdir` entries with `pmatch`.

`pmatch` is an iterative matcher with single-star backtracking, `[...]` classes, and backslash escapes. A `[` without its `]` is literal, and such a word isn't a glob.

### Running commands

`exec_node` walks the tree; `sh.last_status` is set after every node, then traps run and `errexit` is checked. `errexit` applies to simple commands, pipelines, subshells, `[[ ]]` and `(( ))`. It does not apply inside `if`/`while` conditions or on the left of `&&`/`||`, which `errexit_ok` tracks.

`exec_simple` handles one simple command:

1. **Leading modifiers.** Strip `noglob`, `nocorrect` and `-` from the raw words.
2. **Declarations.** `typeset`, `local`, `export`, `readonly`, `declare` and `integer` get their raw words (`builtin_declare`), so `local a=(x "y z")` and `local x=$(cmd)` are parsed as assignments rather than split.
3. **Expansion.** Expand the words. `command`, `builtin` and `exec` with arguments are modifiers.
4. **No command word.** Do the assignments in the shell, and apply the redirections for their side effects (`> file` creates `file`). The status is that of the last command substitution.
5. **Functions and builtins** run in the shell process:
   - `tmp_assign` applies prefix assignments, exporting them for functions;
   - redirections are applied with `save_fd` (each original descriptor duplicated to fd 10 or above with close-on-exec);
   - afterwards stdout is flushed, the descriptors restored, and the variables put back.
6. **External commands.**
   - The parent looks the name up first (`path_lookup`, with a 64-entry cache), so an unknown command costs no fork. It prints `command not found` (plus a suggestion with `correct`), or `cd`s for `autocd`.
   - Otherwise it forks a one-process job (`run_in_child`). The child applies redirections and assignments and calls `exec_external_now`.
   - That uses `execve` with `var_environ()`. A file that isn't a binary (`ENOEXEC`) is run as an esh script.
   - Exit statuses: 127 for not found, 126 for not executable.
7. **Already in a child** (`EF_FORKED`). A command in tail position of a subshell or pipeline stage execs directly, without another fork.

**Pipelines** (`exec_pipeline`) fork every stage. Each child:

1. joins the job's process group (and takes the terminal for a foreground job), via `child_setup`;
2. resets signals and turns off job control and interactivity;
3. connects the pipes (`|&` also connects stderr);
4. runs `exec_node(stage, EF_FORKED)`.

So builtins and functions work anywhere in a pipeline. The parent also calls `setpgid` for every child, so neither side races, then waits (`job_wait_fg`) or reports `[1] pids` for `&`.

When the last stage is a builtin, a function or a compound command (`runs_inline`), it runs in the shell instead, with fd 0 temporarily connected to the pipe, so its variable assignments stay (zsh's behavior). The forked stages then stay in the shell's own process group, and the shell keeps the terminal: `^C` reaches the shell and the stages together, the shell's loop stops, and closing the pipe ends the writers. The status is that of the last stage; the others are only in `$pipestatus`.

**Other nodes:**

- **Background lists** (`N_BG`) fork a subshell job unless the list is a single pipeline.
- **Subshells** fork (`run_in_child`) and apply their redirections inside the child.
- **Compound commands** (`if`, loops, `case`, `[[ ]]`, `(( ))`, groups) run in the shell, with their redirections saved and restored around them.
- **Loops** honor `break n`/`continue n` through `sh.breaking`/`sh.continuing`, and stop after `^C` (`loop_check`).

**Redirections** (`apply_redirs`):

- the target word is expanded first;
- here-documents and here-strings are written to a pipe if they are under 4000 bytes (the kernel pipe holds 4 KiB), otherwise to an unlinked file in `/tmp`;
- `>& word` becomes `&>` when the word is not a number or `-`.

**Functions** (`run_function`) save the positional parameters, increment `func_depth`, parse and run the body text, then pop the function's locals and restore the parameters. `return` sets `sh.returning`, which `run_string` checks after each command.

**Traps.** Signal handlers (`on_signal`) only set flags: `got_sigint`, `got_sigchld`, `pending_traps` and per-signal `trap_pending`. `run_traps` runs the actions at safe points: after each node, between commands, and from the editor when a read is interrupted. The `EXIT` trap runs in `sh_exit`, once, and never in a forked child.

### Job control

- **Foreground jobs.** `job_wait_fg` loops on `waitpid(-1, WUNTRACED)` and records each status in whichever job owns the pid, so background jobs that finish meanwhile are recorded too. It stops when the job has exited or every process has stopped. Then it:
  1. saves the job's terminal modes if it stopped;
  2. takes the terminal back (`term_take`: `tcsetpgrp` plus the shell's saved modes);
  3. sets `$pipestatus`;
  4. prints `esh: suspended  cmd` (the kernel's TTY already echoed `^Z` and a newline), or a fatal signal other than `SIGINT`/`SIGPIPE` (`esh: segmentation fault (core dumped)  cmd`).
- **`^C`.** A job killed by `SIGINT` sets `got_sigint`, as if the shell had been interrupted, so the rest of the line stops.
- **Reaping.** `jobs_reap` polls with `WNOHANG | WUNTRACED` and prints `[n]  + done       cmd` and the like for finished background jobs (then frees them), and stopped jobs not yet reported. The state text is padded to 11 columns, as zsh does.
- **Resuming.** `fg`/`bg` (`job_continue`) mark the job's stopped processes running again and send `SIGCONT` to the group. `fg` also restores the job's terminal modes and gives it the terminal.
- **Scripts.** Non-interactive shells don't create process groups (`monitor` is off), so background jobs in scripts stay in the shell's group, and `wait` still works.

### The line editor

`zle_read` puts the terminal in raw mode:

- `ICANON`, `ECHO`, `ISIG`, `IEXTEN` off;
- `ICRNL`, `IXON` off;
- output post-processing left on, so `\n` still becomes CR LF.

`^C` therefore arrives as a key rather than a signal. The terminal's modes are restored before every command runs.

**Keys.**
- `readkey` decodes escape sequences: CSI and SS3 arrows with modifiers, Home/End/Delete/PageUp/PageDown with `~` codes, Shift-Tab, bracketed paste (`ESC[200~`), and `Alt` as an `ESC` prefix.
- A lone `ESC` is recognized after 60 ms of silence.
- Bytes that arrive together are buffered (`ahead`), and the screen is only redrawn when no input is pending, so a paste or fast typing costs one redraw.

**Terminal queries.** `term_query_size` saves the cursor, moves it to 999;999, asks for its position (`ESC[6n`), and restores it.

- Other bytes read while waiting for the answer are kept as typed input.
- If the terminal answers (`sh.term_ok`), esh sets `COLUMNS`/`LINES` and the kernel's window size (`TIOCSWINSZ`, so `top` and others see it), and asks again before each prompt to follow resizes.
- If it doesn't answer within 400 ms (a pipe, or the test harness), esh uses the kernel's size and stops asking. The right prompt, `promptsp`, OSC 133 marks and bracketed paste are then off.

**Rendering** (`refresh_full`). The prompt is printed once per line (`print_prompt`, between OSC 133 `A` and `B` marks). Each refresh:

1. moves up to the prompt's last row, goes to the column after the prompt, and rewrites the whole buffer with highlighting;
2. writes the gray suggestion and clears to the end of the line;
3. writes the right prompt (only if everything fits on one row);
4. writes the area below (completion listing or search status);
5. clears to the end of the screen (`ESC[J`), and moves the cursor back to its row and column.

Positions are computed the way terminals wrap:

- one column per character (a control character shows as `^X`, two columns);
- a character placed past the last column wraps first.

When the cursor ends exactly at the margin, esh writes a newline so the terminal's cursor really moves to the next row. `z.cur_row` remembers where the cursor was left for the next refresh. The whole frame is a single `write`.

**State.** `z` holds:

- the buffer and cursor;
- the history index, and the line being edited before navigating;
- the prefix for Up/Down searches;
- the suggestion;
- completion candidates and menu state;
- incremental-search state;
- 100 undo snapshots;
- `last`/`cur`, the kind of the previous command. These let consecutive kills join, `Alt-Y` follow `^Y`, `Alt-.` repeat, Up/Down keep their prefix, and a second Tab open the menu.

**Background events.** While the editor waits for a key, `SIGCHLD` interrupts the read. If a job has something to report (`jobs_pending_report`), `notify_above`:

1. erases the prompt and buffer;
2. prints the messages in cooked mode;
3. reprints the prompt and redraws.

Pending traps run the same way.

**After Enter.** The line is redrawn without suggestion or listing, the cursor goes to its end, and a newline is written. Bracketed paste is turned off, an OSC 133 `C` mark is sent, and the terminal is restored. The `D` mark with the exit status is sent at the next prompt, and `promptsp` checks whether the cursor was left mid-line.

**Completion** (`do_complete`):

- `complete_collect` finds the word around the cursor and calls `command_context`. That function runs the highlighter's scanner over the line to decide whether the word is in command position, which command it belongs to, and whether it follows a redirection.
- `collect` then generates candidates for that context (case-insensitively if nothing matched).
- Each candidate has the text to insert (escaped), a display form, an optional description, a kind (for color), and a suffix (space or `/`).
- `build_listing` lays candidates out in columns, sorted down the columns, or one per line with descriptions. It shows a window of rows when they don't fit, and marks the menu selection in reverse video.

**Highlighting** (`highlight`) uses the same scanner. The scanner tracks command position through operators, keywords and precommands (`command`, `exec`, `time`, `strace`, `env`, ...). It also tracks redirection targets and comments. Command words are looked up (alias, function, builtin, `$PATH`), and argument words are checked with `stat` to underline existing files.

## Concurrency and locking

esh is single-threaded. The asynchronous parts are signal handlers, and they only set flags (`volatile int`); all real work happens in the main flow:

| Flag | Set by | Handled in |
|---|---|---|
| `got_sigchld` | `SIGCHLD` | `jobs_reap` (prompt, editor via `notify_above`, plain reads). |
| `got_sigint` | `SIGINT` to the shell, or a foreground job killed by `SIGINT` | `stop_requested`/`loop_check` stop lists and loops; reset before each prompt. |
| `pending_traps`, `trap_pending[]` | any trapped signal | `run_traps` after each node and in the editor. |

- **Signals.** All handlers are installed without `SA_RESTART`, so `read`, `poll` and `waitpid` return `EINTR`. Every loop around them handles that.
- **Waiting.** `job_wait_fg` uses `waitpid(-1)` and attributes the pid to its job. `run_cmdsub` waits for its own pid. Only one of them is active at a time.
- **Children.** In every child, `child_setup` resets signal dispositions; a `trap ''` stays ignored. It also clears `interactive` and `monitor`, so a child never touches the terminal's foreground group except to take it for itself.
- **Output.** stdout is buffered by libc. esh flushes it before every `fork` (`sh_fork`), before descriptors are restored after a builtin, and before error messages, so output appears in order.
- **History.** Several shells append to the same history file. Each command is written in a single `write` with `O_APPEND`, so entries don't interleave. Shells don't see each other's new entries until they start.

## Interfaces

- **For users:** everything in the first half of this document, and `help`.
- **For programs:**
  - the exit status (`$?` conventions above);
  - `SHELL=/bin/esh` and exported variables in the environment;
  - `COLUMNS`/`LINES` and the kernel window size, set when the terminal answers;
  - `esh -c` and `sh -c` for running command strings;
  - OSC 133 prompt marks for terminal emulators that support them (iTerm2, WezTerm, kitty, VS Code), which can then jump between prompts and show exit statuses.
- **Files read:** `/etc/eshrc`, `~/.eshrc`, `/etc/esh_profile` and `~/.esh_profile` (login), `$HISTFILE`, `/etc/passwd` (user names, homes), `/etc/hostname`.
- **Kernel features used:**
  - `tcsetpgrp`/`setpgid` and `TCGETS`/`TCSETS`;
  - `TIOCGWINSZ`/`TIOCSWINSZ`, and `ppoll` on the TTY (key timeouts, terminal queries);
  - `fcntl(F_DUPFD_CLOEXEC)`;
  - `wait4` with `WUNTRACED`;
  - `SIGCHLD` delivery.

## Debugging and observability

- **`set -x`** (or `esh -x script`) prints each command after expansion to stderr, prefixed with `PS4` (`+%N:%i> ` gives `+script:12> `). `set -v` prints each command as it is read.
- **Introspection:**
  - `whence -v name` or `type name`, and `functions`/`which f` for definitions;
  - `alias`, `typeset -p`, `set` (all variables) and `setopt` (options that are on);
  - `jobs -l`, `dirs -v`, `history`, `trap`.
- **Errors** carry the location in scripts (`/tmp/t.sh:29: bad substitution`).
- **Kernel side:**
  - `strace esh -c '...'` (or `dbg strace 2` to follow forks) shows the system calls;
  - `/proc/<pid>/status` shows each process's pgid and session;
  - the monitor's `tty` command shows the foreground group, the termios flags (raw while editing) and queued input.
- **Memory.** libc's `malloc` aborts with a message and a `dbg_mark` on heap corruption (`malloc: free of a pointer malloc did not return`). That message usually means an arena pointer outlived its command; see the arena rule above.
- **Testing.** `make test`'s "interactive shell" stage drives esh over the serial console. `tools/run_tests.py` strips escape sequences (`ESCAPES`) before matching output, and logs the stripped text in `build/test-logs/interactive shell.log`. The harness doesn't answer `ESC[6n`, so esh runs with the defaults for unknown terminals.

## Limitations and known issues

- **Not implemented:**
  - regular expressions (`[[ x =~ re ]]` reports an error);
  - extended globbing and glob qualifiers;
  - `select`, coprocesses, process substitution, `noclobber`;
  - vi mode, `bindkey`, `zstyle` (accepted, ignored), `fc`, floating point;
  - named directories (`~name` is a user only), `CDPATH`, `ulimit`, `times`.
- **Width assumptions.** Display width is one column per code point: wide East Asian characters and combining marks misplace the cursor. Prompts are measured on their last line, so a prompt wider than the terminal confuses the editor.
- **Times are UTC.** Prompt and history times ignore time zones (the libc has no `TZ` support).
- **Parsing.** Command substitutions are found by scanning for the matching parenthesis, with a heuristic for `case` patterns inside `$(...)`. Unusual nesting, such as an unbalanced `)` in a comment inside `$(...)`, can end the substitution early.
- **`$(...)` with large output** is read entirely into memory.
- **Here-documents over 4000 bytes** need a writable `/tmp`.
- **Window size.** Resizes are noticed only at the next prompt, and only if the terminal answers position queries. There is no `SIGWINCH` from the serial console.
- **History** is read once at start-up. Commands from other shells running at the same time are not seen until the next shell starts.
- **Function calls re-parse the body** every time, which costs a little in tight loops.
- **Job control in pipelines inside functions:** a function's pipelines run as separate jobs. A function as a whole cannot be suspended with `^Z`; only the command currently running in it can.
- **`^Z` on a pipeline whose last stage runs in the shell** (`cmd | while read ...`) stops the forked stages, but the shell keeps waiting on the pipe. `^C` then ends the loop, and the stopped stages are reported as a suspended job.

## Potential improvements

1. **`SIGWINCH` from the console.** Have the kernel's TTY layer turn a terminal size report into `SIGWINCH`, or poll in the editor, so resizes redraw immediately. Impact: low. Effort: small.
2. **Regular expressions for `[[ =~ ]]`.** The libc now has POSIX `regcomp`/`regexec` (extended syntax with `REG_EXTENDED`, [10](10-user-space.md#regular-expressions-regexh)), so this is mostly wiring: compile the right-hand side, and set `$MATCH`/`$match` (zsh) from `pmatch`. Impact: medium. Effort: small.
3. **Extended globbing and qualifiers.**
   - Patterns: `^pat`, `pat~exclude`, `(a|b)`, `#`/`##`.
   - Qualifiers: `*(/)`, `*(.)`, `*(om[1,3])`.

   These are among zsh's best-loved features. Impact: medium. Effort: medium.
4. **Cache parsed functions.** Keep each function's tree in its own arena and re-parse only when it is redefined. Impact: low. Effort: small.
5. **Shared history.** Re-read new lines from `$HISTFILE` before each prompt (`sharehistory`). Impact: low. Effort: small.
6. **More completion.** Complete `$PATH` programs' own arguments from a description file in `/etc`, hostnames for `ping`/`nc`, and pids (from `/proc`) for `kill`; offer approximate matches when there are none. Impact: medium. Effort: medium.
7. **vi mode and `bindkey`.** A key table instead of the `switch` in `handle_key`, so bindings can be changed from `~/.eshrc`. Impact: medium. Effort: medium.
8. **Wide characters.** A `wcwidth` table for the editor and for prompt widths. Impact: low. Effort: small.
9. **Time zones.** `TZ` support in libc for prompts and `date`. Impact: low. Effort: medium.
10. **Process substitution** with pipes and `/dev/fd`, which needs the kernel to provide `/dev/fd/N`. Impact: low. Effort: medium.
