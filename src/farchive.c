/*
 * farchive.c - FArchive, archiver for TOS / MultiTOS / MiNT.
 *
 * Starts as an icon, like FConv. Archives dropped on it (AP_DRAGDROP,
 * VA_START, VA_DRAGACCWIND or the command line) are extracted into the
 * destination folder or opened in a window of their own; other files and
 * folders dropped on it go into a new archive. Files dropped on an
 * archive's window are added to it. The formats: ZIP, LHA, ZOO, ARC, LZX,
 * TAR, TAR.GZ and GZ, with real compression in each.
 *
 * Names in archives that are not valid 8.3 names are renamed when they
 * are extracted to a FAT (TOS) drive, with a line in the log; drives with
 * long names (MiNT file systems) get the names as they are.
 *
 * The work is done by the job system that came from FView and FConv: under
 * MiNT with U:\SHM each task runs in its own FAWORK.TTP at nice 19, so
 * FArchive stays responsive; on plain TOS the jobs run cooperatively.
 * Settings live in FARCHIVE.INF next to the program; the log is also kept
 * in FARCHIVE.TXT, but only on hard disks (never on A: or B:).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <osbind.h>
#include <mintbind.h>
#include <mint/cookie.h>
#include "gemui.h"
#include "archive.h"
#include "wdlg.h"
#include "farsc.h"             /* the resource file's trees (rsc/mkrsc.py) */

#undef APPNAME
#define APPNAME "FArchive"
#define FA_VERSION "1.6.1"          /* as the binaries say */

#ifndef VA_START
#define VA_START      0x4711
#define AV_STARTED    0x4738
#define AV_PROTOKOLL  0x4700
#endif
#ifndef VA_DRAGACCWIND
#define VA_DRAGACCWIND 0x4725   /* AV desktops: files dropped on a window */
#endif
#ifndef AP_DRAGDROP
#define AP_DRAGDROP   63
#endif
#ifndef WM_ICONIFY
#define WM_ICONIFY    34
#define WM_UNICONIFY  35
#define WM_ALLICONIFY 36
#endif
#ifndef WF_ICONIFY
#define WF_ICONIFY    26
#define WF_UNICONIFY  27
#endif
#define DD_OK    0
#define DD_NAK   1
#define DD_EXT   2
#ifndef FA_DIR
#define FA_DIR   0x10
#endif
#ifndef K_LSHIFT
#define K_RSHIFT 1
#define K_LSHIFT 2
#endif
#ifndef K_CTRL
#define K_CTRL   4
#endif

long _stksize = 64 * 1024L;

/* what the shared code (job.c) expects to find */
int  vdih, ap_id;
int  scr_w, scr_h, scr_planes, pix_w_um, pix_h_um;
int  cell_w, cell_h;
GRECT desk;
PREFS prefs;
DISPINFO dinfo;
int  quitting;
int  aes_iconify, aes_3d, aes_xbutton;
static int bg3d = -1;    /* the AES's 3D grey (objc_sysvar BACKGRCOL), -1: none */   /* aes_xbutton: wdlg.c (check boxes) */
extern const char *job_app_name, *job_log_name, *job_worker_name;

/* ------------------------------------------------------------ settings */

typedef struct {
    int  fmt;                    /* AF_* for new archives                */
    int  xdrop;                  /* extract dropped archives (else open) */
    int  folder;                 /* a folder per archive when extracting */
    int  paths;                  /* keep the archive's folders           */
    int  replace;                /* overwrite existing files             */
    int  subdirs;                /* adding folders: include subfolders   */
    int  rename83;               /* 8.3 names on FAT drives              */
    int  best;                   /* best (slower) compression            */
    int  nomac;                  /* leave out .DS_Store, ._name, __MACOSX */
    int  confirm;                /* ask before copying a drag's members  */
    char dest[256];              /* destination folder, ends in '\'      */
} ASET;

static ASET cs = { AF_ZIP, 1, 1, 1, 0, 1, 1, 0, 1, 1, "" };
static char app_folder[256];

long now_ms(void)
{
    return (long)(clock() * 1000L / CLOCKS_PER_SEC);
}

static int fa_file_select(char *path, char *name, const char *title)
{
    short btn = 0, ok;
    if (_AESversion >= 0x0140)
        ok = fsel_exinput(path, name, &btn, title);
    else
        ok = fsel_input(path, name, &btn);
    return ok && btn;
}

/* the folder FARCHIVE.PRG was started from (ends in a backslash) */
static void find_app_folder(void)
{
    char cmd[256], tail[128], *s;
    cmd[0] = 0;
    if (shel_read(cmd, tail) && cmd[0] &&
        ((s = strrchr(cmd, '\\')) != 0 || (s = strrchr(cmd, '/')) != 0) &&
        (cmd[1] == ':' || cmd[0] == '\\' || cmd[0] == '/')) {
        s[1] = 0;
        strcpy(app_folder, cmd);
        return;
    }
    app_folder[0] = Dgetdrv() + 'A';
    app_folder[1] = ':';
    Dgetpath(app_folder + 2, 0);
    strcat(app_folder, "\\");
}

static void settings_path(char *p)
{
    strcpy(p, app_folder);
    strcat(p, "FARCHIVE.INF");
}

static void settings_save(void)
{
    char p[300];
    FILE *f;
    settings_path(p);
    f = fopen(p, "w");
    if (!f)
        return;
    fprintf(f, "FARCHIVE 1\nformat=%d\nextract=%d\nfolder=%d\npaths=%d\nreplace=%d\n"
            "subfolders=%d\nrename83=%d\nbest=%d\nnomac=%d\nconfirm=%d\ndest=%s\n",
            cs.fmt, cs.xdrop, cs.folder, cs.paths, cs.replace, cs.subdirs, cs.rename83,
            cs.best, cs.nomac, cs.confirm, cs.dest);
    fclose(f);
}

static void settings_load(void)
{
    char p[300], line[300];
    FILE *f;
    settings_path(p);
    f = fopen(p, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *v = strchr(line, '=');
        int n;
        if (!v)
            continue;
        *v++ = 0;
        v[strcspn(v, "\r\n")] = 0;
        n = atoi(v);
        if (!strcmp(line, "format") && n > AF_NONE && n < AF_COUNT) cs.fmt = n;
        else if (!strcmp(line, "extract")) cs.xdrop = n != 0;
        else if (!strcmp(line, "folder")) cs.folder = n != 0;
        else if (!strcmp(line, "paths")) cs.paths = n != 0;
        else if (!strcmp(line, "replace")) cs.replace = n != 0;
        else if (!strcmp(line, "subfolders")) cs.subdirs = n != 0;
        else if (!strcmp(line, "rename83")) cs.rename83 = n != 0;
        else if (!strcmp(line, "best")) cs.best = n != 0;
        else if (!strcmp(line, "nomac")) cs.nomac = n != 0;
        else if (!strcmp(line, "confirm")) cs.confirm = n != 0;
        else if (!strcmp(line, "dest")) {
            strncpy(cs.dest, v, sizeof cs.dest - 2);
            cs.dest[sizeof cs.dest - 2] = 0;
        }
    }
    fclose(f);
}


/* ----------------------------------------------------------------- menu */

static OBJECT *menu;                         /* T_MENU of the resource file */

static struct AWIN *top_awin(void);
static int aw_can_extract(struct AWIN *w, int selected);
static int aw_count_sel(struct AWIN *w);
static int clip_has(void);
static int aw_editable(struct AWIN *w, int how);
static void add_files_ask(struct AWIN *to, char **names, int n);
static void edit_copy(int cut);
static void edit_paste(void);
static void do_delete(void);
static void do_rename(void);
static int aw_filtered(struct AWIN *w);
static void fix83(struct AWIN *w);
static char find_pat[41];                   /* Archive > Find: what to look for */

static void fa_menu_update(void)
{
    struct AWIN *t = top_awin();
    menu_icheck(menu, MI_XDROP, cs.xdrop);
    menu_icheck(menu, MI_FOLDER, cs.folder);
    menu_icheck(menu, MI_PATHS, cs.paths);
    menu_icheck(menu, MI_REPLACE, cs.replace);
    menu_icheck(menu, MI_SUBDIRS, cs.subdirs);
    menu_icheck(menu, MI_83, cs.rename83);
    menu_icheck(menu, MI_NOMAC, cs.nomac);
    menu_icheck(menu, MI_CONFIRM, cs.confirm);
    menu_ienable(menu, MI_CHECK83, aw_can_extract(t, 0));
    menu_ienable(menu, MI_FIX83, aw_can_extract(t, 0) && aw_editable(t, 0));
    /* Archive: Extract selected only with something selected; Extract all
       for any archive that has been read (not a new, empty one) */
    menu_ienable(menu, MI_XSEL, aw_can_extract(t, 1));
    menu_ienable(menu, MI_XALL, aw_can_extract(t, 0));
    menu_ienable(menu, MI_FIND, aw_can_extract(t, 0));
    menu_ienable(menu, MI_FILTER, aw_can_extract(t, 0));
    menu_icheck(menu, MI_FILTER, aw_filtered(t));
    menu_ienable(menu, MI_FNEXT, aw_can_extract(t, 0) && find_pat[0]);
    menu_ienable(menu, MI_ADD, t != 0);
    menu_ienable(menu, MI_SELALL, t != 0);
    menu_ienable(menu, MI_SELNONE, t && aw_count_sel(t) > 0);
    menu_ienable(menu, MI_CUT, aw_editable(t, 1));
    menu_ienable(menu, MI_COPY, aw_editable(t, 1));
    menu_ienable(menu, MI_PASTE, aw_editable(t, 0) && clip_has());
    menu_ienable(menu, MI_DELETE, aw_editable(t, 2));
    menu_ienable(menu, MI_RENAME, aw_editable(t, 2) && aw_count_sel(t) == 1);
    menu_ienable(menu, MI_CLOSE, t != 0);
}

/* ------------------------------------------------------------- helpers */

static const char *base_name(const char *p)
{
    const char *b = p, *s;
    for (s = p; *s; s++)
        if ((*s == '\\' || *s == '/' || *s == ':') && s[1])
            b = s + 1;
    return b;
}

static int is_dir(const char *p)
{
    char t[256];
    long a;
    size_t n = strlen(p);
    if (n == 0 || n > 250)
        return 0;
    strcpy(t, p);
    if (n > 3 && (t[n - 1] == '\\' || t[n - 1] == '/'))
        t[n - 1] = 0;
    if (n <= 3 && t[1] == ':')                /* a whole drive */
        return 1;
    a = Fattrib(t, 0, 0);
    return a >= 0 && (a & FA_DIR);
}

static int exists(const char *p)
{
    return Fattrib(p, 0, 0) >= 0;
}

static int same_path(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (x != y)
            return 0;
    }
    return *a == *b;
}

/* a floppy: no log file there */
static int on_floppy(const char *p)
{
    int d = p[0] & 0xDF;
    if (p[1] == ':')
        return d == 'A' || d == 'B';
    d = Dgetdrv();
    return d < 2;
}

/* "12345", "1234K", "12.3M" in 6 columns */
static char *size_str(long n, char *b)
{
    if (n < 100000L)
        sprintf(b, "%6ld", n);
    else if (n < 10000000L)
        sprintf(b, "%5ldK", (n + 512) / 1024);
    else
        sprintf(b, "%4ld.%ldM", n / 1048576L, (n % 1048576L) * 10 / 1048576L);
    return b;
}

static char *date_str(u32 dt, char *b)
{
    unsigned d = (unsigned)(dt >> 16), t = (unsigned)(dt & 0xFFFF);
    if (!dt)
        strcpy(b, "                ");
    else
        sprintf(b, "%04d-%02d-%02d %02d:%02d", 1980 + (d >> 9), (d >> 5) & 15, d & 31,
                t >> 11, (t >> 5) & 63);
    return b;
}

/* "1.8 s", "2 min 5 s" */
static char *secs(long ms, char *b)
{
    if (ms < 0) ms = 0;
    if (ms < 60000L)
        sprintf(b, "%ld.%ld s", ms / 1000, (ms % 1000) / 100);
    else
        sprintf(b, "%ld min %ld s", ms / 60000L, (ms % 60000L) / 1000);
    return b;
}

/* ------------------------------------------------------------------ log */

/* What was done and what went wrong: shown in the main window (open the
 * icon) and appended to FARCHIVE.TXT - in the destination folder, else
 * next to the program - but only on a hard disk, never on A: or B:.  */
#define LOGN 200
#define LOGW 96
static char logl[LOGN][LOGW];
static long lognum;                          /* lines ever added          */
static long logtop;                          /* first line shown          */
static void log_changed(int was_bottom);
static int  log_visible(void);

static void log_to_file(const char *s)
{
    char p[300];
    FILE *f;
    const char *dir = cs.dest[0] && !on_floppy(cs.dest) ? cs.dest :
                      !on_floppy(app_folder) ? app_folder : 0;
    if (!dir || strlen(dir) > 280)
        return;
    strcpy(p, dir);
    strcat(p, "FARCHIVE.TXT");
    f = fopen(p, "ab");
    if (!f)
        return;
    fputs(s, f);
    fputs("\r\n", f);                       /* Atari text files */
    fclose(f);
}

static void log_add(const char *fmt, ...)
{
    char b[400];
    va_list ap;
    int was_bottom = logtop >= lognum - log_visible();
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    strncpy(logl[lognum % LOGN], b, LOGW - 1);
    logl[lognum % LOGN][LOGW - 1] = 0;
    lognum++;
    log_to_file(b);
    dbg("log: %s", b);
    log_changed(was_bottom);
}

/* ----------------------------------------------------- archive windows */

typedef struct {
    char *name;
    long  usize, csize;
    u32   dt;
    u8    flags;                             /* 1 folder, 2 unsupported   */
    u8    sel;
    char  m[9];
} MEMB;

#define MAXAW 8
typedef struct AWIN {
    int   handle;
    char  path[256];
    int   fmt;
    int   isnew;                             /* not written yet           */
    MEMB *e;
    int   n;
    long  usize, csize;
    long  top;
    int   anchor;                            /* last clicked row          */
    int   cur;                               /* the cursor keys' row      */
    int  *vis, *rowof, nv;                   /* rows shown: member indices,
                                                each member's row (-1)    */
    char  filt[41];                          /* Archive > Filter          */
    int   busy;                              /* a task is using it        */
    int   stale;                             /* read it again (cancelled) */
    long  serial;                            /* which window, for tasks   */
    char  title[80], info[100];
    GRECT full;
} AWIN;

static AWIN aw[MAXAW];
static long aw_serial;

#define AWK (NAME | CLOSER | MOVER | SIZER | FULLER | INFO | UPARROW | DNARROW | VSLIDE)

static AWIN *find_aw(int handle)
{
    int i;
    for (i = 0; i < MAXAW; i++)
        if (aw[i].handle > 0 && aw[i].handle == handle)
            return &aw[i];
    return 0;
}

/* the window a task was for - not another one that got its handle */
static AWIN *find_aw_task(int handle, long serial)
{
    AWIN *w = find_aw(handle);
    return w && w->serial == serial ? w : 0;
}

static AWIN *find_aw_path(const char *path)
{
    int i;
    for (i = 0; i < MAXAW; i++)
        if (aw[i].handle > 0 && same_path(aw[i].path, path))
            return &aw[i];
    return 0;
}

static AWIN *top_awin(void)
{
    short h = 0, d;
    wind_get(0, WF_TOP, &h, &d, &d, &d);
    return find_aw(h);
}

static void aw_free_list(AWIN *w)
{
    int i;
    for (i = 0; i < w->n; i++)
        free(w->e[i].name);
    free(w->e);
    w->e = 0;
    w->n = 0;
    free(w->vis);
    free(w->rowof);
    w->vis = w->rowof = 0;
    w->nv = 0;
    w->usize = w->csize = 0;
}

static int aw_rows(AWIN *w)
{
    GRECT wk;
    int n;
    wind_get_grect(w->handle, WF_WORKXYWH, &wk);
    n = (wk.g_h - cell_h - 6) / cell_h;
    return n < 1 ? 1 : n;
}

static void aw_slider(AWIN *w)
{
    int vis = aw_rows(w);
    if (w->top > w->nv - vis) w->top = w->nv - vis;
    if (w->top < 0) w->top = 0;
    if (w->nv <= vis) {
        wind_set(w->handle, WF_VSLSIZE, 1000, 0, 0, 0);
        wind_set(w->handle, WF_VSLIDE, 0, 0, 0, 0);
    } else {
        wind_set(w->handle, WF_VSLSIZE, (short)(1000L * vis / w->nv), 0, 0, 0);
        wind_set(w->handle, WF_VSLIDE, (short)(1000L * w->top / (w->nv - vis)), 0, 0, 0);
    }
}

static void aw_set_info(AWIN *w)
{
    char a[12], b[12];
    int nsel = 0, i;
    for (i = 0; i < w->n; i++)
        nsel += w->e[i].sel;
    if (w->busy)
        strcpy(w->info, " Working...");
    else if (w->isnew)
        sprintf(w->info, " New %s archive: drop files here to add them", af_name(w->fmt));
    else {
        size_str(w->usize, a);
        size_str(w->csize, b);
        sprintf(w->info, " %s, %d member%s, %s bytes, %s packed", af_name(w->fmt),
                w->n, w->n == 1 ? "" : "s", a, b);
        if (w->filt[0])
            sprintf(w->info + strlen(w->info), ", %d shown (%s)", w->nv, w->filt);
        if (nsel)
            sprintf(w->info + strlen(w->info), ", %d selected", nsel);
    }
    wind_set_str(w->handle, WF_INFO, w->info);
}

/* windows with a full redraw already on its way: one is enough (the
   message queue is small, and a busy log would fill it)              */
#define NPEND 12
static short pend[NPEND];

static void redraw_done(int h)
{
    int i;
    for (i = 0; i < NPEND; i++)
        if (pend[i] == h)
            pend[i] = 0;
}

static void post_redraw_h(int h)
{
    short msg[8];
    GRECT wk;
    int i, free_slot = -1;
    if (h <= 0)
        return;
    for (i = 0; i < NPEND; i++) {
        if (pend[i] == h)
            return;
        if (!pend[i] && free_slot < 0)
            free_slot = i;
    }
    if (free_slot >= 0)
        pend[free_slot] = (short)h;
    wind_get_grect(h, WF_WORKXYWH, &wk);
    msg[0] = WM_REDRAW; msg[1] = ap_id; msg[2] = 0; msg[3] = h;
    msg[4] = wk.g_x; msg[5] = wk.g_y; msg[6] = wk.g_w; msg[7] = wk.g_h;
    appl_write(ap_id, 16, msg);
}

static void aw_draw(AWIN *w, GRECT *clip)
{
    GRECT wk;
    short pxy[4], d;
    int y, x, cols, vis, i, namew;
    char line[300], s1[12], s2[12], dt[20];
    wind_get_grect(w->handle, WF_WORKXYWH, &wk);
    pxy[0] = clip->g_x; pxy[1] = clip->g_y;
    pxy[2] = clip->g_x + clip->g_w - 1; pxy[3] = clip->g_y + clip->g_h - 1;
    vsf_interior(vdih, 1);
    vsf_color(vdih, 0);
    vswr_mode(vdih, MD_REPLACE);
    vr_recfl(vdih, pxy);
    vst_height(vdih, cell_h > 10 ? 13 : 6, &d, &d, &d, &d);
    vst_alignment(vdih, 0, 5, &d, &d);
    vswr_mode(vdih, MD_TRANS);
    cols = wk.g_w / cell_w - 1;
    namew = cols - 41;                       /* size packed method date */
    if (namew < 12) namew = 12;
    x = wk.g_x + cell_w / 2;
    y = wk.g_y + 2;
    if (bg3d >= 0) {                         /* the column titles on the AES's grey */
        short hb[4];
        hb[0] = wk.g_x; hb[1] = wk.g_y;
        hb[2] = wk.g_x + wk.g_w - 1; hb[3] = y + cell_h;
        vsf_color(vdih, bg3d);
        vswr_mode(vdih, MD_REPLACE);
        vr_recfl(vdih, hb);
        hb[3] = hb[1];                       /* a white edge on top */
        vsl_color(vdih, 0);
        vsl_width(vdih, 1);
        v_pline(vdih, 2, hb);
        vswr_mode(vdih, MD_TRANS);
    }
    vst_color(vdih, 1);
    vst_effects(vdih, 1);
    sprintf(line, "%-*.*s %6s %6s %-7s %s", namew, namew, "Name", "Size", "Packed",
            "Method", "Date");
    v_gtext(vdih, x, y, line);
    vst_effects(vdih, 0);
    {
        short ln[4];
        ln[0] = wk.g_x; ln[2] = wk.g_x + wk.g_w - 1;
        ln[1] = ln[3] = y + cell_h + 1;
        vsl_color(vdih, 1);
        vsl_width(vdih, 1);
        v_pline(vdih, 2, ln);
    }
    y += cell_h + 4;
    vis = aw_rows(w);
    if (!w->n || !w->nv) {
        v_gtext(vdih, x, y, w->busy ? "(reading the archive...)" :
                w->isnew ? "(empty: drop files on this window)" :
                w->n ? "(nothing matches the filter)" : "(no members)");
        return;
    }
    for (i = (int)w->top; i < w->nv && i < w->top + vis + 1; i++) {
        MEMB *e = &w->e[w->vis[i]];
        const char *nm = e->name;
        int l = (int)strlen(nm);
        char shown[260];
        if (l > namew) {                     /* keep the end of long paths */
            strcpy(shown, "...");
            strcat(shown, nm + l - (namew - 3));
        } else
            strcpy(shown, nm);
        if (e->flags & 1)
            sprintf(line, "%-*.*s %6s %6s %-7s %s", namew, namew, shown, "", "", "folder",
                    date_str(e->dt, dt));
        else
            sprintf(line, "%-*.*s %s %s %-7.7s %s", namew, namew, shown,
                    size_str(e->usize, s1), size_str(e->csize, s2),
                    (e->flags & 2) ? "?" : e->m, date_str(e->dt, dt));
        if (e->sel) {
            short r[4];
            r[0] = wk.g_x; r[1] = y - 1;
            r[2] = wk.g_x + wk.g_w - 1; r[3] = y + cell_h - 2;
            vsf_color(vdih, 1);
            vswr_mode(vdih, MD_REPLACE);
            vr_recfl(vdih, r);
            vswr_mode(vdih, MD_TRANS);
            vst_color(vdih, 0);
        } else
            vst_color(vdih, (e->flags & 2) && scr_planes > 1 ? 2 : 1);
        v_gtext(vdih, x, y, line);
        y += cell_h;
    }
    vst_color(vdih, 1);
}

static void aw_redraw(AWIN *w, GRECT *area)
{
    GRECT r, a = *area;
    if (w->handle <= 0 || !rc_intersect(&desk, &a))
        return;
    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);
    wind_get_grect(w->handle, WF_FIRSTXYWH, &r);
    while (r.g_w && r.g_h) {
        if (rc_intersect(&a, &r)) {
            short clip[4];
            clip[0] = r.g_x; clip[1] = r.g_y;
            clip[2] = r.g_x + r.g_w - 1; clip[3] = r.g_y + r.g_h - 1;
            vs_clip(vdih, 1, clip);
            aw_draw(w, &r);
        }
        wind_get_grect(w->handle, WF_NEXTXYWH, &r);
    }
    {
        short none[4] = { 0, 0, 0, 0 };
        vs_clip(vdih, 0, none);
    }
    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);
}

static void aw_changed(AWIN *w)
{
    aw_slider(w);
    aw_set_info(w);
    post_redraw_h(w->handle);
    fa_menu_update();                        /* Extract selected follows the selection */
}

/* make the list from a job's listing blob */
static u32 get4(const u8 *p)
{
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static int find_match(const char *name, const char *pat);

/* is the member shown? Archive > Filter: one or more patterns (between
   them ; , or spaces), matched against the file's name (* and ?; plain
   text: anywhere in it). With a filter folders aren't listed.        */
static int aw_shows(AWIN *w, MEMB *e)
{
    const char *p = w->filt, *b;
    char pat[44];
    if (!*p)
        return 1;
    if (e->flags & 1)
        return 0;
    b = strrchr(e->name, '/');
    b = b ? b + 1 : e->name;
    while (*p) {
        int n = 0;
        while (*p == ';' || *p == ',' || *p == ' ')
            p++;
        while (*p && *p != ';' && *p != ',' && *p != ' ' && n < 40)
            pat[n++] = *p++;
        pat[n] = 0;
        if (n && find_match(b, pat))
            return 1;
    }
    return 0;
}

/* the rows shown again (the list or the filter changed); members that
   are hidden aren't selected, so nothing unseen is extracted or deleted */
static void aw_refilter(AWIN *w)
{
    int i;
    free(w->vis);
    free(w->rowof);
    w->nv = 0;
    w->vis = (int *)malloc((w->n + 1) * sizeof(int));
    w->rowof = (int *)malloc((w->n + 1) * sizeof(int));
    if (!w->vis || !w->rowof) {
        free(w->vis);
        free(w->rowof);
        w->vis = w->rowof = 0;
        w->filt[0] = 0;                      /* can't: show none */
        return;
    }
    for (i = 0; i < w->n; i++)
        if (aw_shows(w, &w->e[i])) {
            w->rowof[i] = w->nv;
            w->vis[w->nv++] = i;
        } else {
            w->rowof[i] = -1;
            w->e[i].sel = 0;
        }
    if (w->cur >= w->n || (w->cur >= 0 && w->rowof[w->cur] < 0))
        w->cur = -1;
    if (w->anchor >= w->n || (w->anchor >= 0 && w->rowof[w->anchor] < 0))
        w->anchor = -1;
}

static void aw_take_list(AWIN *w, const u8 *b, long len)
{
    long n, i;
    const u8 *p = b + 18, *end = b + len;
    aw_free_list(w);
    if (len < 18 || memcmp(b, "FAL1", 4))
        return;
    w->fmt = b[4] << 8 | b[5];
    n = (long)get4(b + 6);
    w->usize = (long)get4(b + 10);
    w->csize = (long)get4(b + 14);
    w->e = calloc(n + 1, sizeof(MEMB));
    if (!w->e)
        return;
    for (i = 0; i < n && p + 22 <= end; i++) {
        MEMB *e = &w->e[i];
        int nl = p[21];
        if (p + 22 + nl > end)
            break;
        e->usize = (long)get4(p);
        e->csize = (long)get4(p + 4);
        e->dt = get4(p + 8);
        e->flags = p[12];
        memcpy(e->m, p + 13, 8);
        e->m[8] = 0;
        e->name = malloc(nl + 1);
        if (!e->name)
            break;
        memcpy(e->name, p + 22, nl);
        e->name[nl] = 0;
        p += 22 + nl;
        w->n++;
    }
    w->isnew = 0;
    aw_refilter(w);
}

static void aw_close(AWIN *w)
{
    redraw_done(w->handle);
    if (w->handle > 0) {
        wind_close(w->handle);
        wind_delete(w->handle);
    }
    aw_free_list(w);
    w->handle = 0;
    fa_menu_update();
}

static void queue_list(AWIN *w);

static AWIN *aw_open(const char *path, int fmt, int isnew)
{
    AWIN *w = find_aw_path(path);
    GRECT wk, r;
    int i, slot = -1;
    if (w) {
        wind_set(w->handle, WF_TOP, 0, 0, 0, 0);
        return w;
    }
    for (i = 0; i < MAXAW; i++)
        if (aw[i].handle <= 0) {
            slot = i;
            break;
        }
    if (slot < 0) {
        alertf("[1][FArchive: close an archive|window first.][ OK ]");
        return 0;
    }
    w = &aw[slot];
    memset(w, 0, sizeof *w);
    strncpy(w->path, path, sizeof w->path - 1);
    w->fmt = fmt;
    w->isnew = isnew;
    w->anchor = -1;
    w->cur = -1;
    w->serial = ++aw_serial;
    wk.g_x = wk.g_y = 0;
    wk.g_w = 72 * cell_w;
    wk.g_h = 16 * cell_h + 8;
    wind_calc_grect(WC_BORDER, AWK, &wk, &r);
    if (r.g_w > desk.g_w) r.g_w = desk.g_w;
    if (r.g_h > desk.g_h) r.g_h = desk.g_h;
    r.g_x = desk.g_x + (desk.g_w - r.g_w) / 2 + slot * 16 - 48;
    r.g_y = desk.g_y + 16 + slot * 16;
    if (r.g_x < desk.g_x) r.g_x = desk.g_x;
    if (r.g_y + r.g_h > desk.g_y + desk.g_h) r.g_y = desk.g_y + desk.g_h - r.g_h;
    w->handle = wind_create_grect(AWK, &r);
    if (w->handle <= 0) {
        w->handle = 0;
        alertf("[1][FArchive: no more windows.][ OK ]");
        return 0;
    }
    sprintf(w->title, " %.60s ", path);
    wind_set_str(w->handle, WF_NAME, w->title);
    aw_set_info(w);
    wind_open_grect(w->handle, &r);
    w->full = r;
    aw_slider(w);
    if (!isnew)
        queue_list(w);
    fa_menu_update();
    return w;
}

static int aw_count_sel(AWIN *w)
{
    int i, n = 0;
    for (i = 0; i < w->n; i++)
        n += w->e[i].sel;
    return n;
}

/* for the menu: an archive that has been read, and for Extract selected
   something selected in it */

static int aw_can_extract(struct AWIN *w, int selected)
{
    if (!w || w->isnew)
        return 0;
    return !selected || aw_count_sel(w) > 0;
}

/* ------------------------------------------------------------ the tasks */

typedef struct {
    int   op;                                /* OP_ARC_*                  */
    char  path[256];                         /* the archive               */
    char  out[256];                          /* extract into              */
    char  list[256];                         /* list file, deleted after  */
    int   afmt;
    int   nitems;                            /* files being added         */
    int   handle;                            /* the archive's window      */
    long  serial;                            /* ... and which one         */
    int   setf, clrf;                        /* AJ_* flags forced on, off */
    int   copy_h;                            /* extract for a copy into   */
    long  copy_serial;                       /* ... this archive window   */
    char  cleanup[256];                      /* temp folder, removed after */
    char  map[256];                          /* copy: FAT's names -> originals */
    int   addclr;                            /* copy: clrf for its add    */
    char  movel[256];                        /* cut: members to delete... */
    char  movep[256];                        /* ... from this archive     */
    int   movefmt;                           /* ... in its format         */
    char  rnold[256], rnnew[256];            /* rename: member or folder  */
    char  dell[256];                         /* rename: names to take out */
} TASK;

static TASK *tasks;
static int  tn, tcap, tpos;                  /* count, room, next to do   */
static int  n_ok, n_fail, n_skip, n_ren, n_tfail;
static char first_err[90];
static int  running, cancelling, finished;
static int  want_next;
static JOB *cur;
static long batch_t0, cur_t0;
static int  item_i, item_n;                  /* member now being done      */
static int  list_seq;

static TASK *task_new(int op, const char *path)
{
    TASK *t;
    if (tn == tcap) {
        int nc = tcap ? tcap * 2 : 16;
        TASK *nt = realloc(tasks, nc * sizeof(TASK));
        if (!nt)
            return 0;
        tasks = nt;
        tcap = nc;
    }
    t = &tasks[tn++];
    memset(t, 0, sizeof *t);
    t->op = op;
    strncpy(t->path, path, sizeof t->path - 1);
    return t;
}

/* a temporary list file next to the program (or in the destination) */
static FILE *list_file(char *path, const char *mode)
{
    const char *dir = app_folder;
    FILE *f;
    int k;
    for (k = 0; k < 100; k++) {
        sprintf(path, "%sFARC%04d.LST", dir, (list_seq++) % 10000);
        if (!exists(path))
            break;
    }
    f = fopen(path, mode);
    if (!f && cs.dest[0]) {
        sprintf(path, "%sFARC%04d.LST", cs.dest, (list_seq++) % 10000);
        f = fopen(path, mode);
    }
    return f;
}

static void begin(void);

/* the folder an archive is extracted into */
static void extract_dest(const char *arc, char *out)
{
    strcpy(out, cs.dest);
    if (cs.folder) {
        char stem[64];
        const char *b = base_name(arc), *dot = strrchr(b, '.');
        size_t n = dot && dot != b ? (size_t)(dot - b) : strlen(b);
        if (n > 60) n = 60;
        memcpy(stem, b, n);
        stem[n] = 0;
        /* the folder's name: 8.3 on FAT drives */
        if (cs.rename83 && !afs_long_names(cs.dest) && n > 8)
            stem[8] = 0;
        if (strlen(out) + strlen(stem) + 2 < 256) {
            strcat(out, stem);
            strcat(out, "\\");
        }
    }
}

static int choose_dest(void);

static void queue_extract(const char *arc, AWIN *w, int selected)
{
    TASK *t;
    if (!cs.dest[0] && !choose_dest())
        return;
    t = task_new(OP_ARC_EXTRACT, arc);
    if (!t)
        return;
    extract_dest(arc, t->out);
    /* read as the window's format, else what the contents show */
    t->afmt = w ? w->fmt : AF_NONE;
    if (w)
        t->handle = w->handle, t->serial = w->serial;
    if (w && selected && aw_count_sel(w)) {
        FILE *f = list_file(t->list, "w");
        int i;
        if (!f) {
            tn--;
            log_add("Cannot write a list file next to FArchive");
            return;
        }
        for (i = 0; i < w->n; i++)
            if (w->e[i].sel) {
                fputs(w->e[i].name, f);
                fputs("\n", f);
            }
        fclose(f);
        t->nitems = aw_count_sel(w);
    }
}

static void queue_list(AWIN *w)
{
    TASK *t = task_new(OP_ARC_LIST, w->path);
    if (!t)
        return;
    t->handle = w->handle;
    t->serial = w->serial;
    t->afmt = w->fmt;                        /* read as the window's format */
    w->busy = 1;
    aw_set_info(w);
    begin();
}

/* names[] go into the archive; afmt for a new one */
static void queue_add(const char *arc, int afmt, char **names, int n, AWIN *w)
{
    TASK *t;
    FILE *f;
    int i;
    if (!n)
        return;
    t = task_new(OP_ARC_ADD, arc);
    if (!t)
        return;
    t->afmt = afmt;
    t->nitems = n;
    if (w)
        t->handle = w->handle, t->serial = w->serial;
    f = list_file(t->list, "w");
    if (!f) {
        tn--;
        log_add("Cannot write a list file next to FArchive");
        return;
    }
    for (i = 0; i < n; i++) {
        fputs(names[i], f);
        fputs("\n", f);
    }
    fclose(f);
}

/* ------------------------------------------------------ progress dialog */

#ifndef FL3DIND
#define FL3DIND 0x0200
#define FL3DBAK 0x0400
#endif
#define FL3DACT 0x0600
#define PD_PAD  16

static OBJECT ptree[FPROGRESS_NOBJ];
static char t_total[80], t_file[80], t_stat[80], t_btn[12];
static char pd_title[40];
static int  pdlg;                            /* window handle, 0 = closed  */
static long f_done, f_total = 1;             /* this task's progress       */

#define SET(i, x_, y_, w_, h_) (ptree[i].ob_x = (x_), ptree[i].ob_y = (y_), \
                                ptree[i].ob_width = (w_), ptree[i].ob_height = (h_))

/* the FPROGRESS tree of the resource file, laid out in pixels here */
static void build_dialog(void)
{
    long fill = (scr_planes >= 4) ? 0x00001174L : 0x00001171L;
    int pad = PD_PAD, pw = 40 * cell_w;
    int bh = cell_h > 10 ? 12 : 8, btnh = cell_h + 6, btnw = 11 * cell_w, y;
    memcpy(ptree, rs_tree(T_FPROGRESS), sizeof ptree);
    ptree[P_ROOT].ob_flags = aes_3d ? FL3DBAK : 0;
    ptree[P_ROOT].ob_spec.index = aes_3d ? 0x00001100L : 0x00011100L;
    SET(P_ROOT, 0, 0, pw + 2 * pad, 10);
    y = pad;
    ptree[P_TOTAL].ob_spec.free_string = t_total;
    SET(P_TOTAL, pad, y, pw, cell_h);
    y += cell_h + 6;
    ptree[P_TBOX].ob_flags = aes_3d ? FL3DIND : 0;
    ptree[P_TBOX].ob_state = aes_3d ? SELECTED : 0;
    SET(P_TBOX, pad, y, pw, bh);
    ptree[P_TBAR].ob_spec.index = fill;
    SET(P_TBAR, 0, 0, 0, bh);
    y += bh + cell_h;
    ptree[P_FILE].ob_spec.free_string = t_file;
    SET(P_FILE, pad, y, pw, cell_h);
    y += cell_h + 6;
    ptree[P_FBOX].ob_flags = aes_3d ? FL3DIND : 0;
    ptree[P_FBOX].ob_state = aes_3d ? SELECTED : 0;
    SET(P_FBOX, pad, y, pw, bh);
    ptree[P_FBAR].ob_spec.index = fill;
    SET(P_FBAR, 0, 0, 0, bh);
    y += bh + cell_h + 4;
    ptree[P_STAT].ob_spec.free_string = t_stat;
    SET(P_STAT, pad, y + (btnh - cell_h) / 2, pw - btnw - 2 * cell_w, cell_h);
    ptree[P_BTN].ob_flags = SELECTABLE | EXIT | DEFAULT | LASTOB | (aes_3d ? FL3DACT : 0);
    ptree[P_BTN].ob_spec.free_string = t_btn;
    SET(P_BTN, pad + pw - btnw, y, btnw, btnh);
    ptree[P_ROOT].ob_height = y + btnh + pad;
}

static void dlg_place(void)
{
    GRECT wk;
    if (pdlg <= 0)
        return;
    wind_get_grect(pdlg, WF_WORKXYWH, &wk);
    ptree[P_ROOT].ob_x = wk.g_x;
    ptree[P_ROOT].ob_y = wk.g_y;
}

static void dlg_bars(void)
{
    int pw = ptree[P_TBOX].ob_width;
    long tot = tn > 0 ? tn : 1, fd = f_total > 0 ? f_done * 1000L / f_total : 0;
    long t;
    if (fd > 1000) fd = 1000;
    t = finished ? 1000 : (tpos * 1000L + (cur ? fd : 0)) / tot;
    if (t > 1000) t = 1000;
    ptree[P_TBAR].ob_width = (int)(pw * t / 1000);
    ptree[P_FBAR].ob_width = finished ? pw : cur ? (int)(pw * fd / 1000) : 0;
}

/* draw one object (with its children) through the rectangle list */
static void dlg_draw(int obj, GRECT *area)
{
    GRECT r, o;
    short x, y;
    if (pdlg <= 0)
        return;
    dlg_place();
    dlg_bars();
    objc_offset(ptree, obj, &x, &y);
    o.g_x = x - 3; o.g_y = y - 3;
    o.g_w = ptree[obj].ob_width + 6;
    o.g_h = ptree[obj].ob_height + 6;
    if (area && !rc_intersect(area, &o))
        return;
    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);
    wind_get_grect(pdlg, WF_FIRSTXYWH, &r);
    while (r.g_w && r.g_h) {
        if (rc_intersect(&o, &r) && rc_intersect(&desk, &r))
            /* always from the root, clipped to the object: text objects
               are transparent, so the background must be redrawn under
               them or the new text lands on top of the old one         */
            objc_draw_grect(ptree, 0, 8, &r);
        wind_get_grect(pdlg, WF_NEXTXYWH, &r);
    }
    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);
}

static void dlg_open(void)
{
    GRECT wk, r;
    int k = NAME | MOVER;
    if (pdlg > 0) {
        wind_set(pdlg, WF_TOP, 0, 0, 0, 0);
        dlg_draw(P_ROOT, 0);
        return;
    }
    build_dialog();
    wk.g_x = wk.g_y = 0;
    wk.g_w = ptree[P_ROOT].ob_width;
    wk.g_h = ptree[P_ROOT].ob_height;
    wind_calc_grect(WC_BORDER, k, &wk, &r);
    r.g_x = desk.g_x + (desk.g_w - r.g_w) / 2;
    r.g_y = desk.g_y + (desk.g_h - r.g_h) / 2;
    pdlg = wind_create_grect(k, &r);
    if (pdlg <= 0) {
        pdlg = 0;
        return;
    }
    strcpy(pd_title, " FArchive ");
    wind_set_str(pdlg, WF_NAME, pd_title);
    wind_open_grect(pdlg, &r);
    dlg_draw(P_ROOT, 0);
}

static void dlg_close(void)
{
    if (pdlg > 0) {
        wind_close(pdlg);
        wind_delete(pdlg);
    }
    pdlg = 0;
    fa_menu_update();                        /* another window is on top now */
}

static const char *op_verb(int op)
{
    return op == OP_ARC_LIST ? "Reading" : op == OP_ARC_EXTRACT ? "Extracting" : "Adding to";
}

static void set_total_text(void)
{
    if (finished)
        sprintf(t_total, "%s: %d file%s", cancelling ? "Cancelled" : "Done", n_ok,
                n_ok == 1 ? "" : "s");
    else if (tpos < tn)
        sprintf(t_total, "%d of %d: %s %.24s", tpos + 1, tn, op_verb(tasks[tpos].op),
                base_name(tasks[tpos].path));
    else
        strcpy(t_total, "");
}

static void set_stat_text(int pct)
{
    if (finished) {
        if (n_fail || n_tfail)
            sprintf(t_stat, "%d failed: %.24s", n_fail + n_tfail, first_err);
        else if (n_ren)
            sprintf(t_stat, "%d renamed for 8.3", n_ren);
        else if (n_skip)
            sprintf(t_stat, "%d skipped", n_skip);
        else
            strcpy(t_stat, "");
    } else if (cancelling)
        strcpy(t_stat, "Cancelling...");
    else if (n_fail || n_tfail)
        sprintf(t_stat, "%3d%%   %d failed", pct, n_fail + n_tfail);
    else
        sprintf(t_stat, "%3d%%", pct);
}

/* ------------------------------------------------------ running tasks */

static void finish_all(void)
{
    char t[24];
    int i, only_lists = 1;
    for (i = 0; i < tn; i++)
        if (tasks[i].op != OP_ARC_LIST)
            only_lists = 0;
    running = 0;
    finished = 1;
    cur = 0;
    if (only_lists && !cancelling && !n_tfail) {
        /* just reading archives: no report, no dialog to close */
        dlg_close();
        finished = 0;
        tn = tpos = 0;
        return;
    }
    log_add("%s: %d done, %d failed, %d skipped, %d renamed in %s",
            cancelling ? "Cancelled" : "Finished", n_ok, n_fail + n_tfail, n_skip, n_ren,
            secs(now_ms() - batch_t0, t));
    cur = 0;
    strcpy(t_btn, "OK");
    ptree[P_BTN].ob_state &= ~SELECTED;
    fa_menu_update();
    strcpy(t_file, "");
    set_total_text();
    set_stat_text(100);
    dlg_draw(P_ROOT, 0);
}

static void start_next(void)
{
    want_next = 0;
    if (cur || !running)
        return;
    while (tpos < tn && !cancelling) {
        TASK *t = &tasks[tpos];
        JOB *j = calloc(1, sizeof(JOB));
        if (!j) {
            n_tfail++;
            if (t->list[0])
                Fdelete(t->list);
            tpos++;
            continue;
        }
        j->q.op = t->op;
        strncpy(j->q.path, t->path, sizeof j->q.path - 1);
        strncpy(j->q.out, t->out, sizeof j->q.out - 1);
        strncpy(j->q.list, t->list, sizeof j->q.list - 1);
        strncpy(j->q.map, t->map, sizeof j->q.map - 1);
        j->q.afmt = t->afmt;
        j->q.aflags = ((cs.replace ? AJ_REPLACE : 0) | (cs.rename83 ? AJ_RENAME83 : 0) |
                       (cs.subdirs ? AJ_SUBDIRS : 0) | (cs.paths ? AJ_PATHS : 0) |
                       (cs.best ? AJ_BEST : 0) | (cs.nomac ? AJ_NOMAC : 0) | t->setf) & ~t->clrf;
        j->q.pf = prefs;
        j->win = (VWIN *)&cur;               /* any non-null tag */
        f_done = 0;
        f_total = 1;
        item_i = item_n = 0;
        cur = j;
        cur_t0 = now_ms();
        if (t->op == OP_ARC_EXTRACT)
            log_add("Extracting %s to %s", t->path, t->out);
        else if (t->op == OP_ARC_ADD)
            log_add("Adding %d item%s to %s", t->nitems, t->nitems == 1 ? "" : "s", t->path);
        else if (t->op == OP_ARC_DELETE)
            log_add("Deleting %d member%s from %s", t->nitems, t->nitems == 1 ? "" : "s", t->path);
        strcpy(t_file, "");
        set_total_text();
        set_stat_text((int)(tpos * 100L / (tn ? tn : 1)));
        dlg_draw(P_ROOT, 0);
        dbg("task %d: op %d %s -> %s", tpos, t->op, t->path, t->out);
        if (!job_start(j)) {
            free(j);
            cur = 0;
            n_tfail++;
            if (t->list[0])
                Fdelete(t->list);
            tpos++;
            continue;
        }
        return;
    }
    finish_all();
}

/* ----------------------------------------- copying between archives */

/* Members dragged from one archive window to another: extracted (with
   their folders) into a temporary folder next to FArchive, then that
   folder's contents added to the other archive in its own format, and the
   folder removed. Two ordinary tasks, so the progress dialog and the log
   show both. */
/* what the temporary folder holds (top level), as full paths; any
   number of them (a full list or none: no part of a copy is lost)   */
static char **cp_names;
static int  cp_n, cp_cap, cp_bad;

static int copy_scan(void *ctx, const char *name)
{
    const char *dir = (const char *)ctx;
    char *s;
    if (cp_n == cp_cap) {
        char **a = realloc(cp_names, (cp_cap = cp_cap ? cp_cap * 2 : 64) * sizeof(char *));
        if (!a) {
            cp_bad = 1;
            return 0;
        }
        cp_names = a;
    }
    if (!(s = malloc(strlen(dir) + strlen(name) + 1))) {
        cp_bad = 1;
        return 0;
    }
    strcpy(s, dir);
    strcat(s, name);
    cp_names[cp_n++] = s;
    return 0;
}

static void cp_clear(void)
{
    int i;
    for (i = 0; i < cp_n; i++)
        free(cp_names[i]);
    cp_n = 0;
    cp_bad = 0;
}

/* a task is done (ok) or dropped: the copy's next step, or tidying up */
static void queue_delete_list(const char *arc, int afmt, const char *list, int n);
static int map_table(const char *map, const char *tab);

/* Fix 8.3 names: in the map, each original name found in the table
   (old TAB new, one a line) becomes its new name; 0 if it can't be done */
static int map_table(const char *map, const char *tab)
{
    FILE *f = fopen(tab, "r");
    char **from = 0, **to = 0, line[600];
    int nt = 0, cap = 0, ok = 1, i;
    if (!f)
        return 0;
    while (ok && fgets(line, sizeof line, f)) {
        char *t = strchr(line, '\t');
        line[strcspn(line, "\r\n")] = 0;
        if (!t)
            continue;
        *t++ = 0;
        if (nt == cap) {
            char **a = realloc(from, (cap = cap ? cap * 2 : 64) * sizeof(char *));
            char **b = a ? realloc(to, cap * sizeof(char *)) : 0;
            if (a) from = a;
            if (b) to = b;
            if (!a || !b) { ok = 0; break; }
        }
        from[nt] = malloc(strlen(line) + 1);
        to[nt] = malloc(strlen(t) + 1);
        if (!from[nt] || !to[nt]) {
            free(from[nt]);
            free(to[nt]);
            ok = 0;
            break;
        }
        strcpy(from[nt], line);
        strcpy(to[nt], t);
        nt++;
    }
    fclose(f);
    if (ok) {
        FILE *m = fopen(map, "r");
        char **ln = 0, out[700];
        int n = 0, lc = 0;
        if (!m)
            ok = 0;
        while (ok && fgets(line, sizeof line, m)) {
            char *t = strchr(line, '\t');
            const char *o;
            line[strcspn(line, "\r\n")] = 0;
            if (!t)
                continue;
            *t++ = 0;
            o = t;
            for (i = 0; i < nt; i++)
                if (!strcmp(from[i], t)) {
                    o = to[i];
                    break;
                }
            sprintf(out, "%s\t%s", line, o);
            if (n == lc) {
                char **a = realloc(ln, (lc = lc ? lc * 2 : 64) * sizeof(char *));
                if (!a) { ok = 0; break; }
                ln = a;
            }
            if (!(ln[n] = malloc(strlen(out) + 1))) { ok = 0; break; }
            strcpy(ln[n++], out);
        }
        if (m)
            fclose(m);
        if (ok && n && (m = fopen(map, "w")) != 0) {
            for (i = 0; i < n; i++)
                fprintf(m, "%s\n", ln[i]);
            fclose(m);
        } else
            ok = 0;
        for (i = 0; i < n; i++)
            free(ln[i]);
        free(ln);
    }
    for (i = 0; i < nt; i++) {
        free(from[i]);
        free(to[i]);
    }
    free(from);
    free(to);
    return ok;
}

/* a rename: in the map the extracting wrote, every original name that
   is old (or in the folder old) becomes new; 0 if it can't be done */
static int map_rename(const char *map, const char *old, const char *nw)
{
    FILE *f = map[0] ? fopen(map, "r") : 0;
    char **ln = 0, line[600], out[600];
    int n = 0, cap = 0, i, ok = 1;
    size_t ol = strlen(old);
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t'), *o;
        line[strcspn(line, "\r\n")] = 0;
        if (!tab)
            continue;
        *tab = 0;
        o = tab + 1;
        if (strlen(o) >= ol && !strncmp(o, old, ol) && (!o[ol] || o[ol] == '/'))
            sprintf(out, "%s\t%s%s", line, nw, o + ol);
        else
            sprintf(out, "%s\t%s", line, o);
        if (n == cap) {
            char **nl = realloc(ln, (cap = cap ? cap * 2 : 16) * sizeof(char *));
            if (!nl) { ok = 0; break; }
            ln = nl;
        }
        if (!(ln[n] = malloc(strlen(out) + 1))) { ok = 0; break; }
        strcpy(ln[n++], out);
    }
    fclose(f);
    if (ok && (f = fopen(map, "w")) != 0) {
        for (i = 0; i < n; i++)
            fprintf(f, "%s\n", ln[i]);
        fclose(f);
    } else
        ok = 0;
    for (i = 0; i < n; i++)
        free(ln[i]);
    free(ln);
    return ok && n;
}

static void copy_step(TASK *t, int ok)
{
    char tmp[256], movel[256], movep[256], rnold[256], rnnew[256], dell[256];
    int h = t->copy_h, addclr = t->addclr, movefmt = t->movefmt;
    long ser = t->copy_serial;
    strcpy(movel, t->movel);
    strcpy(movep, t->movep);
    strcpy(rnold, t->rnold);
    strcpy(rnnew, t->rnnew);
    strcpy(dell, t->dell);
    t->movel[0] = t->rnold[0] = t->dell[0] = 0;
    if (t->cleanup[0]) {                     /* the add is done: the temp goes */
        afs_rmtree(t->cleanup);
        t->cleanup[0] = 0;
        if (t->map[0])
            Fdelete(t->map);
        t->map[0] = 0;
        if (t->op == OP_ARC_ADD && t->out[0])
            Fdelete(t->out);                 /* a rename's names taken out */
        if (movel[0]) {                      /* a cut: now out of the source */
            if (ok)
                queue_delete_list(movep, movefmt, movel, t->nitems);
            else
                Fdelete(movel);
        }
        return;
    }
    if (!h) {
        if (movel[0])
            Fdelete(movel);
        if (dell[0])
            Fdelete(dell);
        if (rnold[0] == 1)
            Fdelete(rnnew);
        return;
    }
    t->copy_h = 0;
    strcpy(tmp, t->out);                     /* t moves if tasks[] grows */
    {
        AWIN *to = find_aw_task(h, ser);
        char map[256];
        int n = 0;
        strcpy(map, t->map);
        cp_clear();
        if (ok && to) {
            afs_scandir(tmp, copy_scan, tmp);
            n = cp_bad ? 0 : cp_n;
            if (cp_bad)
                log_add("Out of memory: %s left as it was", base_name(to->path));
        }
        if (n && rnold[0] == 1) {            /* Fix 8.3: a table of new names */
            if (!map_table(map, rnnew))
                n = 0;
        } else if (n && rnold[0] && !map_rename(map, rnold, rnnew))
            n = 0;                           /* no map: nothing changes */
        if (rnold[0] == 1)
            Fdelete(rnnew);                  /* the table: done with */
        if (!n) {
            afs_rmtree(tmp);
            if (map[0])
                Fdelete(map);
            if (movel[0])
                Fdelete(movel);
            if (dell[0])
                Fdelete(dell);
            if (rnold[0] && to)
                queue_list(to);              /* not renamed: as it was */
            return;
        }
        queue_add(to->path, to->fmt, cp_names, n, to);
        cp_clear();
        if (tn > 0 && tasks[tn - 1].op == OP_ARC_ADD) {
            TASK *a = &tasks[tn - 1];
            a->setf = AJ_PATHS | AJ_SUBDIRS;
            a->clrf = addclr;
            strcpy(a->cleanup, tmp);
            strcpy(a->map, map);             /* the original names back */
            strcpy(a->movel, movel);
            strcpy(a->movep, movep);
            a->movefmt = movefmt;
            if (rnold[0]) {                  /* the old names out, same rewrite */
                strcpy(a->out, dell);
                a->setf |= AJ_REPLACE;
                a->clrf |= AJ_NOMAC;
                a->nitems = 1;
            }
        } else {
            afs_rmtree(tmp);
            if (map[0])
                Fdelete(map);
            if (movel[0])
                Fdelete(movel);
            if (dell[0])
                Fdelete(dell);
            if (rnold[0])
                queue_list(to);
        }
    }
}

/* names[] (members of the archive fpath) copied into the archive window
   to; clr: AJ_ flags its add leaves off; move: deleted from fpath after */
static void copy_members(const char *fpath, int ffmt, int fh, long fs,
                         char **names, int nn, AWIN *to, int clr, int move)
{
    char tmp[256], dir[256], fp[256];
    TASK *t;
    int k, i;
    FILE *f;
    if (!nn || !to || same_path(fpath, to->path))
        return;
    strcpy(fp, fpath);                       /* names may point into tasks */
    for (k = 0; k < 1000; k++) {
        sprintf(dir, "%sFACP%04d", app_folder, (int)((now_ms() / 7 + k) % 10000));
        if (!exists(dir))
            break;
    }
    if (afs_mkdir(dir) != E_OK) {
        log_add("Cannot make a temporary folder %s", dir);
        return;
    }
    sprintf(tmp, "%s\\", dir);
    t = task_new(OP_ARC_EXTRACT, fp);
    if (!t) {
        afs_rmtree(dir);
        return;
    }
    strcpy(t->out, tmp);
    t->afmt = ffmt;
    t->handle = fh;
    t->serial = fs;
    t->setf = AJ_PATHS | AJ_REPLACE;
    t->copy_h = to->handle;
    t->copy_serial = to->serial;
    t->addclr = clr;
    /* FAT shortens long names in the temporary folder: the extracting
       notes each file's original name here, the adding puts it back */
    sprintf(t->map, "%s.MAP", dir);
    f = list_file(t->list, "w");
    if (!f) {
        tn--;
        afs_rmtree(dir);
        log_add("Cannot write a list file next to FArchive");
        return;
    }
    for (i = 0; i < nn; i++) {
        fputs(names[i], f);
        fputs("\n", f);
    }
    fclose(f);
    t->nitems = nn;
    if (move) {                              /* a cut: what to delete after */
        f = list_file(t->movel, "w");
        if (f) {
            for (i = 0; i < nn; i++) {
                fputs(names[i], f);
                fputs("\n", f);
            }
            fclose(f);
            strcpy(t->movep, fp);
            t->movefmt = ffmt;
        } else
            t->movel[0] = 0;
    }
    log_add("%s %d member%s from %s to %s", move ? "Moving" : "Copying", nn, nn == 1 ? "" : "s",
            base_name(fp), base_name(to->path));
    begin();
}

/* the GUI side of job.c */
void win_job_finished(VWIN *vw, JOB *j)
{
    TASK *t = tpos < tn ? &tasks[tpos] : 0;
    AWIN *w = t ? find_aw_task(t->handle, t->serial) : 0;
    char tm[24];
    (void)vw;
    if (j->res.err == E_CANCEL || j->cancel) {
        cancelling = 1;
        log_add("%s: cancelled", base_name(j->q.path));
    } else if (j->res.err) {
        n_tfail++;
        if (!first_err[0])
            sprintf(first_err, "%.20s: %.40s", base_name(j->q.path),
                    j->res.detail[0] ? j->res.detail : fv_errstr(j->res.err));
        log_add("%s: failed, %s%s%s", base_name(j->q.path), fv_errstr(j->res.err),
                j->res.detail[0] ? ": " : "", j->res.detail);
    } else if (j->q.op != OP_ARC_LIST)
        log_add("%s: %ld done, %ld failed, %ld skipped, %ld renamed  %s", base_name(j->q.path),
                j->res.acount[0], j->res.acount[1], j->res.acount[2], j->res.acount[3],
                secs(now_ms() - cur_t0, tm));
    if (j->q.op != OP_ARC_LIST) {
        n_ok += (int)j->res.acount[0];
        n_fail += (int)j->res.acount[1];
        n_skip += (int)j->res.acount[2];
        n_ren += (int)j->res.acount[3];
    }
    if (j->q.op == OP_ARC_LIST && w) {
        w->busy = 0;
        if (!j->res.err && j->res.ptr[RB_DOC])
            aw_take_list(w, j->res.ptr[RB_DOC], j->res.doclen);
        else if (j->res.err && !j->cancel) {
            /* not an archive after all: drop the window */
            aw_close(w);
            w = 0;
        }
        if (w)
            aw_changed(w);
    }
    {
        int i;
        for (i = 0; i < RB_MAX; i++)
            job_release(&j->res, i);
    }
    if (t && t->list[0])
        Fdelete(t->list);
    /* the next step of a copy, move or rename only if nothing failed (a
       move or rename takes the originals out after); a move not if some
       were skipped either (Keep old): they would be lost                */
    if (t)
        copy_step(t, !j->res.err && !j->cancel && !cancelling &&
                  (j->q.op == OP_ARC_LIST || !j->res.acount[1]) &&
                  !(j->q.op == OP_ARC_ADD && t->movel[0] && j->res.acount[2]));
    /* the archive (may have) changed: read it again - now, or when the
       cancelled batch's dialog is closed                              */
    if (j->q.op == OP_ARC_ADD || j->q.op == OP_ARC_DELETE) {
        AWIN *aw2 = find_aw_path(j->q.path);
        if (aw2) {
            /* read again even after an error: the archive may have
               been written all the same                          */
            if (!j->cancel && !cancelling) {
                if (!j->res.err)
                    aw2->isnew = 0;
                if (!aw2->isnew)
                    queue_list(aw2);
            } else
                aw2->stale = 1;
        }
    }
    if (cur == j)
        cur = 0;
    free(j);
    tpos++;
    if (cancelling) {
        int k;
        for (k = tpos; k < tn; k++) {
            AWIN *x = find_aw_task(tasks[k].handle, tasks[k].serial);
            if (tasks[k].list[0])
                Fdelete(tasks[k].list);
            copy_step(&tasks[k], 0);
            if (x) {
                x->busy = 0;
                aw_set_info(x);
            }
        }
        tpos = tn;
        finish_all();
    } else
        want_next = 1;                       /* from the main loop */
}

void win_stage(VWIN *w, int stage)
{
    (void)w;
    (void)stage;
}

/* log lines and "now doing" from the job */
void win_note(VWIN *w, int kind, const char *text)
{
    (void)w;
    if (kind == NOTE_LOG) {
        log_add("  %s", text);
        return;
    }
    if (kind == NOTE_ITEM && cur) {
        int i = 0, n = 0;
        const char *s = text;
        i = atoi(s);
        while (*s && *s != ' ') s++;
        if (*s) s++;
        n = atoi(s);
        while (*s && *s != ' ') s++;
        if (*s) s++;
        item_i = i;
        item_n = n;
        if (n > 0)
            sprintf(t_file, "%d/%d %.30s", i, n, base_name(s));
        else
            sprintf(t_file, "%.36s", base_name(s));
        dlg_draw(P_FILE, 0);
    }
}

void win_progress(VWIN *w, long done, long total)
{
    static int lastpct = -1, lastf = -1;
    int pct, fp;
    (void)w;
    if (total <= 0) total = 1;
    f_done = done;
    f_total = total;
    fp = (int)(done * 100 / total);
    pct = (int)((tpos * 100L + fp) / (tn ? tn : 1));
    if (pct == lastpct && fp == lastf)
        return;
    lastpct = pct;
    lastf = fp;
    set_stat_text(pct);
    dlg_draw(P_TBOX, 0);
    dlg_draw(P_FBOX, 0);
    dlg_draw(P_STAT, 0);
}

static void cancel_all(void)
{
    if (!running || cancelling)
        return;
    cancelling = 1;
    ptree[P_BTN].ob_state |= SELECTED;
    set_stat_text(0);
    dlg_draw(P_BTN, 0);
    dlg_draw(P_STAT, 0);
    if (cur)
        job_cancel(cur);
    else {
        tpos = tn;
        finish_all();
    }
}

static int choose_dest(void)
{
    char path[300], name[64] = "";
    char *s;
    if (cs.dest[0])
        strcpy(path, cs.dest);
    else
        strcpy(path, app_folder);
    strcat(path, "*.*");
    if (!fa_file_select(path, name, "Destination folder (OK)"))
        return 0;
    s = strrchr(path, '\\');
    if (!s)
        return 0;
    s[1] = 0;
    if (strlen(path) >= sizeof cs.dest - 1) {
        alertf("[1][FArchive: that folder's path|is too long.][ OK ]");
        return 0;
    }
    strcpy(cs.dest, path);
    settings_save();
    return 1;
}

static void show_log(void);

/* tasks were queued: start (or carry on) */
static void begin(void)
{
    int i, quiet = 1;
    if (tpos >= tn)
        return;
    for (i = tpos; i < tn; i++)
        if (tasks[i].op != OP_ARC_LIST)
            quiet = 0;
    if (!quiet)
        show_log();                          /* open the icon: the log */
    if (running)
        return;                              /* added to the running batch */
    /* a new batch: forget the tasks of the last one */
    if (tpos > 0) {
        memmove(tasks, tasks + tpos, (tn - tpos) * sizeof(TASK));
        tn -= tpos;
        tpos = 0;
    }
    batch_t0 = now_ms();
    if (!quiet) {
        time_t now = time(0);
        struct tm *tm = localtime(&now);
        char when[32];
        if (tm)
            sprintf(when, "%04d-%02d-%02d %02d:%02d", tm->tm_year + 1900, tm->tm_mon + 1,
                    tm->tm_mday, tm->tm_hour, tm->tm_min);
        else
            strcpy(when, "");
        log_add("%s  %d task%s", when, tn, tn == 1 ? "" : "s");
    }
    running = 1;
    finished = cancelling = 0;
    n_ok = n_fail = n_skip = n_ren = n_tfail = 0;
    first_err[0] = 0;
    cur = 0;
    strcpy(t_btn, "Cancel");
    ptree[P_BTN].ob_state = 0;
    t_file[0] = 0;
    set_total_text();
    set_stat_text(0);
    dlg_open();
    ptree[P_BTN].ob_state = 0;
    want_next = 1;
}

static void dlg_button(void)
{
    if (finished) {
        int i;
        dlg_close();
        finished = 0;
        tn = tpos = 0;
        for (i = 0; i < MAXAW; i++)
            if (aw[i].handle > 0 && aw[i].stale) {
                aw[i].stale = 0;
                if (exists(aw[i].path)) {
                    aw[i].isnew = 0;
                    queue_list(&aw[i]);
                }
            }
    } else
        cancel_all();
}

/* ------------------------------------------------- what was dropped */

static char **loose;                         /* not archives: to be added  */
static int  nloose, caploose;

static void loose_add(const char *p)
{
    char *s;
    if (nloose == caploose) {
        int nc = caploose ? caploose * 2 : 32;
        char **nl = realloc(loose, nc * sizeof(char *));
        if (!nl)
            return;
        loose = nl;
        caploose = nc;
    }
    s = malloc(strlen(p) + 1);
    if (!s)
        return;
    strcpy(s, p);
    loose[nloose++] = s;
}

static void loose_clear(void)
{
    int i;
    for (i = 0; i < nloose; i++)
        free(loose[i]);
    nloose = 0;
}

static int new_archive_name(char *out, const char *suggest);
static void new_archive_begin(int kind);
static void pack_loose(void);
enum { NA_NEW = 1, NA_PACK };                /* what a new archive is for */

/* one name dropped on the icon or given on the command line */
static void drop_path(const char *p)
{
    char t[256];
    size_t n;
    if (!*p)
        return;
    strncpy(t, p, sizeof t - 1);
    t[sizeof t - 1] = 0;
    n = strlen(t);
    if (n > 3 && (t[n - 1] == '\\' || t[n - 1] == '/'))
        t[n - 1] = 0;
    if (!is_dir(t)) {
        int fmt = af_detect(t);
        if (fmt != AF_NONE) {
            if (cs.xdrop)
                queue_extract(t, 0, 0);
            else
                aw_open(t, fmt, 0);
            return;
        }
    }
    loose_add(t);
}

/* files that are not archives were dropped: put them in a new one */
static void loose_done(AWIN *into)
{
    char arc[300];
    if (!nloose)
        return;
    (void)arc;
    if (into) {
        add_files_ask(into, loose, nloose);
        loose_clear();
        return;
    }
    /* a new archive for them: its format first (a window), then its name;
       the files wait in loose[] until then */
    new_archive_begin(NA_PACK);
}

/* the dropped files, once the new archive's format is chosen */
static void pack_loose(void)
{
    char arc[300];
    if (nloose && new_archive_name(arc, nloose == 1 ? loose[0] : 0)) {
        int fmt = af_from_name(arc);
        if (exists(arc) && af_detect(arc) != AF_NONE)
            fmt = af_detect(arc);
        if (fmt == AF_NONE)
            fmt = cs.fmt;
        queue_add(arc, fmt, loose, nloose, find_aw_path(arc));
        begin();
    }
    loose_clear();
}

/* "ARGS": names separated by spaces, quoted with ' ('' is a quote) or " */
static void each_arg(char *s, void (*fn)(const char *))
{
    char name[256];
    while (*s) {
        int n = 0;
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
            s++;
        if (!*s)
            break;
        if (*s == '\'' || *s == '"') {
            char q = *s++;
            while (*s) {
                if (*s == q) {
                    if (q == '\'' && s[1] == '\'') {
                        if (n < 255) name[n++] = '\'';
                        s += 2;
                        continue;
                    }
                    s++;
                    break;
                }
                if (n < 255) name[n++] = *s;
                s++;
            }
        } else
            while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') {
                if (n < 255) name[n++] = *s;
                s++;
            }
        name[n] = 0;
        if (n)
            fn(name);
    }
}

/* names dropped: on an archive window they are added to it, on the
   icon (or the log) archives are extracted / opened and the rest packed */
static void dropped(char *args, int handle)
{
    AWIN *w = find_aw(handle);
    graf_mouse(BUSYBEE, 0);
    if (w)
        each_arg(args, loose_add);
    else
        each_arg(args, drop_path);
    graf_mouse(ARROW, 0);
    if (w && nloose) {
        int i;
        /* not the archive itself */
        for (i = 0; i < nloose; i++)
            if (same_path(loose[i], w->path)) {
                free(loose[i]);
                loose[i--] = loose[--nloose];
            }
    }
    loose_done(w);
    begin();
}

/* ------------------------------------------------------- the main window */

static int  mainw;                           /* window handle             */
static int  iconified;                       /* 1 AES, 2 our own          */
static GRECT main_full;                      /* size before iconifying    */
static OBJECT *ic_tree;                      /* T_ICON of the resource file */
static char ic_label[] = "FArchive";

static void fa_build_icon(void)
{
    ic_tree = rs_tree(T_ICON);
    ic_tree[1].ob_spec.iconblk->ib_ptext = ic_label;
}

static void draw_main(GRECT *clip)
{
    GRECT wk;
    short pxy[4], d;
    char line[300];
    int x, y;
    wind_get_grect(mainw, WF_WORKXYWH, &wk);
    pxy[0] = clip->g_x; pxy[1] = clip->g_y;
    pxy[2] = clip->g_x + clip->g_w - 1; pxy[3] = clip->g_y + clip->g_h - 1;
    vsf_interior(vdih, 1);
    vsf_color(vdih, 0);
    vswr_mode(vdih, MD_REPLACE);
    vr_recfl(vdih, pxy);
    if (iconified) {
        ic_tree[0].ob_x = wk.g_x;
        ic_tree[0].ob_y = wk.g_y;
        ic_tree[0].ob_width = wk.g_w;
        ic_tree[0].ob_height = wk.g_h;
        ic_tree[1].ob_x = (wk.g_w - ic_tree[1].ob_width) / 2;
        ic_tree[1].ob_y = (wk.g_h - ic_tree[1].ob_height) / 2;
        if (ic_tree[1].ob_y < 0) ic_tree[1].ob_y = 0;
        objc_draw_grect(ic_tree, 0, 2, clip);
        return;
    }
    vst_height(vdih, cell_h > 10 ? 13 : 6, &d, &d, &d, &d);
    vst_alignment(vdih, 0, 5, &d, &d);
    vst_color(vdih, 1);
    vswr_mode(vdih, MD_TRANS);
    x = wk.g_x + 8;
    y = wk.g_y + 4;
    vst_effects(vdih, 1);
    v_gtext(vdih, x, y, "FArchive " FA_VERSION " (" FV_CPU ") - drop archives or files here");
    vst_effects(vdih, 0);
    y += cell_h + 6;
    sprintf(line, "New archives: format asked for (last %s%s)", af_name(cs.fmt), cs.best ? ", best" : "");
    v_gtext(vdih, x, y, line); y += cell_h;
    sprintf(line, "Dropped archives are %s", cs.xdrop ? "extracted" : "opened");
    v_gtext(vdih, x, y, line); y += cell_h;
    sprintf(line, "Destination:  %.50s", cs.dest[0] ? cs.dest : "(asked for first)");
    v_gtext(vdih, x, y, line); y += cell_h;
    sprintf(line, "Folder per archive %s, keep folders %s, replace %s, 8.3 names %s",
            cs.folder ? "on" : "off", cs.paths ? "on" : "off", cs.replace ? "on" : "off",
            cs.rename83 ? "on" : "off");
    v_gtext(vdih, x, y, line); y += cell_h;
    sprintf(line, "%s", !have_mint ? "TOS: working inside FArchive" :
            !have_worker ? "MiNT: FAWORK.TTP not found, working inside FArchive" :
            "MiNT: working in background workers");
    v_gtext(vdih, x, y, line); y += cell_h + 3;
    {
        short ln[4];
        long i, first = lognum > LOGN ? lognum - LOGN : 0;
        int vis = log_visible();
        ln[0] = wk.g_x; ln[2] = wk.g_x + wk.g_w - 1;
        ln[1] = ln[3] = y;
        vsl_color(vdih, 1);
        vsl_width(vdih, 1);
        v_pline(vdih, 2, ln);
        y += 4;
        if (!lognum)
            v_gtext(vdih, x, y, "(what FArchive does is listed here)");
        for (i = logtop < first ? first : logtop; i < lognum && i < logtop + vis; i++) {
            v_gtext(vdih, x, y, logl[i % LOGN]);
            y += cell_h;
        }
    }
}

#define HDR_LINES 6                          /* title + 5 lines above the log */

static int log_visible(void)
{
    GRECT wk;
    int n;
    if (mainw <= 0 || iconified)
        return 20;
    wind_get_grect(mainw, WF_WORKXYWH, &wk);
    n = (wk.g_h - (4 + HDR_LINES * cell_h + 6 + 7)) / cell_h;
    return n < 1 ? 1 : n;
}

static void clamp_top(void)
{
    long first = lognum > LOGN ? lognum - LOGN : 0, maxtop = lognum - log_visible();
    if (maxtop < first) maxtop = first;
    if (logtop > maxtop) logtop = maxtop;
    if (logtop < first) logtop = first;
}

static void set_slider(void)
{
    long first = lognum > LOGN ? lognum - LOGN : 0, avail = lognum - first;
    int vis = log_visible();
    if (mainw <= 0 || iconified)
        return;
    clamp_top();
    if (avail <= vis) {
        wind_set(mainw, WF_VSLSIZE, 1000, 0, 0, 0);
        wind_set(mainw, WF_VSLIDE, 0, 0, 0, 0);
    } else {
        wind_set(mainw, WF_VSLSIZE, (short)(1000L * vis / avail), 0, 0, 0);
        wind_set(mainw, WF_VSLIDE, (short)(1000L * (logtop - first) / (avail - vis)), 0, 0, 0);
    }
}

static void post_redraw(void)
{
    post_redraw_h(mainw);
}

static void log_changed(int was_bottom)
{
    if (was_bottom)
        logtop = lognum - log_visible();
    clamp_top();
    if (mainw > 0 && !iconified) {
        set_slider();
        post_redraw();
    }
}

static void main_redraw(GRECT *area)
{
    GRECT r, a = *area;
    if (mainw <= 0 || !rc_intersect(&desk, &a))
        return;
    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);
    wind_get_grect(mainw, WF_FIRSTXYWH, &r);
    while (r.g_w && r.g_h) {
        if (rc_intersect(&a, &r)) {
            short clip[4];
            clip[0] = r.g_x; clip[1] = r.g_y;
            clip[2] = r.g_x + r.g_w - 1; clip[3] = r.g_y + r.g_h - 1;
            vs_clip(vdih, 1, clip);
            draw_main(&r);
        }
        wind_get_grect(mainw, WF_NEXTXYWH, &r);
    }
    {
        short none[4] = { 0, 0, 0, 0 };
        vs_clip(vdih, 0, none);
    }
    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);
}

/* the icon sits at the bottom left, to the right of FView's and FConv's */
static void icon_rect(GRECT *r)
{
    GRECT wk;
    if (aes_iconify) {
        r->g_w = 72;
        r->g_h = 72;
    } else {
        wk.g_x = wk.g_y = 0;
        wk.g_w = 72;
        wk.g_h = 44;
        wind_calc_grect(WC_BORDER, NAME | CLOSER | MOVER, &wk, r);
    }
    r->g_x = desk.g_x + 2 * (r->g_w + 8);
    r->g_y = desk.g_y + desk.g_h - r->g_h;
}

#define MAINK (NAME | CLOSER | MOVER | SIZER | FULLER | UPARROW | DNARROW | VSLIDE)
#define ICONK (NAME | CLOSER | MOVER)

/* (re)create the main window with the given gadgets */
static void make_window(int kind, GRECT *r, const char *title)
{
    redraw_done(mainw);
    if (mainw > 0) {
        wind_close(mainw);
        wind_delete(mainw);
    }
    mainw = wind_create_grect(kind, r);
    if (mainw <= 0) {
        mainw = 0;
        return;
    }
    wind_set_str(mainw, WF_NAME, title);
    wind_open_grect(mainw, r);
}

static void to_icon(void)
{
    GRECT r;
    icon_rect(&r);
    if (aes_iconify) {
        wind_get_grect(mainw, WF_CURRXYWH, &main_full);
        wind_set_grect(mainw, WF_ICONIFY, &r);
        iconified = 1;
        wind_set_str(mainw, WF_NAME, APPNAME);
    } else {
        /* no WF_ICONIFY: a small window of its own shows the icon */
        wind_get_grect(mainw, WF_CURRXYWH, &main_full);
        iconified = 2;
        make_window(ICONK, &r, APPNAME);
    }
    post_redraw();
}

static void from_icon(GRECT *r)
{
    if (iconified == 1) {
        wind_set_grect(mainw, WF_UNICONIFY, r);
        iconified = 0;
        wind_set_str(mainw, WF_NAME, " FArchive ");
    } else {
        iconified = 0;
        make_window(MAINK | (aes_iconify ? SMALLER : 0), &main_full, " FArchive ");
    }
    logtop = lognum - log_visible();
    set_slider();
    post_redraw();
}

/* open the icon into the log window (and bring it forward) */
static void show_log(void)
{
    if (mainw <= 0)
        return;
    if (iconified == 1) {
        GRECT r = main_full;
        if (r.g_w <= 0 || r.g_h <= 0)
            wind_get_grect(mainw, WF_UNICONIFY, &r);
        from_icon(&r);
    } else if (iconified == 2)
        from_icon(&main_full);
    wind_set(mainw, WF_TOP, 0, 0, 0, 0);
    if (pdlg > 0)
        wind_set(pdlg, WF_TOP, 0, 0, 0, 0);  /* the progress stays in front */
}

static void open_main(void)
{
    GRECT wk, r;
    int k = MAINK | (aes_iconify ? SMALLER : 0);
    wk.g_x = wk.g_y = 0;
    wk.g_w = 66 * cell_w;
    wk.g_h = (HDR_LINES + 12) * cell_h + 20;
    wind_calc_grect(WC_BORDER, k, &wk, &r);
    if (r.g_w > desk.g_w) r.g_w = desk.g_w;
    if (r.g_h > desk.g_h) r.g_h = desk.g_h;
    r.g_x = desk.g_x + (desk.g_w - r.g_w) / 2;
    r.g_y = desk.g_y + (desk.g_h - r.g_h) / 3;
    main_full = r;
    make_window(k, &r, " FArchive ");
    if (mainw <= 0)
        return;
    to_icon();
}

/* ------------------------------------------------------------- commands */

/* About: a window of its own (nothing waits for it) */
static int about_exit(WDIALOG *d, int obj)
{
    (void)d; (void)obj;
    return 1;
}

static void about(void)
{
    WDIALOG *d = wd_open(T_ABOUT, rs(S_W_ABOUT), 0, about_exit, 0, 0);
    if (!d)
        return;
    rsf(wd_text(d, AB_VER), 60, S_S_VERSION, FA_VERSION, FV_CPU);
    wd_redraw_obj(d, AB_VER);
}

static char sel_path[300];

static int pick_file(char *full, const char *title, int allow_folder)
{
    static char name[64];
    char *s;
    if (!sel_path[0]) {
        strcpy(sel_path, cs.dest[0] ? cs.dest : app_folder);
        strcat(sel_path, "*.*");
    }
    name[0] = 0;
    if (!fa_file_select(sel_path, name, title))
        return 0;
    strcpy(full, sel_path);
    s = strrchr(full, '\\');
    if (s) s[1] = 0;
    if (name[0])
        strcat(full, name);
    else if (allow_folder && s)
        s[0] = 0;                            /* no name: the whole folder */
    else
        return 0;
    return 1;
}

static void do_open(void)
{
    char full[300];
    int fmt;
    if (!pick_file(full, "Open archive", 0))
        return;
    fmt = af_detect(full);
    if (fmt == AF_NONE) {
        alertf("[1][FArchive: %.30s|is not an archive FArchive|can read.][ OK ]",
               base_name(full));
        return;
    }
    aw_open(full, fmt, 0);
}

static void do_extract_file(void)
{
    char full[300];
    if (!pick_file(full, "Extract archive", 0))
        return;
    if (af_detect(full) == AF_NONE) {
        alertf("[1][FArchive: %.30s|is not an archive FArchive|can read.][ OK ]",
               base_name(full));
        return;
    }
    queue_extract(full, 0, 0);
    begin();
}

/* The format of a new archive, asked as it is made: a window of its own
   (nothing waits for it) with the formats as radio buttons and Best
   compression, set to the last ones used. OK goes on to the name. */
static const int nf_obj[AF_COUNT - 1] = { NF_ZIP, NF_LHA, NF_ZOO, NF_ARC, NF_LZX, NF_TAR, NF_TGZ, NF_GZ };
static WDIALOG *nf_dlg;
static void new_archive_go(int kind);

static int nf_exit(WDIALOG *d, int obj)
{
    OBJECT *t = wd_tree(d);
    int kind = (int)(long)wd_ctx(d), i;
    nf_dlg = 0;
    if (obj == NF_OK) {
        for (i = 0; i < AF_COUNT - 1; i++)
            if (t[nf_obj[i]].ob_state & SELECTED)
                cs.fmt = i + 1;
        cs.best = (t[NF_BEST].ob_state & SELECTED) != 0;
        settings_save();                     /* the next new archive starts here */
        new_archive_go(kind);
    } else if (kind == NA_PACK)
        loose_clear();                       /* the dropped files: not packed */
    return 1;
}

/* open the format window (or bring it to the front) */
static void new_archive_begin(int kind)
{
    OBJECT *t;
    int i;
    if (nf_dlg) {
        wd_close(nf_dlg);                    /* one at a time: the newest wins */
        nf_dlg = 0;
    }
    nf_dlg = wd_open(T_NEWFMT, rs(S_W_NEWFMT), 0, nf_exit, 0, (void *)(long)kind);
    if (!nf_dlg)
        return;
    t = wd_tree(nf_dlg);
    for (i = 0; i < AF_COUNT - 1; i++) {
        t[nf_obj[i]].ob_state &= ~SELECTED;
        if (cs.fmt == i + 1 || (i == 0 && (cs.fmt <= AF_NONE || cs.fmt >= AF_COUNT)))
            t[nf_obj[i]].ob_state |= SELECTED;
    }
    if (cs.best)
        t[NF_BEST].ob_state |= SELECTED;
    else
        t[NF_BEST].ob_state &= ~SELECTED;
    wd_redraw_obj(nf_dlg, NF_ROOT);
}

/* a name for a new archive, with the extension of the format */
static int new_archive_name(char *out, const char *suggest)
{
    char path[300], name[64], *s;
    int f;
    if (cs.dest[0])
        strcpy(path, cs.dest);
    else
        strcpy(path, app_folder);
    strcat(path, "*.");
    strcat(path, af_ext(cs.fmt));
    if (suggest) {
        const char *b = base_name(suggest), *dot = strrchr(b, '.');
        size_t n = dot && dot != b ? (size_t)(dot - b) : strlen(b);
        if (n > 8) n = 8;
        memcpy(name, b, n);
        name[n] = 0;
    } else
        strcpy(name, "ARCHIVE");
    strcat(name, ".");
    strcat(name, af_ext(cs.fmt));
    if (!fa_file_select(path, name, "Name of the new archive"))
        return 0;
    s = strrchr(path, '\\');
    if (!s || !name[0])
        return 0;
    s[1] = 0;
    strcpy(out, path);
    strcat(out, name);
    f = af_from_name(out);
    if (f == AF_NONE && !strchr(name, '.')) {
        strcat(out, ".");
        strcat(out, af_ext(cs.fmt));
    }
    return 1;
}

static void do_new(void)
{
    new_archive_begin(NA_NEW);
}

/* the format is chosen: the name, then the archive or the packing */
static void new_archive_go(int kind)
{
    char arc[300];
    int fmt;
    if (kind == NA_PACK) {
        pack_loose();
        return;
    }
    if (!new_archive_name(arc, 0))
        return;
    if (exists(arc)) {
        fmt = af_detect(arc);
        if (fmt == AF_NONE) {
            alertf("[1][FArchive: %.30s|exists and is not an archive.][ OK ]", base_name(arc));
            return;
        }
        aw_open(arc, fmt, 0);
        return;
    }
    fmt = af_from_name(arc);
    aw_open(arc, fmt != AF_NONE ? fmt : cs.fmt, 1);
}

static void do_add_files(void)
{
    AWIN *w = top_awin();
    char full[300];
    char *one[1];
    if (!w)
        return;
    if (!pick_file(full, "Add to the archive (no name: folder)", 1))
        return;
    one[0] = full;
    add_files_ask(w, one, 1);
}

/* the selected members only (nothing selected: nothing to do) */
static void do_extract_sel(void)
{
    AWIN *w = top_awin();
    if (!w || w->isnew || !aw_count_sel(w))
        return;
    queue_extract(w->path, w, 1);
    begin();
}

/* every member of the archive, whatever is selected */
static void do_extract_all(void)
{
    AWIN *w = top_awin();
    if (!w || w->isnew)
        return;
    queue_extract(w->path, w, 0);
    begin();
}

/* ---------------------------------------------------------- finding */

/* does one path part (or the whole name) match? Case doesn't matter;
   * any run of characters, ? any one */
static int wild(const char *p, const char *s, const char *end)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            for (p++; ; s++) {
                if (wild(p, s, end))
                    return 1;
                if (s >= end)
                    return 0;
            }
        }
        if (s >= end)
            return 0;
        if (*p != '?' && toupper((unsigned char)*p) != toupper((unsigned char)*s))
            return 0;
    }
    return s >= end;
}

/* a member matches: with * or ? the pattern is matched against each part of
   its path (a file or any folder on the way); otherwise the text is looked
   for anywhere in the path. "readme" finds DOCS/README.TXT, "*.c" every C
   file, "src" the folder SRC/ and what's in it. */
static int find_match(const char *name, const char *pat)
{
    const char *s, *part;
    size_t n = strlen(pat);
    if (!*pat)
        return 0;
    if (!strpbrk(pat, "*?")) {
        for (s = name; *s; s++) {
            size_t k;
            for (k = 0; k < n && s[k] &&
                 toupper((unsigned char)s[k]) == toupper((unsigned char)pat[k]); k++) ;
            if (k == n)
                return 1;
        }
        return 0;
    }
    for (part = name; *part; ) {
        const char *e = part;
        while (*e && *e != '/' && *e != '\\')
            e++;
        if (e > part && wild(pat, part, e))
            return 1;
        part = *e ? e + 1 : e;
    }
    return 0;
}

/* bring row i into view (a third of the way down when it has to move) */
static void aw_show_row(AWIN *w, int i)
{
    int vis = aw_rows(w), r;
    w->anchor = i;
    if (i < 0 || i >= w->n || !w->rowof || (r = w->rowof[i]) < 0)
        return;
    if (r < w->top || r >= w->top + vis)
        w->top = r - vis / 3;
}

/* select every match, show the first */
static void find_all(AWIN *w)
{
    int i, n = 0, first = -1;
    for (i = 0; i < w->n; i++) {
        w->e[i].sel = (u8)(w->rowof && w->rowof[i] >= 0 &&
                           find_match(w->e[i].name, find_pat));
        if (w->e[i].sel) {
            n++;
            if (first < 0) first = i;
        }
    }
    if (first < 0) {
        aw_changed(w);
        alertf("[1][FArchive: nothing in this|archive matches|\"%.30s\".][ OK ]", find_pat);
        return;
    }
    aw_show_row(w, first);
    aw_changed(w);
    log_add("Find \"%s\" in %s: %d match%s", find_pat, base_name(w->path), n, n == 1 ? "" : "es");
}

/* Archive > Find: a window of its own; the archive it is for is the one
   on top when Find was chosen (found by its handle and serial again) */
typedef struct { int handle; long serial; } FINDFOR;
static WDIALOG *fd_dlg;

static int fd_exit(WDIALOG *d, int obj)
{
    FINDFOR *ff = (FINDFOR *)wd_ctx(d);
    fd_dlg = 0;
    if (obj == FD_OK) {
        const char *t = wd_field(d, FD_FIELD);
        AWIN *w = find_aw_task(ff->handle, ff->serial);
        if (t && *t && w) {
            strncpy(find_pat, t, sizeof find_pat - 1);
            find_pat[sizeof find_pat - 1] = 0;
            find_all(w);
            fa_menu_update();
        }
    }
    free(ff);
    return 1;
}

static void do_find(AWIN *w)
{
    FINDFOR *ff;
    if (!aw_can_extract(w, 0))
        return;
    if (fd_dlg) {                            /* closed without its callback */
        free(wd_ctx(fd_dlg));
        wd_close(fd_dlg);
    }
    fd_dlg = 0;
    ff = (FINDFOR *)malloc(sizeof *ff);
    if (!ff)
        return;
    ff->handle = w->handle;
    ff->serial = w->serial;
    fd_dlg = wd_open(T_FIND, rs(S_W_FIND), FD_FIELD, fd_exit, 0, ff);
    if (!fd_dlg) {
        free(ff);
        return;
    }
    wd_set_field(fd_dlg, FD_FIELD, find_pat);
}

static int aw_filtered(AWIN *w)
{
    return w && w->filt[0];
}

/* Archive > Filter: a window of its own, for the archive on top when
   it was chosen; Show all (or nothing typed) takes the filter away */
static WDIALOG *fl_dlg;

static int fl_exit(WDIALOG *d, int obj)
{
    FINDFOR *ff = (FINDFOR *)wd_ctx(d);
    AWIN *w = find_aw_task(ff->handle, ff->serial);
    fl_dlg = 0;
    if (w && (obj == FL_OK || obj == FL_ALL)) {
        const char *t = obj == FL_OK ? wd_field(d, FL_FIELD) : "";
        while (*t == ' ')
            t++;
        sprintf(w->filt, "%.40s", t);
        aw_refilter(w);
        w->top = 0;
        aw_changed(w);
    }
    free(ff);
    return 1;
}

static void do_filter(AWIN *w)
{
    FINDFOR *ff;
    if (!w || w->isnew)
        return;
    if (fl_dlg) {
        free(wd_ctx(fl_dlg));
        wd_close(fl_dlg);
    }
    fl_dlg = 0;
    if (!(ff = (FINDFOR *)malloc(sizeof *ff)))
        return;
    ff->handle = w->handle;
    ff->serial = w->serial;
    fl_dlg = wd_open(T_FILTER, rs(S_W_FILTER), FL_FIELD, fl_exit, 0, ff);
    if (!fl_dlg) {
        free(ff);
        return;
    }
    wd_set_field(fl_dlg, FL_FIELD, w->filt);
}

/* Archive > Check 8.3 names: is the archive safe to extract on TOS as it
   is? Every member whose name, or a folder on its way, isn't a valid 8.3
   name is selected and listed in the log (Mac files too, unless Skip Mac
   files leaves them out anyway). */
static void lw_open(const char *title, char **line, int n);

static void check83(AWIN *w)
{
    int i, bad = 0, mac = 0, shown = 0, nl = 0;
    char **list;
    if (!aw_can_extract(w, 0))
        return;
    list = (char **)malloc((w->n + 1) * sizeof(char *));
    log_add("8.3 check of %s:", base_name(w->path));
    if (w && w->filt[0]) {                   /* the whole archive: no filter */
        w->filt[0] = 0;
        aw_refilter(w);
    }
    for (i = 0; i < w->n; i++) {
        char part[256];
        const char *p = w->e[i].name, *e;
        int ok = 1;
        w->e[i].sel = 0;
        if (af_mac_meta(w->e[i].name)) {
            mac++;
            if (cs.nomac)
                continue;                    /* left out when extracting */
        }
        while (*p && ok) {
            size_t n;
            for (e = p; *e && *e != '/' && *e != '\\'; e++) ;
            n = (size_t)(e - p);
            if (n && n < sizeof part) {
                memcpy(part, p, n);
                part[n] = 0;
                if (!af_valid83(part))
                    ok = 0;
            }
            p = *e ? e + 1 : e;
        }
        if (!ok) {
            w->e[i].sel = 1;
            if (!bad)
                aw_show_row(w, i);
            bad++;
            if (shown++ < 30)
                log_add("  not 8.3: %s", w->e[i].name);
            if (list && (list[++nl] = (char *)malloc(strlen(w->e[i].name) + 3)) != 0)
                sprintf(list[nl], "  %s", w->e[i].name);
            else if (list)
                nl--;
        }
    }
    if (shown > 30)
        log_add("  ... and %d more", shown - 30);
    aw_changed(w);
    if (!bad) {
        log_add("  all %d member%s have 8.3 names: TOS safe%s", w->n, w->n == 1 ? "" : "s",
                mac && cs.nomac ? " (Mac files left out)" : "");
        alertf("[1][FArchive: every name in|%.30s|is a valid 8.3 name:|safe to extract on TOS.][ OK ]",
               base_name(w->path));
        if (list)
            free(list);
    } else if (list && (list[0] = (char *)malloc(80)) != 0) {
        /* the list in a window of its own: heading, then the names */
        char title[80];
        sprintf(list[0], "%d of %d member%s: not 8.3 names", bad, w->n, w->n == 1 ? "" : "s");
        sprintf(title, "Not 8.3: %.40s", base_name(w->path));
        log_add("  %d member%s with names that aren't 8.3", bad, bad == 1 ? "" : "s");
        lw_open(title, list, nl + 1);
    } else {
        if (list) {
            for (i = 1; i <= nl; i++)
                free(list[i]);
            free(list);
        }
        log_add("  %d member%s with names that aren't 8.3", bad, bad == 1 ? "" : "s");
        alertf("[2][FArchive: %d member%s of|%.30s|have names that aren't 8.3.|They are selected and|listed in the log.][ OK ]",
               bad, bad == 1 ? "" : "s", base_name(w->path));
    }
}

/* Archive > Find next: the next match after the last one shown (wraps) */
static void find_next(AWIN *w)
{
    int i, k;
    if (!aw_can_extract(w, 0) || !find_pat[0])
        return;
    for (k = 1; k <= w->n; k++) {
        i = (w->anchor + k) % w->n;
        if (w->rowof && w->rowof[i] >= 0 && find_match(w->e[i].name, find_pat)) {
            w->e[i].sel = 1;
            w->anchor = w->cur = i;
            aw_show_row(w, i);
            aw_changed(w);
            return;
        }
    }
    alertf("[1][FArchive: nothing in this|archive matches|\"%.30s\".][ OK ]", find_pat);
}

static void select_all(AWIN *w, int on)
{
    int i;
    if (!w)
        return;
    for (i = 0; i < w->n; i++)
        w->e[i].sel = (u8)(on && w->rowof && w->rowof[i] >= 0);
    aw_changed(w);
}

static void changed(void)
{
    settings_save();
    fa_menu_update();
    if (!iconified)
        post_redraw();
}

static void do_menu(int title, int item)
{
    switch (item) {
    case MI_ABOUT:   about(); break;
    case MI_OPEN:    do_open(); break;
    case MI_NEW:     do_new(); break;
    case MI_EXTRACT: do_extract_file(); break;
    case MI_DEST:    choose_dest(); changed(); break;
    case MI_QUIT:    quitting = 1; break;
    case MI_XSEL:    do_extract_sel(); break;
    case MI_XALL:    do_extract_all(); break;
    case MI_FIND:    do_find(top_awin()); break;
    case MI_FNEXT:   find_next(top_awin()); break;
    case MI_ADD:     do_add_files(); break;
    case MI_CUT:     edit_copy(1); break;
    case MI_COPY:    edit_copy(0); break;
    case MI_PASTE:   edit_paste(); break;
    case MI_DELETE:  do_delete(); break;
    case MI_FILTER:  do_filter(top_awin()); break;
    case MI_FIX83:   fix83(top_awin()); break;
    case MI_RENAME:  do_rename(); break;
    case MI_SELALL:  select_all(top_awin(), 1); break;
    case MI_SELNONE: select_all(top_awin(), 0); break;
    case MI_CLOSE:   { AWIN *w = top_awin(); if (w) aw_close(w); } break;
    case MI_XDROP:   cs.xdrop = !cs.xdrop; changed(); break;
    case MI_FOLDER:  cs.folder = !cs.folder; changed(); break;
    case MI_PATHS:   cs.paths = !cs.paths; changed(); break;
    case MI_REPLACE: cs.replace = !cs.replace; changed(); break;
    case MI_SUBDIRS: cs.subdirs = !cs.subdirs; changed(); break;
    case MI_83:      cs.rename83 = !cs.rename83; changed(); break;
    case MI_NOMAC:   cs.nomac = !cs.nomac; changed(); break;
    case MI_CONFIRM: cs.confirm = !cs.confirm; changed(); break;
    case MI_CHECK83: check83(top_awin()); break;
    }
    menu_tnormal(menu, title, 1);
}

/* ------------------------------------------------------------- messages */

/* MultiTOS drag & drop: msg[3] is the window it was dropped on */
static void drag_drop(short *msg)
{
    char pipe[24], ext[32], hdr[256];
    short hlen;
    long fd, size;
    char c;
    int tries;
    strcpy(pipe, "U:\\PIPE\\DRAGDROP.AA");
    pipe[17] = (char)(msg[7] >> 8);
    pipe[18] = (char)(msg[7] & 0xFF);
    fd = Fopen(pipe, 2);
    dbg("AP_DRAGDROP window %d, pipe %s -> %ld", msg[3], pipe, fd);
    if (fd < 0)
        return;
    c = DD_OK;
    Fwrite((short)fd, 1, &c);
    memset(ext, 0, sizeof ext);
    memcpy(ext, "ARGS", 4);
    Fwrite((short)fd, 32, ext);
    for (tries = 0; tries < 8; tries++) {
        char type[5];
        if (Fread((short)fd, 2, &hlen) != 2 || hlen < 8)
            break;
        if (hlen > (short)sizeof hdr) {
            /* a pipe cannot seek: read the rest and drop it */
            long left = hlen - (long)sizeof hdr;
            Fread((short)fd, sizeof hdr, hdr);
            while (left > 0) {
                char junk[64];
                long k = left > 64 ? 64 : left;
                if (Fread((short)fd, k, junk) != k)
                    break;
                left -= k;
            }
        } else
            Fread((short)fd, hlen, hdr);
        memcpy(type, hdr, 4);
        type[4] = 0;
        size = ((long)(u8)hdr[4] << 24) | ((long)(u8)hdr[5] << 16) |
               ((u8)hdr[6] << 8) | (u8)hdr[7];
        dbg("drag & drop offers '%s', %ld bytes", type, size);
        if (!strcmp(type, "ARGS") && size > 0 && size < 32768L) {
            char *buf = malloc(size + 1);
            if (!buf) {
                c = DD_NAK;
                Fwrite((short)fd, 1, &c);
                break;
            }
            c = DD_OK;
            Fwrite((short)fd, 1, &c);
            size = Fread((short)fd, size, buf);
            Fclose((short)fd);             /* let the desktop go first */
            if (size > 0) {
                buf[size] = 0;
                dropped(buf, msg[3]);
            }
            free(buf);
            return;
        }
        c = DD_EXT;
        Fwrite((short)fd, 1, &c);
    }
    Fclose((short)fd);
}

static void va_start_msg(short *msg)
{
    char *args = *(char **)&msg[3];
    short reply[8];
    char copy[1024];
    copy[0] = 0;
    if (args) {
        strncpy(copy, args, sizeof copy - 1);
        copy[sizeof copy - 1] = 0;
    }
    memset(reply, 0, sizeof reply);
    reply[0] = AV_STARTED;
    reply[1] = ap_id;
    reply[3] = msg[3];
    reply[4] = msg[4];
    appl_write(msg[1], 16, reply);
    if (copy[0])
        dropped(copy, 0);
}

/* AV desktops (Thing, Jinnee, TeraDesk...) report files dragged onto one
   of our windows - the icon too - with VA_DRAGACCWIND: msg[3] window,
   msg[4]/[5] mouse position, msg[6]/[7] the names                     */
static void drag_acc_wind(short *msg)
{
    char *args = *(char **)&msg[6];
    char copy[1024];
    if (!args)
        return;
    strncpy(copy, args, sizeof copy - 1);
    copy[sizeof copy - 1] = 0;
    dbg("VA_DRAGACCWIND window %d: '%s'", msg[3], copy);
    dropped(copy, msg[3]);
}

static void av_hello(void)
{
    char *srv = getenv("AVSERVER");
    static char myname[9] = "FARCHIVE";
    short msg[8], id;
    char name[9];
    if (!srv)
        return;
    memset(name, ' ', 8);
    name[8] = 0;
    memcpy(name, srv, strlen(srv) > 8 ? 8 : strlen(srv));
    id = appl_find(name);
    if (id < 0)
        return;
    memset(msg, 0, sizeof msg);
    msg[0] = AV_PROTOKOLL;
    msg[1] = ap_id;
    msg[3] = 0x0012;                         /* VA_START, quoted names */
    *(char **)&msg[6] = myname;
    appl_write(id, 16, msg);
}

static void aw_message(AWIN *w, short *msg)
{
    GRECT *r = (GRECT *)&msg[4];
    switch (msg[0]) {
    case WM_REDRAW:  redraw_done(w->handle); aw_redraw(w, r); break;
    case WM_TOPPED:  wind_set(w->handle, WF_TOP, 0, 0, 0, 0); fa_menu_update(); break;
    case WM_CLOSED:  aw_close(w); break;
    case WM_MOVED:   wind_set_grect(w->handle, WF_CURRXYWH, r); w->full = *r; break;
    case WM_SIZED: {
        GRECT n = *r;
        if (n.g_w < 30 * cell_w) n.g_w = 30 * cell_w;
        if (n.g_h < 6 * cell_h) n.g_h = 6 * cell_h;
        wind_set_grect(w->handle, WF_CURRXYWH, &n);
        aw_changed(w);
        break;
    }
    case WM_FULLED: {
        GRECT now;
        wind_get_grect(w->handle, WF_CURRXYWH, &now);
        if (now.g_w == desk.g_w && now.g_h == desk.g_h && w->full.g_w)
            now = w->full;
        else {
            w->full = now;
            now = desk;
        }
        wind_set_grect(w->handle, WF_CURRXYWH, &now);
        aw_changed(w);
        break;
    }
    case WM_ARROWED: {
        int vis = aw_rows(w);
        switch (msg[4]) {
        case 0: w->top -= vis; break;
        case 1: w->top += vis; break;
        case 2: w->top--; break;
        case 3: w->top++; break;
        }
        aw_changed(w);
        break;
    }
    case WM_VSLID: {
        int vis = aw_rows(w);
        if (w->nv > vis)
            w->top = (long)msg[4] * (w->nv - vis) / 1000;
        aw_changed(w);
        break;
    }
    }
}

/* ------------------------------------------------ the 8.3 list window */

/* A window listing lines of text (Check 8.3 names: the members whose names
   aren't 8.3), with a scroll bar; one at a time, the newest replaces it. */
static struct {
    int    handle;
    char   title[80];
    char **line;
    int    n;
    long   top;
} lw;

static int lw_rows(void)
{
    GRECT wk;
    int n;
    wind_get_grect(lw.handle, WF_WORKXYWH, &wk);
    n = (wk.g_h - 4) / cell_h;
    return n < 1 ? 1 : n;
}

static void lw_slider(void)
{
    int vis = lw_rows();
    if (lw.top > lw.n - vis) lw.top = lw.n - vis;
    if (lw.top < 0) lw.top = 0;
    wind_set(lw.handle, WF_VSLSIZE, lw.n <= vis ? 1000 : (short)(1000L * vis / lw.n), 0, 0, 0);
    wind_set(lw.handle, WF_VSLIDE, lw.n <= vis ? 0 : (short)(1000L * lw.top / (lw.n - vis)), 0, 0, 0);
}

static void lw_free(void)
{
    int i;
    for (i = 0; i < lw.n; i++)
        free(lw.line[i]);
    free(lw.line);
    lw.line = 0;
    lw.n = 0;
    lw.top = 0;
}

static void lw_close(void)
{
    if (lw.handle > 0) {
        wind_close(lw.handle);
        wind_delete(lw.handle);
    }
    lw.handle = 0;
    lw_free();
}

static void lw_redraw(GRECT *area)
{
    GRECT r, a = *area, wk;
    short pxy[4], d, none[4] = { 0, 0, 0, 0 };
    int i, y;
    if (lw.handle <= 0 || !rc_intersect(&desk, &a))
        return;
    wind_get_grect(lw.handle, WF_WORKXYWH, &wk);
    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);
    wind_get_grect(lw.handle, WF_FIRSTXYWH, &r);
    while (r.g_w && r.g_h) {
        if (rc_intersect(&a, &r)) {
            pxy[0] = r.g_x; pxy[1] = r.g_y;
            pxy[2] = r.g_x + r.g_w - 1; pxy[3] = r.g_y + r.g_h - 1;
            vs_clip(vdih, 1, pxy);
            vsf_interior(vdih, 1);
            vsf_color(vdih, 0);
            vswr_mode(vdih, MD_REPLACE);
            vr_recfl(vdih, pxy);
            vst_height(vdih, cell_h > 10 ? 13 : 6, &d, &d, &d, &d);
            vst_alignment(vdih, 0, 5, &d, &d);
            vswr_mode(vdih, MD_TRANS);
            vst_color(vdih, 1);
            y = wk.g_y + 2;
            for (i = (int)lw.top; i < lw.n && y < wk.g_y + wk.g_h; i++, y += cell_h) {
                vst_effects(vdih, i == 0 ? 1 : 0);      /* the heading in bold */
                v_gtext(vdih, wk.g_x + cell_w / 2, y, lw.line[i]);
            }
            vst_effects(vdih, 0);
        }
        wind_get_grect(lw.handle, WF_NEXTXYWH, &r);
    }
    vs_clip(vdih, 0, none);
    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);
}

/* takes the lines (malloc'd array of malloc'd strings) */
static void lw_open(const char *title, char **line, int n)
{
    GRECT r, wk;
    int kind = NAME | CLOSER | MOVER | SIZER | FULLER | UPARROW | DNARROW | VSLIDE;
    lw_close();
    lw.line = line;
    lw.n = n;
    lw.top = 0;
    sprintf(lw.title, " %.70s ", title);
    lw.handle = wind_create_grect(kind, &desk);
    if (lw.handle <= 0) {
        lw.handle = 0;
        lw_free();
        alertf("[1][FArchive: no window free|for the list (it is|in the log too).][ OK ]");
        return;
    }
    wk.g_w = 64 * cell_w;
    wk.g_h = (n < 16 ? n + 1 : 16) * cell_h + 4;
    if (wk.g_h < 6 * cell_h) wk.g_h = 6 * cell_h;
    wk.g_x = wk.g_y = 0;
    wind_calc_grect(WC_BORDER, kind, &wk, &r);
    if (r.g_w > desk.g_w) r.g_w = desk.g_w;
    if (r.g_h > desk.g_h) r.g_h = desk.g_h;
    r.g_x = desk.g_x + (desk.g_w - r.g_w) / 2;
    r.g_y = desk.g_y + (desk.g_h - r.g_h) / 3;
    wind_set_str(lw.handle, WF_NAME, lw.title);
    wind_open_grect(lw.handle, &r);
    lw_slider();
}

/* 1 if the message was for the list window */
static int lw_message(short *msg)
{
    GRECT *r = (GRECT *)&msg[4];
    int vis;
    if (lw.handle <= 0 || msg[3] != lw.handle)
        return 0;
    switch (msg[0]) {
    case WM_REDRAW:  lw_redraw(r); break;
    case WM_TOPPED:  wind_set(lw.handle, WF_TOP, 0, 0, 0, 0); break;
    case WM_CLOSED:  lw_close(); break;
    case WM_MOVED:   wind_set_grect(lw.handle, WF_CURRXYWH, r); break;
    case WM_SIZED:
    case WM_FULLED: {
        GRECT n = *r;
        if (msg[0] == WM_FULLED)
            n = desk;
        if (n.g_w < 20 * cell_w) n.g_w = 20 * cell_w;
        if (n.g_h < 4 * cell_h) n.g_h = 4 * cell_h;
        wind_set_grect(lw.handle, WF_CURRXYWH, &n);
        lw_slider();
        post_redraw_h(lw.handle);
        break;
    }
    case WM_ARROWED:
        vis = lw_rows();
        switch (msg[4]) {
        case 0: lw.top -= vis; break;
        case 1: lw.top += vis; break;
        case 2: lw.top--; break;
        case 3: lw.top++; break;
        }
        lw_slider();
        post_redraw_h(lw.handle);
        break;
    case WM_VSLID:
        vis = lw_rows();
        if (lw.n > vis)
            lw.top = (long)msg[4] * (lw.n - vis) / 1000;
        lw_slider();
        post_redraw_h(lw.handle);
        break;
    default:
        return 0;
    }
    return 1;
}

static void message(short *msg)
{
    GRECT *r = (GRECT *)&msg[4];
    AWIN *w;
    if (msg[0] != WM_REDRAW)
        dbg("message %04x from %d: %d %d %d %d %d", (unsigned short)msg[0], msg[1],
            msg[3], msg[4], msg[5], msg[6], msg[7]);
    if (wd_message(msg))                     /* dialogs and alerts in windows */
        return;
    if (lw_message(msg))                     /* the 8.3 list */
        return;
    switch (msg[0]) {
    case MN_SELECTED:  do_menu(msg[3], msg[4]); return;
    case AP_TERM:      quitting = 1; return;
    case AP_DRAGDROP:  drag_drop(msg); return;
    case VA_START:     va_start_msg(msg); return;
    case VA_DRAGACCWIND: drag_acc_wind(msg); return;
    }
    if (pdlg > 0 && msg[3] == pdlg) {
        switch (msg[0]) {
        case WM_REDRAW: dlg_draw(P_ROOT, r); break;
        case WM_TOPPED: wind_set(pdlg, WF_TOP, 0, 0, 0, 0); break;
        case WM_MOVED:  wind_set_grect(pdlg, WF_CURRXYWH, r); break;
        }
        return;
    }
    if ((w = find_aw(msg[3])) != 0) {
        aw_message(w, msg);
        return;
    }
    if (mainw <= 0 || msg[3] != mainw)
        return;
    switch (msg[0]) {
    case WM_REDRAW:  redraw_done(mainw); main_redraw(r); break;
    case WM_TOPPED:  wind_set(mainw, WF_TOP, 0, 0, 0, 0); fa_menu_update(); break;
    case WM_MOVED:
        wind_set_grect(mainw, WF_CURRXYWH, r);
        if (!iconified)
            main_full = *r;
        break;
    case WM_CLOSED:
        /* closing the window makes it the icon again; closing the icon
           (or File > Quit) ends FArchive                              */
        if (iconified)
            quitting = 1;
        else
            to_icon();
        break;
    case WM_ICONIFY:
    case WM_ALLICONIFY:
        if (!iconified) {
            wind_get_grect(mainw, WF_CURRXYWH, &main_full);
            wind_set_grect(mainw, WF_ICONIFY, r);
            iconified = 1;
            wind_set_str(mainw, WF_NAME, APPNAME);
            post_redraw();
        }
        break;
    case WM_UNICONIFY:
        if (iconified == 1)
            from_icon(r);
        break;
    case WM_SIZED:
        if (!iconified) {
            GRECT n = *r;
            if (n.g_w < 30 * cell_w) n.g_w = 30 * cell_w;
            if (n.g_h < (HDR_LINES + 4) * cell_h) n.g_h = (HDR_LINES + 4) * cell_h;
            wind_set_grect(mainw, WF_CURRXYWH, &n);
            main_full = n;
            clamp_top();
            set_slider();
            post_redraw();
        }
        break;
    case WM_FULLED:
        if (!iconified) {
            static GRECT before;
            GRECT now;
            wind_get_grect(mainw, WF_CURRXYWH, &now);
            if (now.g_w == desk.g_w && now.g_h == desk.g_h && before.g_w)
                now = before;
            else {
                before = now;
                now = desk;
            }
            wind_set_grect(mainw, WF_CURRXYWH, &now);
            main_full = now;
            clamp_top();
            set_slider();
            post_redraw();
        }
        break;
    case WM_ARROWED:
        if (!iconified) {
            int vis = log_visible();
            switch (msg[4]) {
            case 0: logtop -= vis; break;        /* page up   */
            case 1: logtop += vis; break;        /* page down */
            case 2: logtop--; break;             /* line up   */
            case 3: logtop++; break;             /* line down */
            }
            clamp_top();
            set_slider();
            post_redraw();
        }
        break;
    case WM_VSLID:
        if (!iconified) {
            long first = lognum > LOGN ? lognum - LOGN : 0, avail = lognum - first;
            int vis = log_visible();
            if (avail > vis)
                logtop = first + (long)msg[4] * (avail - vis) / 1000;
            clamp_top();
            set_slider();
            post_redraw();
        }
        break;
    }
}

/* a click in an archive window: select it (Shift: add or take away one,
   Control: everything from the last click), double click: extract it  */
/* ------------------------------------------------------------- Edit */

static int lc_same(const char *a, const char *b)
{
    while (*a && toupper((u8)*a) == toupper((u8)*b))
        a++, b++;
    return !*a && !*b;
}

static int lc_prefix(const char *s, const char *pre)
{
    while (*pre && toupper((u8)*s) == toupper((u8)*pre))
        s++, pre++;
    return !*pre;
}

static void names_free(char **names, int n)
{
    int i;
    if (!names)
        return;
    for (i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

static char **names_dup(char **src, int n)
{
    char **d = (char **)calloc(n ? n : 1, sizeof(char *));
    int i;
    if (!d)
        return 0;
    for (i = 0; i < n; i++)
        if (!(d[i] = (char *)malloc(strlen(src[i]) + 1))) {
            names_free(d, i);
            return 0;
        } else
            strcpy(d[i], src[i]);
    return d;
}

/* the selected members' names, copied */
static char **names_sel(AWIN *w, int *n)
{
    char **d;
    int i, k = 0;
    *n = aw_count_sel(w);
    d = (char **)calloc(*n ? *n : 1, sizeof(char *));
    if (!d)
        return 0;
    for (i = 0; i < w->n && k < *n; i++)
        if (w->e[i].sel) {
            if (!(d[k] = (char *)malloc(strlen(w->e[i].name) + 1))) {
                names_free(d, k);
                return 0;
            }
            strcpy(d[k++], w->e[i].name);
        }
    return d;
}

/* Adding to an archive window - members of another archive (a drag or
   Paste) or files: with Options > Confirm changes an alert in a window
   asks first, Replace / Keep old if some are in the archive already
   (nothing waits for it); the windows are found again by handle and
   serial when it is answered */
typedef struct {
    int    files;                            /* names: files, else members */
    char   fpath[256];                       /* members: of this archive   */
    int    ffmt, fh;
    long   fs;
    int    th;                               /* into this window           */
    long   ts;
    char **names;
    int    n;
    int    move;                             /* Cut and Paste              */
    int    repl;                             /* the alert has Keep old     */
} ADDASK;

static struct {                              /* Edit > Cut / Copy          */
    char   path[256];
    int    fmt, h;
    long   s;
    char **names;
    int    n;
    int    cut;
} clip;

/* how: 0 a window not busy, 1 and something selected in a read
   archive, 2 and members can be deleted from it */
static int aw_editable(AWIN *w, int how)
{
    if (!w || w->busy)
        return 0;
    if (how && !aw_can_extract(w, 1))
        return 0;
    return how < 2 || w->fmt != AF_GZ;
}

static int clip_has(void)
{
    return clip.n > 0;
}

static void clip_clear(void)
{
    names_free(clip.names, clip.n);
    clip.names = 0;
    clip.n = 0;
    clip.cut = 0;
}

/* how many of them are in the archive already (and would be replaced) */
static int add_replaced(AWIN *to, ADDASK *a)
{
    int i, k, r = 0;
    for (k = 0; k < to->n; k++) {
        const char *m = to->e[k].name;
        if (to->e[k].flags & 1)
            continue;
        for (i = 0; i < a->n; i++) {
            if (!a->files) {
                if (lc_same(m, a->names[i]))
                    break;
            } else {
                char b[260];
                size_t l;
                sprintf(b, "%.250s", base_name(a->names[i]));
                l = strlen(b);
                while (l && (b[l - 1] == '\\' || b[l - 1] == '/'))
                    b[--l] = 0;
                if (!l)
                    continue;
                if (is_dir(a->names[i])) {
                    strcat(b, "/");
                    if (lc_prefix(m, b))
                        break;
                } else if (lc_same(m, b))
                    break;
            }
        }
        if (i < a->n)
            r++;
    }
    return r;
}

static void add_free(ADDASK *a)
{
    names_free(a->names, a->n);
    free(a);
}

static void add_run(ADDASK *a, int clr)
{
    AWIN *to = find_aw_task(a->th, a->ts);
    if (to) {
        if (a->files) {
            queue_add(to->path, to->fmt, a->names, a->n, to);
            if (tn > 0 && tasks[tn - 1].op == OP_ARC_ADD)
                tasks[tn - 1].clrf = clr;
            begin();
        } else {
            copy_members(a->fpath, a->ffmt, a->fh, a->fs, a->names, a->n, to, clr, a->move);
            if (a->move && same_path(clip.path, a->fpath))
                clip_clear();                /* they are moving: gone after */
        }
    }
    add_free(a);
}

static void add_answer(int button, void *ctx)
{
    ADDASK *a = (ADDASK *)ctx;
    if (button == 1)
        add_run(a, 0);
    else if (button == 2 && a->repl)
        add_run(a, AJ_REPLACE);              /* Keep old */
    else
        add_free(a);
}

/* a: malloc'd, names its own; taken over */
static void add_request(ADDASK *a)
{
    char al[300];
    AWIN *to = find_aw_task(a->th, a->ts);
    int r;
    if (!to || !a->n) {
        add_free(a);
        return;
    }
    if (!cs.confirm) {
        add_run(a, 0);
        return;
    }
    r = cs.replace ? add_replaced(to, a) : 0;
    if (r) {
        a->repl = 1;
        sprintf(al, "[2][%d of the %d item%s to add|%s already in|%.30s.|Replace %s?]"
                "[Replace|Keep old|Cancel]", r, a->n, a->n == 1 ? "" : "s",
                r == 1 ? "is" : "are", base_name(to->path), r == 1 ? "it" : "them");
    } else if (!a->files)
        sprintf(al, "[2][%s %d member%s of|%.30s|to %.27s?][ %s |Cancel]",
                a->move ? "Move" : "Add", a->n, a->n == 1 ? "" : "s", base_name(a->fpath),
                base_name(to->path), a->move ? "Move" : "Add");
    else {
        add_run(a, 0);                       /* new files: nothing to lose */
        return;
    }
    alert_show(1, al, add_answer, a, 0);
}

static ADDASK *add_new(AWIN *to)
{
    ADDASK *a = (ADDASK *)calloc(1, sizeof(ADDASK));
    if (a) {
        a->th = to->handle;
        a->ts = to->serial;
    }
    return a;
}

/* members of one archive window dropped on another */
static void copy_ask(AWIN *from, AWIN *to)
{
    ADDASK *a;
    if (from == to || from->isnew || !(a = add_new(to)))
        return;
    strcpy(a->fpath, from->path);
    a->ffmt = from->fmt;
    a->fh = from->handle;
    a->fs = from->serial;
    if (!(a->names = names_sel(from, &a->n))) {
        free(a);
        return;
    }
    add_request(a);
}

/* files (taken over: names[] freed after) into an archive window */
static void add_files_ask(struct AWIN *to, char **names, int n)
{
    ADDASK *a = add_new(to);
    if (!a || !(a->names = names_dup(names, n))) {
        free(a);
        return;
    }
    a->files = 1;
    a->n = n;
    add_request(a);
}

static void edit_copy(int cut)
{
    AWIN *w = top_awin();
    if (!aw_can_extract(w, 1) || w->busy)
        return;
    clip_clear();
    if (!(clip.names = names_sel(w, &clip.n)))
        return;
    strcpy(clip.path, w->path);
    clip.fmt = w->fmt;
    clip.h = w->handle;
    clip.s = w->serial;
    clip.cut = cut;
    fa_menu_update();
}

static void edit_paste(void)
{
    AWIN *w = top_awin();
    ADDASK *a;
    if (!w || w->busy || !clip.n)
        return;
    if (same_path(clip.path, w->path)) {
        alertf("[1][Those members are in|this archive already.][ OK ]");
        return;
    }
    if (!exists(clip.path)) {
        alertf("[1][%.30s|is not there any more.][ OK ]", base_name(clip.path));
        clip_clear();
        fa_menu_update();
        return;
    }
    if (!(a = add_new(w)))
        return;
    strcpy(a->fpath, clip.path);
    a->ffmt = clip.fmt;
    a->fh = clip.h;
    a->fs = clip.s;
    a->move = clip.cut;
    a->n = clip.n;
    if (!(a->names = names_dup(clip.names, clip.n))) {
        free(a);
        return;
    }
    add_request(a);
}

/* the members in list (a list file, taken over) out of the archive arc */
static void queue_delete_list(const char *arc, int afmt, const char *list, int n)
{
    TASK *t = task_new(OP_ARC_DELETE, arc);
    AWIN *w = find_aw_path(arc);
    (void)n;
    if (!t) {
        Fdelete(list);
        return;
    }
    t->afmt = afmt;
    strcpy(t->list, list);
    t->nitems = 0;
    {
        FILE *f = fopen(list, "r");
        char line[260];
        if (f) {
            while (fgets(line, sizeof line, f))
                if (line[0] != '\n' && line[0] != '\r')
                    t->nitems++;
            fclose(f);
        }
    }
    if (w)
        t->handle = w->handle, t->serial = w->serial;
}

typedef struct { int h; long s; char **names; int n; } DELASK;

static void delete_run(DELASK *d)
{
    AWIN *w = find_aw_task(d->h, d->s);
    char list[256];
    FILE *f;
    int i;
    if (w && (f = list_file(list, "w")) != 0) {
        for (i = 0; i < d->n; i++) {
            fputs(d->names[i], f);
            fputs("\n", f);
        }
        fclose(f);
        queue_delete_list(w->path, w->fmt, list, d->n);
        begin();
    }
    names_free(d->names, d->n);
    free(d);
}

static void delete_answer(int button, void *ctx)
{
    DELASK *d = (DELASK *)ctx;
    if (button == 1)
        delete_run(d);
    else {
        names_free(d->names, d->n);
        free(d);
    }
}

/* Edit > Rename: one member, or a folder and everything in it, gets a
   new name in the same folder - to make an archive safe for TOS before
   extracting it, say. The archive formats keep names inside their own
   headers (with checksums), so the members are extracted to a temporary
   folder and added again under the new name, the old ones taken out in
   the same rewrite: the copy machinery, with the map file it writes
   pointing the files at their new names.                              */
static void rename_members(AWIN *w, const char *old, const char *nw)
{
    char tmp[256], dir[256], dell[256];
    TASK *t;
    int k, i, nn = 0;
    size_t ol = strlen(old);
    FILE *f, *fd;
    for (k = 0; k < 1000; k++) {
        sprintf(dir, "%sFARN%04d", app_folder, (int)((now_ms() / 7 + k) % 10000));
        if (!exists(dir))
            break;
    }
    if (afs_mkdir(dir) != E_OK) {
        log_add("Cannot make a temporary folder %s", dir);
        return;
    }
    sprintf(tmp, "%s\\", dir);
    t = task_new(OP_ARC_EXTRACT, w->path);
    if (!t) {
        afs_rmtree(dir);
        return;
    }
    strcpy(t->out, tmp);
    t->afmt = w->fmt;
    t->handle = w->handle;
    t->serial = w->serial;
    t->setf = AJ_PATHS | AJ_REPLACE | AJ_SUBDIRS;
    t->clrf = AJ_NOMAC;                      /* all of them: all go out after */
    t->copy_h = w->handle;                   /* back into the same archive */
    t->copy_serial = w->serial;
    sprintf(t->map, "%s.MAP", dir);
    strcpy(t->rnold, old);
    strcpy(t->rnnew, nw);
    f = list_file(t->list, "w");
    fd = f ? list_file(dell, "w") : 0;
    if (!fd) {
        if (f) {
            fclose(f);
            Fdelete(t->list);
        }
        tn--;
        afs_rmtree(dir);
        log_add("Cannot write a list file next to FArchive");
        return;
    }
    for (i = 0; i < w->n; i++) {
        const char *m = w->e[i].name;
        if (!strncmp(m, old, ol) && (!m[ol] || m[ol] == '/')) {
            if (!(w->e[i].flags & 1)) {      /* the files are extracted */
                fputs(m, f);
                fputs("\n", f);
            }
            fputs(m, fd);                    /* all of them taken out */
            fputs("\n", fd);
            nn++;
        }
    }
    fclose(f);
    fclose(fd);
    strcpy(t->dell, dell);
    t->nitems = nn;
    log_add("Renaming %s to %s in %s", old, nw, base_name(w->path));
    w->busy = 1;                             /* until it is read again */
    aw_changed(w);
    begin();
}

/* Archive > Fix 8.3 names: every file and folder name that isn't a
   valid 8.3 name (or clashes with another in its folder once TOS has
   made them capitals) gets one, "Long File Name.txt" -> LONGFI_1.TXT,
   the same way FArchive renames when extracting to a FAT drive - but in
   the archive, so it can be extracted anywhere as it is. Like Rename:
   the members are extracted, added again under their new names and the
   old ones taken out in the same rewrite, which happens only if all of
   them came out. */

typedef struct {
    char **key;                              /* old parent path + "/" + old part */
    char **nw;                               /* ... its new part             */
    char **used;                             /* old parent path + "/" + NEW   */
    int    n, nu, cap, capu;
} FIXT;

static int fx_add(char ***arr, int *n, int *cap, const char *s)
{
    if (*n == *cap) {
        char **a = realloc(*arr, (*cap = *cap ? *cap * 2 : 64) * sizeof(char *));
        if (!a)
            return 0;
        *arr = a;
    }
    if (!((*arr)[*n] = malloc(strlen(s) + 1)))
        return 0;
    strcpy((*arr)[(*n)++], s);
    return 1;
}

static int fx_used(FIXT *x, const char *s)
{
    int i;
    for (i = 0; i < x->nu; i++)
        if (lc_same(x->used[i], s))
            return 1;
    return 0;
}

static void fx_free(FIXT *x)
{
    names_free(x->key, x->n);
    free(x->nw);                             /* (they point into key's) */
    x->key = x->nw = 0;
    names_free(x->used, x->nu);
    x->used = 0;
}

/* the new name of one part (in the folder whose old path is parent) */
static const char *fx_part(FIXT *x, const char *parent, const char *part, int keep)
{
    char key[520], cand[16], u[540];
    int i, k;
    sprintf(key, "%.255s/%.255s", parent, part);
    for (i = 0; i < x->n; i++)
        if (!strcmp(x->key[i], key))
            return x->nw[i];
    if (keep) {                              /* valid already: as it is */
        strcpy(cand, "");
        sprintf(u, "%.255s/%.255s", parent, part);
        if (!fx_used(x, u)) {
            if (x->n == x->cap) {
                char **a = realloc(x->key, (x->cap = x->cap ? x->cap * 2 : 64) * sizeof(char *));
                char **b = a ? realloc(x->nw, x->cap * sizeof(char *)) : 0;
                if (a) x->key = a;
                if (b) x->nw = b;
                if (!a || !b)
                    return 0;
            }
            if (!fx_add(&x->used, &x->nu, &x->capu, u))
                return 0;
            x->key[x->n] = malloc(strlen(key) + 1);
            if (!x->key[x->n])
                return 0;
            strcpy(x->key[x->n], key);
            x->nw[x->n] = x->key[x->n] + strlen(parent) + 1;    /* the part itself */
            return x->nw[x->n++];
        }
    }
    for (k = 0; k < 1000; k++) {             /* LONGFI_1.TXT, _2, ... */
        af_make83(part, cand, k);
        sprintf(u, "%.255s/%s", parent, cand);
        if (!fx_used(x, u))
            break;
    }
    if (k == 1000)
        return 0;
    if (x->n == x->cap) {
        char **a = realloc(x->key, (x->cap = x->cap ? x->cap * 2 : 64) * sizeof(char *));
        char **b = a ? realloc(x->nw, x->cap * sizeof(char *)) : 0;
        if (a) x->key = a;
        if (b) x->nw = b;
        if (!a || !b)
            return 0;
    }
    if (!fx_add(&x->used, &x->nu, &x->capu, u))
        return 0;
    /* key and new part in one block: "key\0new" */
    x->key[x->n] = malloc(strlen(key) + strlen(cand) + 2);
    if (!x->key[x->n])
        return 0;
    strcpy(x->key[x->n], key);
    x->nw[x->n] = x->key[x->n] + strlen(key) + 1;
    strcpy(x->nw[x->n], cand);
    return x->nw[x->n++];
}

/* a member's new path; 0 if it can't be made */
static int fx_path(FIXT *x, const char *old, char *out)
{
    char parent[300], part[256];
    const char *p = old, *e, *np;
    parent[0] = out[0] = 0;
    while (*p) {
        size_t n;
        for (e = p; *e && *e != '/'; e++) ;
        n = (size_t)(e - p);
        if (n >= sizeof part || strlen(out) + n + 14 >= 256)
            return 0;
        memcpy(part, p, n);
        part[n] = 0;
        if (n) {
            np = fx_part(x, parent, part, af_valid83(part));
            if (!np)
                return 0;
            if (out[0])
                strcat(out, "/");
            strcat(out, np);
            if (parent[0])
                strcat(parent, "/");
            strcat(parent, part);
        }
        p = *e ? e + 1 : e;
    }
    return 1;
}

typedef struct { int h; long s; int n; char tab[256], list[256], dell[256]; } FIXASK;

static void fix83_run(FIXASK *f)
{
    AWIN *w = find_aw_task(f->h, f->s);
    char tmp[256], dir[256];
    TASK *t;
    int k;
    if (!w || w->busy) {
        Fdelete(f->tab);
        Fdelete(f->list);
        Fdelete(f->dell);
        free(f);
        return;
    }
    for (k = 0; k < 1000; k++) {
        sprintf(dir, "%sFARN%04d", app_folder, (int)((now_ms() / 7 + k) % 10000));
        if (!exists(dir))
            break;
    }
    if (afs_mkdir(dir) != E_OK || !(t = task_new(OP_ARC_EXTRACT, w->path))) {
        log_add("Cannot make a temporary folder %s", dir);
        Fdelete(f->tab);
        Fdelete(f->list);
        Fdelete(f->dell);
        free(f);
        return;
    }
    sprintf(tmp, "%s\\", dir);
    strcpy(t->out, tmp);
    t->afmt = w->fmt;
    t->handle = w->handle;
    t->serial = w->serial;
    t->setf = AJ_PATHS | AJ_REPLACE | AJ_SUBDIRS;
    t->clrf = AJ_NOMAC;
    t->copy_h = w->handle;
    t->copy_serial = w->serial;
    sprintf(t->map, "%s.MAP", dir);
    t->rnold[0] = 1;                         /* a table, not one rename */
    t->rnold[1] = 0;
    strcpy(t->rnnew, f->tab);
    strcpy(t->list, f->list);
    strcpy(t->dell, f->dell);
    t->nitems = f->n;
    log_add("Fixing %d name%s in %s", f->n, f->n == 1 ? "" : "s", base_name(w->path));
    w->busy = 1;
    aw_changed(w);
    free(f);
    begin();
}

static void fix83_answer(int button, void *ctx)
{
    FIXASK *f = (FIXASK *)ctx;
    if (button == 1)
        fix83_run(f);
    else {
        Fdelete(f->tab);
        Fdelete(f->list);
        Fdelete(f->dell);
        free(f);
    }
}

static void fix83(struct AWIN *w)
{
    FIXT x;
    FIXASK *f;
    FILE *ft = 0, *fl = 0, *fd = 0;
    char nw[300], al[300];
    int i, j, nfix = 0, nfiles = 0, shown = 0, err = 0;
    if (!aw_editable(w, 0) || w->isnew)
        return;
    if (w->fmt == AF_GZ || w->fmt == AF_ARC) {
        alertf("[1][%s archives keep 8.3 names|(or one file) already.][ OK ]", af_name(w->fmt));
        return;
    }
    if (w->filt[0]) {                        /* the whole archive */
        w->filt[0] = 0;
        aw_refilter(w);
    }
    memset(&x, 0, sizeof x);
    if (!(f = (FIXASK *)calloc(1, sizeof *f)))
        return;
    ft = list_file(f->tab, "w");
    fl = ft ? list_file(f->list, "w") : 0;
    fd = fl ? list_file(f->dell, "w") : 0;
    if (!fd) {
        if (ft) { fclose(ft); Fdelete(f->tab); }
        if (fl) { fclose(fl); Fdelete(f->list); }
        free(f);
        log_add("Cannot write a list file next to FArchive");
        return;
    }
    /* first the names that are fine are taken as they are, so a new
       name never takes one of theirs; then the new names            */
    for (i = 0; i < w->n && !err; i++) {
        const char *p = w->e[i].name, *e;
        char parent[300], part[256];
        if (cs.nomac && af_mac_meta(p))
            continue;
        parent[0] = 0;
        while (*p) {
            size_t n;
            for (e = p; *e && *e != '/'; e++) ;
            n = (size_t)(e - p);
            if (n && n < sizeof part) {
                char u[540];
                memcpy(part, p, n);
                part[n] = 0;
                if (af_valid83(part)) {
                    sprintf(u, "%.255s/%.255s", parent, part);
                    if (!fx_used(&x, u))
                        fx_part(&x, parent, part, 1);
                }
                if (parent[0])
                    strcat(parent, "/");
                if (strlen(parent) + n < sizeof parent - 1)
                    strcat(parent, part);
            }
            p = *e ? e + 1 : e;
        }
    }
    for (i = 0; i < w->n && !err; i++) {
        const char *m = w->e[i].name;
        if (cs.nomac && af_mac_meta(m))
            continue;
        if (!fx_path(&x, m, nw)) {
            err = 1;
            break;
        }
        if (strcmp(nw, m)) {
            if (!(w->e[i].flags & 1)) {
                fprintf(fl, "%s\n", m);
                nfiles++;
            } else {                         /* a folder: only with files in it */
                size_t ml = strlen(m);
                for (j = 0; j < w->n; j++)
                    if (!(w->e[j].flags & 1) && !strncmp(w->e[j].name, m, ml) &&
                        w->e[j].name[ml] == '/')
                        break;
                if (j == w->n) {
                    log_add("  %s: an empty folder, left as it is", m);
                    continue;
                }
            }
            fprintf(ft, "%s\t%s\n", m, nw);
            fprintf(fd, "%s\n", m);
            if (shown++ < 40)
                log_add("  %s -> %s", m, nw);
            nfix++;
        }
    }
    fx_free(&x);
    fclose(ft);
    fclose(fl);
    fclose(fd);
    if (err || !nfiles) {
        Fdelete(f->tab);
        Fdelete(f->list);
        Fdelete(f->dell);
        free(f);
        if (err)
            alertf("[1][FArchive: couldn't work out|new names (too many, or out|of memory).][ OK ]");
        else
            alertf("[1][Every name in|%.30s|is a valid 8.3 name.][ OK ]", base_name(w->path));
        return;
    }
    if (shown > 40)
        log_add("  ... and %d more", shown - 40);
    f->h = w->handle;
    f->s = w->serial;
    f->n = nfix;
    /* always asked: it changes many names at once */
    sprintf(al, "[2][Give %d name%s in|%.30s|8.3 names? (listed in the log)][ Fix |Cancel]",
            nfix, nfix == 1 ? "" : "s", base_name(w->path));
    show_log();
    alert_show(2, al, fix83_answer, f, 0);
}

typedef struct { int handle; long serial; int full; char old[256]; } RENFOR;
static WDIALOG *rn_dlg;

#define RN_MAX 64                            /* the field: longer paths name
                                                just the last part       */

/* split a path at its slashes: parts point into buf */
static int path_parts(const char *path, char *buf, char **part, int max)
{
    int n = 0;
    char *p;
    strcpy(buf, path);
    for (p = buf; n < max; ) {
        part[n++] = p;
        p = strchr(p, '/');
        if (!p)
            break;
        *p++ = 0;
    }
    return n;
}

static int rn_bad_name(const char *nb)
{
    return !*nb || strpbrk(nb, "\\:*?\"<>|") || !strcmp(nb, ".") || !strcmp(nb, "..") ||
           nb[0] == ' ' || nb[strlen(nb) - 1] == ' ';
}

static int rn_exit(WDIALOG *d, int obj)
{
    RENFOR *rf = (RENFOR *)wd_ctx(d);
    rn_dlg = 0;
    if (obj == RN_OK) {
        const char *typed = wd_field(d, RN_FIELD);
        AWIN *w = find_aw_task(rf->handle, rf->serial);
        char from[300], to[300], ob[300], nb[300], *op[32], *np[32];
        int i, no, nn, k, diff = -1, ndiff = 0;
        if (!w || w->busy || !typed) {
            free(rf);
            return 1;
        }
        if (rf->full) {
            /* the whole path was shown: which one part changed? */
            no = path_parts(rf->old, ob, op, 32);
            nn = path_parts(typed, nb, np, 32);
            if (no != nn) {
                alertf("[1][Change one name in the path:|folders can't be added|or taken away here.][ OK ]");
                free(rf);
                return 1;
            }
            for (k = 0; k < no; k++)
                if (strcmp(op[k], np[k])) {
                    diff = k;
                    ndiff++;
                }
        } else {                             /* just the last part shown */
            no = path_parts(rf->old, ob, op, 32);
            np[0] = (char *)typed;
            diff = strcmp(op[no - 1], typed) ? no - 1 : -1;
            ndiff = diff >= 0;
            if (diff >= 0)
                np[diff] = (char *)typed;
        }
        if (ndiff > 1)
            alertf("[1][Change one name at a time:|the file's or one folder's.][ OK ]");
        else if (ndiff == 1) {
            const char *newpart = rf->full ? np[diff] : typed;
            if (rn_bad_name(newpart) || strchr(newpart, '/')) {
                alertf("[1][A name can't be empty or|have \\ : * ? \" < > or | in it.][ OK ]");
            } else {
                /* from: the path up to the part that changed; to: the same
                   with the new name in its place                         */
                from[0] = to[0] = 0;
                for (k = 0; k <= diff; k++) {
                    if (k) {
                        strcat(from, "/");
                        strcat(to, "/");
                    }
                    strcat(from, op[k]);
                    strcat(to, k == diff ? newpart : op[k]);
                }
                for (i = 0; i < w->n; i++) {
                    const char *m = w->e[i].name;
                    size_t tl = strlen(to);
                    if (lc_same(m, from) || (lc_prefix(m, from) && m[strlen(from)] == '/'))
                        continue;            /* itself (a change of case) */
                    if (lc_same(m, to) || (lc_prefix(m, to) && m[tl] == '/'))
                        break;
                }
                if (i < w->n)
                    alertf("[1][%.30s|is in the archive already.][ OK ]", newpart);
                else
                    rename_members(w, from, to);
            }
        }
    }
    free(rf);
    return 1;
}

static void do_rename(void)
{
    AWIN *w = top_awin();
    RENFOR *rf;
    int i;
    if (!aw_editable(w, 2) || aw_count_sel(w) != 1)
        return;
    for (i = 0; i < w->n && !w->e[i].sel; i++)
        ;
    if (i >= w->n)
        return;
    if (rn_dlg) {                            /* one at a time */
        free(wd_ctx(rn_dlg));
        wd_close(rn_dlg);
    }
    rn_dlg = 0;
    if (!(rf = (RENFOR *)malloc(sizeof *rf)))
        return;
    rf->handle = w->handle;
    rf->serial = w->serial;
    sprintf(rf->old, "%.250s", w->e[i].name);
    {
        size_t l = strlen(rf->old);
        while (l && rf->old[l - 1] == '/')
            rf->old[--l] = 0;
    }
    rn_dlg = wd_open(T_RENAME, rs(S_W_RENAME), RN_FIELD, rn_exit, 0, rf);
    if (!rn_dlg) {
        free(rf);
        return;
    }
    if (strlen(rf->old) < RN_MAX) {          /* the path: any part can change */
        rf->full = 1;
        wd_set_field(rn_dlg, RN_FIELD, rf->old);
    } else {
        const char *b = strrchr(rf->old, '/');
        rf->full = 0;
        wd_set_field(rn_dlg, RN_FIELD, b ? b + 1 : rf->old);
    }
}

/* Edit > Delete: the selected members, after asking */
static void do_delete(void)
{
    AWIN *w = top_awin();
    DELASK *d;
    char al[200];
    if (!aw_can_extract(w, 1) || w->busy)
        return;
    if (w->fmt == AF_GZ) {
        alertf("[1][A GZ file holds one file:|nothing can be deleted|from it.][ OK ]");
        return;
    }
    if (!(d = (DELASK *)calloc(1, sizeof *d)))
        return;
    d->h = w->handle;
    d->s = w->serial;
    if (!(d->names = names_sel(w, &d->n))) {
        free(d);
        return;
    }
    if (!cs.confirm) {
        delete_run(d);
        return;
    }
    sprintf(al, "[3][Delete %d member%s from|%.30s?|This can't be undone.][Delete|Cancel]",
            d->n, d->n == 1 ? "" : "s", base_name(w->path));
    alert_show(2, al, delete_answer, d, 0);
}

/* Pressed on a selected member: if the mouse moves with the button held,
   an outline follows it; let go over another archive window and the
   selection is copied there. 1 if it was a drag (whatever came of it). */
static int aw_drag(AWIN *w, int mx, int my)
{
    short x, y, b, k, pxy[10], ox, oy;
    int bw = 22 * cell_w, bh = cell_h + 4, moved = 0, h;
    int ux0 = 32767, uy0 = 32767, ux1 = -1, uy1 = -1;   /* where outlines went */
    AWIN *to;
    graf_mkstate(&x, &y, &b, &k);
    if (!(b & 1))
        return 0;
    wind_update(BEG_UPDATE);
    wind_update(BEG_MCTRL);
    /* a solid XOR outline: drawn twice it is gone. (A dotted one isn't:
       the VDI carries the dash pattern on from line to line, and the
       FreeGEM 256-colour drivers draw patterned lines in the wrong places.) */
    vswr_mode(vdih, MD_XOR);
    vsl_color(vdih, 1);
    vsl_type(vdih, 1);
    vsl_width(vdih, 1);
    vs_clip(vdih, 0, pxy);
    ox = oy = -1;
    for (;;) {
        graf_mkstate(&x, &y, &b, &k);
        if (!moved && (x - mx > 3 || mx - x > 3 || y - my > 3 || my - y > 3)) {
            moved = 1;
            graf_mouse(FLAT_HAND, 0);
        }
        if (moved && (x != ox || y != oy)) {
            graf_mouse(M_OFF, 0);
            if (ox >= 0) {                   /* take the old outline away */
                pxy[0] = ox; pxy[1] = oy; pxy[2] = ox + bw; pxy[3] = oy;
                pxy[4] = ox + bw; pxy[5] = oy + bh; pxy[6] = ox; pxy[7] = oy + bh;
                pxy[8] = ox; pxy[9] = oy + 1;
                v_pline(vdih, 5, pxy);
            }
            ox = x - bw / 2; oy = y - bh / 2;
            if (ox < ux0) ux0 = ox;
            if (oy < uy0) uy0 = oy;
            if (ox + bw > ux1) ux1 = ox + bw;
            if (oy + bh > uy1) uy1 = oy + bh;
            pxy[0] = ox; pxy[1] = oy; pxy[2] = ox + bw; pxy[3] = oy;
            pxy[4] = ox + bw; pxy[5] = oy + bh; pxy[6] = ox; pxy[7] = oy + bh;
            pxy[8] = ox; pxy[9] = oy + 1;
            v_pline(vdih, 5, pxy);
            graf_mouse(M_ON, 0);
        }
        if (!(b & 1))
            break;
        evnt_timer(20);
    }
    if (ox >= 0) {
        graf_mouse(M_OFF, 0);
        pxy[0] = ox; pxy[1] = oy; pxy[2] = ox + bw; pxy[3] = oy;
        pxy[4] = ox + bw; pxy[5] = oy + bh; pxy[6] = ox; pxy[7] = oy + bh;
        pxy[8] = ox; pxy[9] = oy + 1;
        v_pline(vdih, 5, pxy);
        graf_mouse(M_ON, 0);
    }
    vswr_mode(vdih, MD_REPLACE);
    graf_mouse(ARROW, 0);
    wind_update(END_MCTRL);
    wind_update(END_UPDATE);
    /* and to be sure: everything the outline passed over is drawn again */
    if (ux1 >= 0) {
        GRECT a;
        a.g_x = ux0 - 2; a.g_y = uy0 - 2;
        a.g_w = ux1 - ux0 + 5; a.g_h = uy1 - uy0 + 5;
        if (rc_intersect(&desk, &a))
            form_dial(FMD_FINISH, 0, 0, 0, 0, a.g_x, a.g_y, a.g_w, a.g_h);
    }
    if (!moved)
        return 0;
    h = wind_find(x, y);
    to = 0;
    {
        int i;
        for (i = 0; i < MAXAW; i++)
            if (aw[i].handle > 0 && aw[i].handle == h)
                to = &aw[i];
    }
    if (to && to != w)
        copy_ask(w, to);
    return 1;
}

static void aw_click(AWIN *w, int mx, int my, int clicks, int ks)
{
    GRECT wk;
    int row, i, vr;
    wind_get_grect(w->handle, WF_WORKXYWH, &wk);
    if (my < wk.g_y + cell_h + 4) {
        wind_set(w->handle, WF_TOP, 0, 0, 0, 0);
        return;
    }
    vr = (int)w->top + (my - wk.g_y - cell_h - 4) / cell_h;
    if (vr < 0 || vr >= w->nv || !w->vis) {
        if (!(ks & (K_LSHIFT | K_RSHIFT | K_CTRL)))
            select_all(w, 0);
        return;
    }
    row = w->vis[vr];                        /* the member on that row */
    /* a press on a selected member, nothing held: maybe a drag to another
       archive window (the selection stays as it is) */
    if (clicks == 1 && !(ks & (K_LSHIFT | K_RSHIFT | K_CTRL)) && w->e[row].sel &&
        aw_drag(w, mx, my))
        return;
    if ((ks & K_CTRL) && w->anchor >= 0 && w->anchor < w->n && w->rowof[w->anchor] >= 0) {
        int ra = w->rowof[w->anchor];
        int a = ra < vr ? ra : vr, b = ra < vr ? vr : ra;
        for (i = a; i <= b; i++)
            w->e[w->vis[i]].sel = 1;
    } else if (ks & (K_LSHIFT | K_RSHIFT)) {
        w->e[row].sel = !w->e[row].sel;
    } else {
        for (i = 0; i < w->n; i++)
            w->e[i].sel = 0;
        w->e[row].sel = 1;
    }
    if (!(ks & K_CTRL))
        w->anchor = row;
    w->cur = row;
    aw_changed(w);
    if (clicks >= 2 && !(w->e[row].flags & 2)) {
        for (i = 0; i < w->n; i++)
            w->e[i].sel = i == row;
        queue_extract(w->path, w, 1);
        begin();
    }
}

static void button(int mx, int my, int clicks, int ks)
{
    int h = wind_find(mx, my);
    AWIN *w;
    if (wd_button(h, mx, my, clicks))        /* a dialog window's */
        return;
    if (lw.handle > 0 && h == lw.handle) {   /* the 8.3 list: just to the front */
        wind_set(lw.handle, WF_TOP, 0, 0, 0, 0);
        return;
    }
    if (pdlg > 0 && h == pdlg) {
        dlg_place();
        if (objc_find(ptree, 0, 8, mx, my) == P_BTN && !(ptree[P_BTN].ob_state & SELECTED))
            dlg_button();
        else
            wind_set(pdlg, WF_TOP, 0, 0, 0, 0);
        return;
    }
    if ((w = find_aw(h)) != 0) {
        aw_click(w, mx, my, clicks, ks);
        return;
    }
    if (h == mainw && iconified == 2) {      /* our own icon: open it */
        GRECT r;
        wind_get_grect(mainw, WF_CURRXYWH, &r);
        from_icon(&r);
    }
}

/* the cursor keys move a selection along the list; with Shift they
   select everything from the last row clicked (or moved to without
   Shift) to the new one, as in a file selector */
static int aw_cursor(AWIN *w, int ks, int scan)
{
    int i, to, from, a, b, page, ra;
    if (!w || w->busy || w->nv <= 0 || !w->vis)
        return 0;
    page = aw_rows(w) > 1 ? aw_rows(w) - 1 : 1;
    from = w->cur >= 0 && w->cur < w->n ? w->rowof[w->cur] : -1;
    switch (scan) {
    case 0x48: to = from < 0 ? w->nv - 1 : from - 1; break;      /* up      */
    case 0x50: to = from < 0 ? 0 : from + 1; break;               /* down    */
    case 0x49: to = (from < 0 ? w->nv : from) - page; break;      /* page up */
    case 0x51: to = (from < 0 ? -1 : from) + page; break;         /* page dn */
    case 0x47: to = 0; break;                                     /* home    */
    case 0x4F: to = w->nv - 1; break;                             /* end     */
    default: return 0;
    }
    if (to < 0) to = 0;
    if (to >= w->nv) to = w->nv - 1;
    if (ks & (K_LSHIFT | K_RSHIFT)) {
        ra = w->anchor >= 0 && w->anchor < w->n ? w->rowof[w->anchor] : -1;
        if (ra < 0) {
            ra = from >= 0 ? from : to;
            w->anchor = w->vis[ra];
        }
        a = ra < to ? ra : to;
        b = ra < to ? to : ra;
        for (i = 0; i < w->n; i++)
            w->e[i].sel = 0;
        for (i = a; i <= b; i++)
            w->e[w->vis[i]].sel = 1;
    } else {
        for (i = 0; i < w->n; i++)
            w->e[i].sel = 0;
        w->e[w->vis[to]].sel = 1;
        w->anchor = w->vis[to];
    }
    w->cur = w->vis[to];
    {
        int keep = w->anchor;
        aw_show_row(w, w->vis[to]);          /* (it sets the anchor) */
        w->anchor = keep;
    }
    aw_changed(w);
    return 1;
}

static void key(int ks, int k)
{
    int c = k & 0xFF;
    if (wd_key(ks, k))                       /* a dialog's field or buttons */
        return;
    if (!(ks & K_CTRL) && aw_cursor(top_awin(), ks, (k >> 8) & 0xFF))
        return;
    if (c == 15) do_open();                  /* ^O */
    else if (c == 14) do_new();              /* ^N */
    else if (c == 5) do_extract_file();      /* ^E */
    else if (c == 20) do_extract_sel();      /* ^T */
    else if (c == 24) edit_copy(1);          /* ^X: cut */
    else if (c == 3) edit_copy(0);           /* ^C */
    else if (c == 22) edit_paste();          /* ^V */
    else if (c == 18) do_rename();           /* ^R */
    else if (c == 12) do_filter(top_awin()); /* ^L */
    else if (((k >> 8) & 0xFF) == 0x53 || c == 0x7F) do_delete();     /* Delete */
    else if (c == 25) do_extract_all();      /* ^Y */
    else if (c == 6) do_find(top_awin());    /* ^F */
    else if (c == 11) check83(top_awin());   /* ^K */
    else if (c == 10) fix83(top_awin());     /* ^J */
    else if (c == 7) find_next(top_awin());  /* ^G */
    else if (c == 1) do_add_files();         /* ^A */
    else if (c == 19) select_all(top_awin(), 1);     /* ^S */
    else if (c == 26) select_all(top_awin(), 0);     /* ^Z */
    else if (c == 21) { AWIN *w = top_awin(); if (w) aw_close(w); }   /* ^U */
    else if (c == 4) { choose_dest(); changed(); }   /* ^D */
    else if (c == 17) quitting = 1;          /* ^Q */
    else if (c == 27 && pdlg > 0) dlg_button();      /* Esc */
}

static void events(long timer, int all)
{
    short msg[8], mx, my, mb, ks, k, clicks, ev;
    int n = 0;
    do {
        ev = evnt_multi(MU_MESAG | MU_KEYBD | MU_BUTTON | MU_TIMER,
                        0x102, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                        msg, timer, &mx, &my, &mb, &ks, &k, &clicks);
        if (ev & MU_MESAG) message(msg);
        if (ev & MU_BUTTON) button(mx, my, clicks, ks);
        if (ev & MU_KEYBD) key(ks, k);
        timer = 0;
    } while (all && (ev & (MU_MESAG | MU_KEYBD | MU_BUTTON)) && ++n < 16 && !quitting);
}

/* called by cooperative jobs (plain TOS) while they work */
int gui_coop_poll(void)
{
    static int in;
    if (in)
        return 0;
    in = 1;
    events(0L, 1);
    if (quitting && cur && !cur->cancel)
        job_cancel(cur);                     /* Quit during a TOS job */
    job_poll();
    in = 0;
    return 0;
}

/* ----------------------------------------------------------------- main */

/* the AES draws check boxes and round radio buttons itself (MagiC) */
static int detect_xbutton(void)
{
    short o1 = 0, o2 = 0, o3 = 0, o4 = 0;
    if (_AESversion < 0x0400 && appl_find("?AGI") < 0)
        return 0;                            /* no appl_getinfo */
    return appl_getinfo(13, &o1, &o2, &o3, &o4) && (o4 & 4);
}

static int detect_3d(void)
{
    short o1 = 0, o2 = 0, o3 = 0, o4 = 0;
    if ((_AESversion >= 0x0400 || appl_find("?AGI") >= 0) &&
        appl_getinfo(13, &o1, &o2, &o3, &o4))
        return o1 != 0;
    return _AESversion >= 0x0340;
}

static int detect_iconify(void)
{
    short o1 = 0, o2 = 0, o3 = 0, o4 = 0;
    if (_AESversion >= 0x0410)
        return 1;
    if (_AESversion < 0x0400 && appl_find("?AGI") < 0)
        return 0;
    if (!appl_getinfo(11, &o1, &o2, &o3, &o4))
        return 0;
    return (o1 & 0x80) != 0;
}

int main(int argc, char **argv)
{
    short work_in[11], work_out[57];
    int i;

    ap_id = appl_init();
    if (ap_id < 0)
        return 1;
    {
        short cw, ch, bw, bh, h;
        h = graf_handle(&cw, &ch, &bw, &bh);
        cell_w = cw;
        cell_h = ch;
        for (i = 0; i < 10; i++) work_in[i] = 1;
        work_in[10] = 2;
        v_opnvwk(work_in, &h, work_out);
        vdih = h;
    }
    if (vdih <= 0) {
        form_alert(1, "[3][FArchive: no VDI workstation.][ Quit ]");
        appl_exit();
        return 1;
    }
    scr_w = work_out[0] + 1;
    scr_h = work_out[1] + 1;
    pix_w_um = work_out[3];
    pix_h_um = work_out[4];
    wind_get_grect(0, WF_WORKXYWH, &desk);
    {
        short out[57];
        vq_extnd(vdih, 1, out);
        scr_planes = out[4];
    }

    prefs_default(&prefs);
    prefs.use_workers = 1;
    find_app_folder();
    settings_load();
    job_app_name = "farchive " FA_VERSION " (" FV_CPU ")";
    job_log_name = "FARCHIVE.LOG";
    job_worker_name = "FAWORK.TTP";
    job_init_system();
    aes_iconify = detect_iconify();
    aes_3d = detect_3d();
    aes_xbutton = detect_xbutton();
#ifndef PCGEM
    if (aes_3d && scr_planes >= 4) {         /* TOS 4's grey; white in 2 colours */
        short c = -1, d2;
        if (objc_sysvar(0, 4 /* BACKGRCOL */, 0, 0, &c, &d2) && c > 0)
            bg3d = c;
    }
#endif
    dbg("AES %04x screen %dx%d planes %d, format %d dest '%s'",
        _AESversion, scr_w, scr_h, scr_planes, cs.fmt, cs.dest);

    if (_AESversion >= 0x0400)
        shel_write(9, 1, 0, 0L, 0L);          /* AP_TERM */
    if (_AESnumapps != 1 || _AESversion >= 0x0400)
        menu_register(ap_id, "  FArchive");

    if (!rsc_open("FARCHIVE", LANG_EN)) {
        form_alert(1, "[3][FArchive: FARCHIVE.RSC|is missing.][ Quit ]");
        v_clsvwk(vdih);
        appl_exit();
        return 1;
    }
    wd_alert_tree = T_ALERT;
    wd_app_title = " FArchive ";
    menu = rs_tree(T_MENU);
    menu_bar(menu, 1);
    fa_menu_update();
    fa_build_icon();
    build_dialog();
    graf_mouse(ARROW, 0);
    av_hello();
    open_main();

    for (i = 1; i < argc; i++)
        drop_path(argv[i]);
    if (argc > 1) {
        loose_done(0);
        begin();
    }

    while (!quitting) {
        events(job_any_active() || want_next ? 100L : 400L, 0);
        job_poll();
        if (want_next)
            start_next();
        job_run_pending();                   /* TOS: the job runs here */
        if (want_next)
            start_next();
    }

    if (cur)
        job_cancel(cur);
    job_kill_all();
    for (i = tpos; i < tn; i++)              /* tasks never started */
        if (tasks[i].list[0])
            Fdelete(tasks[i].list);
    wd_close_all();
    lw_close();
    dlg_close();
    for (i = 0; i < MAXAW; i++)
        if (aw[i].handle > 0)
            aw_close(&aw[i]);
    if (mainw > 0) {
        wind_close(mainw);
        wind_delete(mainw);
    }
    menu_bar(menu, 0);
    v_clsvwk(vdih);
    appl_exit();
    return 0;
}
