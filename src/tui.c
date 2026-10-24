/*
 * tui.c - interactive timeline browser for ChronoFS (optional, ncurses).
 *
 *   chronofs tui <store> [--at WHEN]
 *
 * Reads a store directly (no mount needed) and shows three panes:
 *
 *   +-----------------------------------------------------------+
 *   | Timeline: every journal change, newest first              |
 *   +----------------------+------------------------------------+
 *   | Tree at the pressed  | Preview of the highlighted file at |
 *   | moment (dirs/files)  | that same moment                   |
 *   +----------------------+------------------------------------+
 *   | hints / messages                                            |
 *
 * Moving the timeline cursor rewinds the tree and preview to that instant.
 * `t` names the moment, `r` restores the highlighted path into the live tree
 * (through the mount when mounted, so the recovery is itself versioned).
 *
 * ncurses is optional: without it this file compiles to a small stub, so the
 * core filesystem keeps its "no dependency beyond libfuse" property.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "chronofs.h"

#ifndef HAVE_NCURSES

int cmd_tui(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    fprintf(stderr,
            "chronofs: this build has no interactive browser (ncurses missing).\n"
            "  install the ncurses development headers and rebuild, e.g.\n"
            "    Debian/Ubuntu: sudo apt install libncurses-dev\n"
            "    Fedora:        sudo dnf install ncurses-devel\n"
            "    macOS/Homebrew: brew install ncurses\n"
            "  then: make clean && make\n");
    return 2;
}

#else /* HAVE_NCURSES */

#if defined(__has_include)
#  if __has_include(<ncursesw/ncurses.h>)
#    include <ncursesw/ncurses.h>
#  else
#    include <ncurses.h>
#  endif
#else
#  include <ncurses.h>
#endif

#define PREVIEW_MAX   (256 * 1024)   /* bytes of a file shown in the preview */
#define PREVIEW_WRAP  100            /* wrap width for preview lines        */
#define PREVIEW_LINES 4096           /* cap on stored preview lines         */

struct moment {
    int64_t  ts;
    uint16_t op;
    uint64_t size;
    char    *path;
};

struct tree_item {
    char   *name;
    char    path[JPATH_MAX];
    int     is_dir;
    uint64_t size;
    int64_t mtime_ns;
    mode_t  mode;
};

struct app {
    struct store      st;
    struct moment    *mom;
    size_t            nmom;
    int               msel;
    int               tl_top;

    int64_t           t;
    char              cwd[JPATH_MAX];

    struct tree_item *items;
    size_t            nitems;
    int               isel;
    int               tr_top;
    char              filter[128];

    int               focus;         /* 0 timeline, 1 tree */
    char             *pv_raw;
    size_t            pv_len;
    int               pv_binary;
    char            **plines;
    size_t            npl;
    int               pv_top;

    char              msg[256];
    int               quit;
};

struct ui {
    WINDOW *tl, *tr, *pv;
    int     rows, cols;
};

/* ------------------------------------------------------------------ */

static const char *op_name(int op)
{
    switch (op) {
    case OP_WRITE:  return "write";
    case OP_UNLINK: return "delete";
    case OP_MKDIR:  return "mkdir";
    case OP_RMDIR:  return "rmdir";
    }
    return "?";
}

static void fmt_clock(int64_t ns, char *buf, size_t n)
{
    time_t t = (time_t)(ns / NS_PER_SEC);
    struct tm tm;
    size_t l;
    localtime_r(&t, &tm);
    l = strftime(buf, n, "%H:%M:%S", &tm);
    snprintf(buf + l, n - l, ".%03d", (int)((ns % NS_PER_SEC) / 1000000));
}

static void set_msg(struct app *a, const char *fmt, const char *arg)
{
    if (arg)
        snprintf(a->msg, sizeof(a->msg), fmt, arg);
    else
        snprintf(a->msg, sizeof(a->msg), "%s", fmt);
}

static int is_mounted(struct store *st, char *mp, size_t n)
{
    char p[PATH_MAX], line[PATH_MAX];
    int fd, mounted = 0;
    FILE *f;

    if (store_path(st, p, sizeof(p), "lock") < 0)
        return 0;
    fd = open(p, O_RDONLY);
    if (fd < 0)
        return 0;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0 && errno == EWOULDBLOCK)
        mounted = 1;
    close(fd);
    if (mounted && mp) {
        mp[0] = '\0';
        store_path(st, p, sizeof(p), "mounted");
        if ((f = fopen(p, "r"))) {
            if (fgets(line, sizeof(line), f))
                snprintf(mp, n, "%s", line);
            mp[strcspn(mp, "\n")] = '\0';
            fclose(f);
        }
    }
    return mounted;
}

static void mkdirs_for(const char *file)
{
    char tmp[PATH_MAX], *p;
    snprintf(tmp, sizeof(tmp), "%s", file);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                break;
            *p = '/';
        }
    }
}

/* ------------------------------------------------------------------ */
/* Model                                                               */
/* ------------------------------------------------------------------ */

static int cmp_moment_desc(const void *x, const void *y)
{
    int64_t a = ((const struct moment *)x)->ts;
    int64_t b = ((const struct moment *)y)->ts;
    return (a < b) - (a > b);       /* newest first */
}

static void timeline_build(struct app *a)
{
    struct jrec *recs = NULL;
    size_t n = 0, i;

    for (i = 0; i < a->nmom; i++)
        free(a->mom[i].path);
    free(a->mom);
    a->mom = NULL;
    a->nmom = 0;
    if (journal_read_all(a->st.root, &recs, &n) < 0)
        return;
    if (n) {
        a->mom = calloc(n, sizeof(*a->mom));
        if (!a->mom) {
            free(recs);
            return;
        }
        for (i = 0; i < n; i++) {
            a->mom[i].ts = recs[i].ts_ns;
            a->mom[i].op = recs[i].op;
            a->mom[i].size = recs[i].size;
            a->mom[i].path = strdup(recs[i].path);
            if (!a->mom[i].path)
                break;
        }
        a->nmom = i;
        if (a->nmom > 1)
            qsort(a->mom, a->nmom, sizeof(*a->mom), cmp_moment_desc);
    }
    free(recs);
}

/* Is path a directory at time t? (also true for a directory implied by files) */
static int dir_at(struct store *st, const char *path, int64_t t)
{
    const struct version *v = index_at(&st->idx, path, t);
    size_t pl, i;

    if (v) {
        if (v->op == OP_MKDIR)
            return 1;
        if (v->op == OP_WRITE)
            return 0;
    }
    pl = strlen(path);
    for (i = 0; i < st->idx.nall; i++) {
        struct pentry *p = st->idx.all[i];
        if (strncmp(p->path, path, pl) == 0 && p->path[pl] == '/' &&
            version_live(pentry_at(p, t)))
            return 1;
    }
    return 0;
}

static int cmp_name(const void *x, const void *y)
{
    return strcmp(*(char *const *)x, *(char *const *)y);
}

static int cmp_item(const void *x, const void *y)
{
    const struct tree_item *a = x, *b = y;
    if (a->is_dir != b->is_dir)
        return b->is_dir - a->is_dir;      /* directories first */
    return strcmp(a->name, b->name);
}

static void tree_free(struct app *a)
{
    size_t i;
    for (i = 0; i < a->nitems; i++)
        free(a->items[i].name);
    free(a->items);
    a->items = NULL;
    a->nitems = 0;
}

static void tree_reload(struct app *a)
{
    char **names = NULL;
    size_t nn = 0, nc = 0, i, dl;
    const char *prefix = strcmp(a->cwd, "/") == 0 ? "" : a->cwd;

    tree_free(a);
    a->isel = 0;
    a->tr_top = 0;
    dl = strlen(prefix);

    for (i = 0; i < a->st.idx.nall; i++) {
        struct pentry *p = a->st.idx.all[i];
        const char *rest, *slash;
        size_t len;

        if (strncmp(p->path, prefix, dl) != 0 || p->path[dl] != '/')
            continue;
        rest = p->path + dl + 1;
        if (!*rest)
            continue;
        if (!version_live(pentry_at(p, a->t)))
            continue;
        slash = strchr(rest, '/');
        len = slash ? (size_t)(slash - rest) : strlen(rest);
        if (a->filter[0] && !strcasestr(rest, a->filter))
            continue;
        if (nn == nc) {
            size_t ncn = nc ? nc * 2 : 16;
            char **nt = realloc(names, ncn * sizeof(*nt));
            if (!nt)
                break;
            names = nt;
            nc = ncn;
        }
        names[nn] = strndup(rest, len);
        if (names[nn])
            nn++;
    }
    if (nn > 1)
        qsort(names, nn, sizeof(*names), cmp_name);

    for (i = 0; i < nn; i++) {
        struct tree_item it;
        char full[JPATH_MAX];
        const struct version *v;

        if (i > 0 && strcmp(names[i], names[i - 1]) == 0) {
            free(names[i]);
            continue;
        }
        memset(&it, 0, sizeof(it));
        it.name = names[i];
        snprintf(full, sizeof(full), "%s%s%s", a->cwd,
                 strcmp(a->cwd, "/") ? "/" : "", names[i]);
        snprintf(it.path, sizeof(it.path), "%s", full);
        it.is_dir = dir_at(&a->st, full, a->t);
        v = index_at(&a->st.idx, full, a->t);
        if (v && v->op == OP_WRITE) {
            it.size = v->size;
            it.mtime_ns = v->mtime_ns;
            it.mode = v->mode;
        }
        {
            struct tree_item *ni = realloc(a->items, (a->nitems + 1) * sizeof(*ni));
            if (!ni) {
                free(it.name);
                break;
            }
            a->items = ni;
            a->items[a->nitems++] = it;
        }
    }
    free(names);
    if (a->nitems > 1)
        qsort(a->items, a->nitems, sizeof(*a->items), cmp_item);
}

static void preview_free(struct app *a)
{
    size_t i;
    for (i = 0; i < a->npl; i++)
        free(a->plines[i]);
    free(a->plines);
    a->plines = NULL;
    a->npl = 0;
    free(a->pv_raw);
    a->pv_raw = NULL;
    a->pv_len = 0;
    a->pv_binary = 0;
    a->pv_top = 0;
}

static void preview_add_line(struct app *a, const char *s, size_t len)
{
    char **np;
    char *line;

    if (a->npl >= PREVIEW_LINES)
        return;
    line = malloc(len + 1);
    if (!line)
        return;
    memcpy(line, s, len);
    line[len] = '\0';
    np = realloc(a->plines, (a->npl + 1) * sizeof(*np));
    if (!np) {
        free(line);
        return;
    }
    a->plines = np;
    a->plines[a->npl++] = line;
}

static void preview_wrap(struct app *a)
{
    size_t i = 0;
    int binary_checks = 0, binary_hits = 0;

    /* crude binary test over the sampled bytes */
    for (i = 0; i < a->pv_len; i++) {
        unsigned char c = (unsigned char)a->pv_raw[i];
        if (c == 0)
            binary_hits++;
        else if (c < 9 || (c > 13 && c < 32))
            binary_hits++;
        binary_checks++;
    }
    a->pv_binary = (a->pv_len > 0 && binary_hits * 100 > binary_checks * 5);

    if (a->pv_binary) {
        char note[128];
        snprintf(note, sizeof(note), "<binary file: %llu bytes>",
                 (unsigned long long)a->pv_len);
        preview_add_line(a, note, strlen(note));
        return;
    }

    i = 0;
    while (i < a->pv_len && a->npl < PREVIEW_LINES) {
        size_t j = i, seg = 0, linelen;
        while (j < a->pv_len && a->pv_raw[j] != '\n')
            j++;
        linelen = j - i;
        if (linelen > 0 && a->pv_raw[i + linelen - 1] == '\r')
            linelen--;
        if (linelen == 0) {
            preview_add_line(a, "", 0);
        }
        while (seg < linelen && a->npl < PREVIEW_LINES) {
            size_t take = linelen - seg;
            if (take > PREVIEW_WRAP)
                take = PREVIEW_WRAP;
            preview_add_line(a, a->pv_raw + i + seg, take);
            seg += take;
        }
        i = (j < a->pv_len) ? j + 1 : a->pv_len;
    }
}

static void preview_load(struct app *a)
{
    const struct version *v;
    uint8_t (*blocks)[HASH_LEN] = NULL;
    uint64_t size = 0, off = 0;
    size_t nb = 0;
    int r;

    preview_free(a);
    if (a->nitems == 0 || a->isel < 0 || a->isel >= (int)a->nitems)
        return;
    if (a->items[a->isel].is_dir)
        return;
    v = index_at(&a->st.idx, a->items[a->isel].path, a->t);
    if (!v || v->op != OP_WRITE)
        return;
    if ((r = bs_get_manifest(&a->st, v->manifest, &size, &blocks, &nb)) < 0) {
        set_msg(a, "cannot read manifest", NULL);
        return;
    }
    a->pv_len = size < PREVIEW_MAX ? (size_t)size : PREVIEW_MAX;
    a->pv_raw = malloc(a->pv_len + 1);
    if (!a->pv_raw) {
        free(blocks);
        return;
    }
    while (off < a->pv_len) {
        ssize_t got = bs_read_version(&a->st, NULL, size,
                                      (const uint8_t (*)[HASH_LEN])blocks, nb,
                                      a->pv_raw + off, a->pv_len - (size_t)off, (off_t)off);
        if (got <= 0)
            break;
        off += (uint64_t)got;
    }
    a->pv_len = (size_t)off;
    a->pv_raw[a->pv_len] = '\0';
    free(blocks);
    preview_wrap(a);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void ui_free(struct ui *u)
{
    if (u->tl) delwin(u->tl);
    if (u->tr) delwin(u->tr);
    if (u->pv) delwin(u->pv);
    u->tl = u->tr = u->pv = NULL;
}

static void ui_layout(struct ui *u, int rows, int cols)
{
    int tlh, bodyh, staty, treew, pvw;

    ui_free(u);
    tlh = rows / 3;
    if (tlh < 4) tlh = 4;
    if (tlh > rows - 8) tlh = rows - 8;
    if (tlh < 3) tlh = 3;
    staty = rows - 2;
    bodyh = staty - tlh;
    if (bodyh < 3) bodyh = 3;
    treew = cols / 3;
    if (treew < 20) treew = 20;
    if (treew > cols - 24) treew = cols - 24;
    if (treew < 10) treew = 10;
    pvw = cols - treew - 1;
    if (pvw < 10) pvw = 10;
    u->tl = newwin(tlh, cols, 0, 0);
    u->tr = newwin(bodyh, treew, tlh, 0);
    u->pv = newwin(bodyh, pvw, tlh, treew + 1);
    u->rows = rows;
    u->cols = cols;
}

static void draw_timeline(struct app *a, struct ui *u)
{
    WINDOW *w = u->tl;
    int h, ww, listh, r, top;

    getmaxyx(w, h, ww);
    werase(w);
    box(w, 0, 0);
    if (a->focus == 0)
        wattron(w, A_BOLD);
    mvwaddnstr(w, 0, 2, " Timeline ", ww - 4);
    if (a->focus == 0)
        wattroff(w, A_BOLD);
    mvwaddnstr(w, 0, 13, a->st.root, ww - 16);

    listh = h - 2;
    top = a->tl_top;
    if (a->msel >= 0) {
        if (a->msel < top)
            top = a->msel;
        if (a->msel >= top + listh)
            top = a->msel - listh + 1;
    }
    if (top < 0)
        top = 0;
    a->tl_top = top;

    for (r = 0; r < listh && top + r < (int)a->nmom; r++) {
        struct moment *m = &a->mom[top + r];
        char clk[16], sz[32], line[600];
        int sel = (top + r == a->msel);

        fmt_clock(m->ts, clk, sizeof(clk));
        if (m->op == OP_WRITE)
            fmt_size(m->size, sz, sizeof(sz));
        else
            snprintf(sz, sizeof(sz), "-");
        snprintf(line, sizeof(line), "%-12s %-6s %9s  %s", clk, op_name(m->op), sz, m->path);
        if (sel)
            wattron(w, A_REVERSE);
        mvwaddnstr(w, 1 + r, 1, line, ww - 2);
        if (sel)
            wattroff(w, A_REVERSE);
    }
    if (a->nmom == 0)
        mvwaddnstr(w, 1, 2, "(no history yet)", ww - 4);
    wnoutrefresh(w);
}

static void draw_tree(struct app *a, struct ui *u)
{
    WINDOW *w = u->tr;
    int h, ww, listh, r, top;
    char title[256];

    getmaxyx(w, h, ww);
    werase(w);
    box(w, 0, 0);
    snprintf(title, sizeof(title), " Tree  %s%s ", a->cwd,
             a->filter[0] ? "  (filtered)" : "");
    if (a->focus == 1)
        wattron(w, A_BOLD);
    mvwaddnstr(w, 0, 2, title, ww - 4);
    if (a->focus == 1)
        wattroff(w, A_BOLD);

    listh = h - 2;
    top = a->tr_top;
    if (a->isel < top)
        top = a->isel;
    if (a->isel >= top + listh)
        top = a->isel - listh + 1;
    if (top < 0)
        top = 0;
    a->tr_top = top;

    for (r = 0; r < listh && top + r < (int)a->nitems; r++) {
        struct tree_item *it = &a->items[top + r];
        char line[PATH_MAX + 64], sz[32];
        int sel = (top + r == a->isel);

        if (it->is_dir)
            snprintf(sz, sizeof(sz), "dir");
        else
            fmt_size(it->size, sz, sizeof(sz));
        snprintf(line, sizeof(line), "%-9s %s%s", sz, it->name, it->is_dir ? "/" : "");
        if (sel)
            wattron(w, A_REVERSE);
        if (it->is_dir)
            wattron(w, A_BOLD);
        mvwaddnstr(w, 1 + r, 1, line, ww - 2);
        if (it->is_dir)
            wattroff(w, A_BOLD);
        if (sel)
            wattroff(w, A_REVERSE);
    }
    if (a->nitems == 0)
        mvwaddnstr(w, 1, 2, "(empty)", ww - 4);
    wnoutrefresh(w);
}

static void draw_preview(struct app *a, struct ui *u)
{
    WINDOW *w = u->pv;
    int h, ww, listh, r, top;
    char title[300];

    getmaxyx(w, h, ww);
    werase(w);
    box(w, 0, 0);
    {
        char clk[16];
        fmt_clock(a->t, clk, sizeof(clk));
        if (a->nitems > 0 && a->isel >= 0 && a->isel < (int)a->nitems &&
            !a->items[a->isel].is_dir)
            snprintf(title, sizeof(title), " %s @ %s ", a->items[a->isel].name, clk);
        else
            snprintf(title, sizeof(title), " Preview @ %s ", clk);
    }
    mvwaddnstr(w, 0, 2, title, ww - 4);

    listh = h - 2;
    top = a->pv_top;
    if (top < 0)
        top = 0;
    a->pv_top = top;
    for (r = 0; r < listh && top + r < (int)a->npl; r++)
        mvwaddnstr(w, 1 + r, 1, a->plines[top + r], ww - 2);
    wnoutrefresh(w);
}

static void draw_status(struct app *a, struct ui *u)
{
    move(u->rows - 2, 0);
    clrtoeol();
    attron(A_REVERSE);
    mvprintw(u->rows - 2, 0, " %s", a->msg);
    attroff(A_REVERSE);
    move(u->rows - 1, 0);
    clrtoeol();
    printw(" Up/Down move  PgUp/PgDn page  Tab pane  Enter open  Bksp back  "
           "t tag  r restore  / filter  n now  ? help  q quit");
}

static void draw_help(struct app *a, struct ui *u)
{
    static const char *lines[] = {
        "ChronoFS timeline browser",
        "",
        "  Up / Down      move in the focused pane; the timeline cursor",
        "                 rewinds the tree and preview to that instant",
        "  PgUp / PgDn     page up / down (preview scrolls when tree focused)",
        "  Home / End     jump to newest / oldest moment",
        "  Tab            switch focus: timeline <-> tree",
        "  Enter          open the highlighted directory",
        "  Backspace      go to the parent directory",
        "  n              jump to 'now' (the live tree)",
        "  t              tag the selected moment",
        "  r              restore the highlighted file/dir at that moment",
        "  /              filter the tree by substring (empty clears)",
        "  R              reload the store from disk",
        "  ?              toggle this help",
        "  q / Esc        quit",
        "",
        "History is read-only; 'r' writes through the mount when mounted,",
        "so a recovery is recorded as a new version too.",
        "Press any key to close.",
    };
    int n = (int)(sizeof(lines) / sizeof(lines[0])), bw = 0, bh, y, x, i;
    WINDOW *w;

    for (i = 0; i < n; i++)
        if ((int)strlen(lines[i]) > bw)
            bw = (int)strlen(lines[i]);
    bw += 4;
    bh = n + 2;
    y = (u->rows - bh) / 2;
    x = (u->cols - bw) / 2;
    if (y < 0) y = 0;
    if (x < 0) x = 0;
    w = newwin(bh, bw, y, x);
    if (!w)
        return;
    box(w, 0, 0);
    mvwaddnstr(w, 0, 2, " Help ", bw - 4);
    for (i = 0; i < n; i++)
        mvwaddnstr(w, 1 + i, 2, lines[i], bw - 4);
    wrefresh(w);
    getch();
    delwin(w);
    (void)a;
}

static void draw_all(struct app *a, struct ui *u)
{
    erase();
    draw_status(a, u);          /* stdscr is the background layer */
    wnoutrefresh(stdscr);
    draw_timeline(a, u);        /* panes composite on top of it   */
    draw_tree(a, u);
    draw_preview(a, u);
    doupdate();
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static void set_time(struct app *a)
{
    if (a->msel >= 0 && a->msel < (int)a->nmom)
        a->t = a->mom[a->msel].ts;
    tree_reload(a);
    preview_load(a);
}

static int find_moment_at_or_before(struct app *a, int64_t t)
{
    size_t i;
    for (i = 0; i < a->nmom; i++)
        if (a->mom[i].ts <= t)
            return (int)i;
    return (int)a->nmom - 1;
}

static int do_tag(struct app *a, struct ui *u)
{
    char name[128], p[PATH_MAX];
    const char *s;
    int64_t dummy;
    FILE *f;

    if (a->msel < 0) {
        set_msg(a, "select a moment first", NULL);
        return 0;
    }
    move(u->rows - 2, 0);
    clrtoeol();
    attron(A_REVERSE);
    mvprintw(u->rows - 2, 0, " tag name: ");
    attroff(A_REVERSE);
    echo();
    curs_set(1);
    move(u->rows - 2, 11);
    refresh();
    if (wgetnstr(stdscr, name, sizeof(name) - 1) != OK) {
        noecho();
        curs_set(0);
        return 0;
    }
    noecho();
    curs_set(0);
    if (!name[0]) {
        set_msg(a, "tag cancelled", NULL);
        return 0;
    }
    for (s = name; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_' && *s != '.')
            break;
    }
    if (*s || name[0] == '.' || strlen(name) > 64 ||
        parse_when(name, now_ns(), &dummy) == 0) {
        set_msg(a, "bad tag name (letters, digits, - _ .; not a time)", NULL);
        return 0;
    }
    if (store_path(&a->st, p, sizeof(p), "tags") < 0)
        return 0;
    if (!(f = fopen(p, "a"))) {
        set_msg(a, "cannot write tags file", NULL);
        return 0;
    }
    fprintf(f, "%s %lld\n", name, (long long)a->t);
    fclose(f);
    set_msg(a, "tagged '%s'", name);
    return 0;
}

static int read_version_fd(struct store *st, const struct version *v, int fd)
{
    uint8_t (*blocks)[HASH_LEN] = NULL;
    uint64_t size = 0, off = 0;
    size_t nb = 0;
    char buf[64 * 1024];
    int r = 0;

    if ((r = bs_get_manifest(st, v->manifest, &size, &blocks, &nb)) < 0)
        return r;
    while (off < size) {
        ssize_t got = bs_read_version(st, NULL, size,
                                      (const uint8_t (*)[HASH_LEN])blocks, nb,
                                      buf, sizeof(buf), (off_t)off);
        ssize_t w = 0, k;
        if (got <= 0) {
            r = got < 0 ? (int)got : -EIO;
            goto out;
        }
        while (w < got) {
            k = write(fd, buf + w, (size_t)(got - w));
            if (k < 0) {
                r = -errno;
                goto out;
            }
            w += k;
        }
        off += (uint64_t)got;
    }
    r = 0;
out:
    free(blocks);
    return r;
}

static void do_restore(struct app *a, struct ui *u)
{
    char mp[PATH_MAX], norm[JPATH_MAX], err[160], prompt[64];
    struct tree_item *it;
    int mounted, n = 0, failed = 0, ch;
    size_t i, pl;

    if (a->nitems == 0 || a->isel >= (int)a->nitems) {
        set_msg(a, "nothing selected", NULL);
        return;
    }
    it = &a->items[a->isel];
    normalize_path(it->path, norm, sizeof(norm));
    move(u->rows - 2, 0);
    clrtoeol();
    attron(A_REVERSE);
    snprintf(prompt, sizeof(prompt), " restore %s? [y/N] ", norm);
    mvprintw(u->rows - 2, 0, "%s", prompt);
    attroff(A_REVERSE);
    refresh();
    ch = getch();
    if (ch != 'y' && ch != 'Y') {
        set_msg(a, "restore cancelled", NULL);
        return;
    }

    mounted = is_mounted(&a->st, mp, sizeof(mp));
    pl = strlen(norm);
    for (i = 0; i < a->st.idx.nall; i++) {
        struct pentry *p = a->st.idx.all[i];
        const struct version *v;
        struct version saved_version;
        char dest[PATH_MAX];
        int fd, r;

        if (strncmp(p->path, norm, pl) != 0 ||
            !(p->path[pl] == '\0' || p->path[pl] == '/'))
            continue;
        v = pentry_at(p, a->t);
        if (!v || v->op != OP_WRITE)
            continue;
        saved_version = *v;
        v = &saved_version;
        if (mounted && mp[0])
            snprintf(dest, sizeof(dest), "%s%s", mp, p->path);
        else
            store_real_path(&a->st, dest, sizeof(dest), p->path);
        mkdirs_for(dest);
        fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, v->mode & 07777);
        if (fd < 0) {
            failed++;
            continue;
        }
        r = read_version_fd(&a->st, v, fd);
        if (close(fd) < 0 && r == 0)
            r = -errno;
        if (r == 0)
            chmod(dest, v->mode & 07777);
        if (r < 0) {
            failed++;
            continue;
        }
        if (!(mounted && mp[0]) && bs_snapshot_file(&a->st, p->path) < 0) {
            failed++;
            continue;
        }
        n++;
    }
    if (n == 0)
        snprintf(err, sizeof(err), "nothing to restore: %s absent at that time", norm);
    else if (failed)
        snprintf(err, sizeof(err), "restored %d file(s), %d failed", n, failed);
    else if (mounted && mp[0])
        snprintf(err, sizeof(err), "restored %d file(s) through the mount", n);
    else
        snprintf(err, sizeof(err), "restored %d file(s) into current/", n);
    set_msg(a, err, NULL);
    tree_reload(a);
    preview_load(a);
}

/* ------------------------------------------------------------------ */

static int handle_key(struct app *a, struct ui *u, int ch)
{
    switch (ch) {
    case 'q':
    case 'Q':
    case 27:
        a->quit = 1;
        break;
    case '\t':
        a->focus ^= 1;
        preview_load(a);
        break;
    case KEY_UP:
    case 'k':
        if (a->focus == 0 && a->msel > 0) {
            a->msel--;
            set_time(a);
        } else if (a->focus == 1 && a->isel > 0) {
            a->isel--;
            preview_load(a);
        }
        break;
    case KEY_DOWN:
    case 'j':
        if (a->focus == 0 && a->msel >= 0 && a->msel + 1 < (int)a->nmom) {
            a->msel++;
            set_time(a);
        } else if (a->focus == 1 && a->isel + 1 < (int)a->nitems) {
            a->isel++;
            preview_load(a);
        }
        break;
    case KEY_PPAGE:
        if (a->focus == 1) {
            a->pv_top -= u->rows - 6;
            if (a->pv_top < 0)
                a->pv_top = 0;
        } else {
            a->msel = a->msel > 10 ? a->msel - 10 : 0;
            set_time(a);
        }
        break;
    case KEY_NPAGE:
        if (a->focus == 1) {
            if (a->pv_top + (u->rows - 6) < (int)a->npl)
                a->pv_top += u->rows - 6;
        } else if (a->msel + 10 < (int)a->nmom) {
            a->msel += 10;
            set_time(a);
        }
        break;
    case KEY_HOME:
        a->msel = a->nmom ? 0 : -1;
        set_time(a);
        break;
    case KEY_END:
        if (a->nmom) {
            a->msel = (int)a->nmom - 1;
            set_time(a);
        }
        break;
    case '\n':
    case KEY_ENTER:
        if (a->focus == 1 && a->nitems > 0 && a->items[a->isel].is_dir) {
            snprintf(a->cwd, sizeof(a->cwd), "%s", a->items[a->isel].path);
            tree_reload(a);
            preview_load(a);
        }
        break;
    case KEY_BACKSPACE:
    case 127:
        if (a->focus == 1 && strcmp(a->cwd, "/") != 0) {
            char *slash = strrchr(a->cwd, '/');
            if (slash == a->cwd)
                a->cwd[1] = '\0';
            else if (slash)
                *slash = '\0';
            tree_reload(a);
            preview_load(a);
        }
        break;
    case 'n':
        a->t = now_ns();
        a->msel = -1;
        tree_reload(a);
        preview_load(a);
        set_msg(a, "showing the live tree", NULL);
        break;
    case 'g':
        a->msel = a->nmom ? 0 : -1;
        set_time(a);
        break;
    case 'G':
        if (a->nmom) {
            a->msel = (int)a->nmom - 1;
            set_time(a);
        }
        break;
    case 't':
        do_tag(a, u);
        break;
    case 'r':
        do_restore(a, u);
        break;
    case '/': {
        char f[128];
        move(u->rows - 2, 0);
        clrtoeol();
        attron(A_REVERSE);
        mvprintw(u->rows - 2, 0, " filter: ");
        attroff(A_REVERSE);
        echo();
        curs_set(1);
        move(u->rows - 2, 9);
        refresh();
        if (wgetnstr(stdscr, f, sizeof(f) - 1) == OK)
            snprintf(a->filter, sizeof(a->filter), "%s", f);
        noecho();
        curs_set(0);
        tree_reload(a);
        preview_load(a);
        if (a->filter[0])
            set_msg(a, "filter set", NULL);
        else
            set_msg(a, "filter cleared", NULL);
        break;
    }
    case 'R': {
        if (store_reload(&a->st) < 0) {
            set_msg(a, "cannot reload store", NULL);
            break;
        }
        timeline_build(a);
        if (a->nmom > 0 && a->msel >= (int)a->nmom)
            a->msel = (int)a->nmom - 1;
        tree_reload(a);
        preview_load(a);
        set_time(a);
        set_msg(a, "store reloaded", NULL);
        break;
    }
    case '?':
    case KEY_F(1):
        draw_help(a, u);
        break;
    case KEY_RESIZE:
        ui_layout(u, LINES, COLS);
        break;
    }
    return 0;
}

int cmd_tui(int argc, char **argv)
{
    struct app a;
    struct ui ui;
    const char *store_dir = NULL, *at = NULL;
    int i, ch, r;

    for (i = 0; i < argc; i++) {
        if ((strcmp(argv[i], "--at") == 0 || strcmp(argv[i], "-t") == 0) && i + 1 < argc)
            at = argv[++i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "chronofs: unknown tui option %s\n", argv[i]);
            return 2;
        } else if (!store_dir) {
            store_dir = argv[i];
        }
    }
    if (!store_dir) {
        fprintf(stderr, "usage: chronofs tui <store> [--at WHEN]\n");
        return 2;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "chronofs: tui needs an interactive terminal\n");
        return 2;
    }
    memset(&a, 0, sizeof(a));
    memset(&ui, 0, sizeof(ui));
    if ((r = store_open(&a.st, store_dir, 0)) < 0) {
        fprintf(stderr, "chronofs: cannot open store %s: %s\n", store_dir, strerror(-r));
        return 1;
    }
    snprintf(a.cwd, sizeof(a.cwd), "/");
    a.focus = 0;
    a.t = now_ns();
    a.msel = -1;
    timeline_build(&a);
    if (at) {
        int64_t t;
        if (resolve_when(&a.st, at, &t) == 0) {
            a.t = t;
            a.msel = a.nmom ? find_moment_at_or_before(&a, t) : -1;
        } else {
            set_msg(&a, "cannot understand time", NULL);
        }
    } else if (a.nmom > 0) {
        a.msel = 0;
        a.t = a.mom[0].ts;
    }
    tree_reload(&a);
    preview_load(&a);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors())
        start_color();
    ui_layout(&ui, LINES, COLS);

    set_msg(&a, "browsing history - press ? for help", NULL);
    while (!a.quit) {
        draw_all(&a, &ui);
        ch = getch();
        if (ch == KEY_RESIZE)
            ui_layout(&ui, LINES, COLS);
        else
            handle_key(&a, &ui, ch);
    }

    endwin();
    ui_free(&ui);
    preview_free(&a);
    tree_free(&a);
    for (i = 0; i < (int)a.nmom; i++)
        free(a.mom[i].path);
    free(a.mom);
    store_close(&a.st);
    return 0;
}

#endif /* HAVE_NCURSES */
