/*
 * ZaxHiRes.asi - custom resolution support for Zax: The Alien Hunter
 * (Zax.exe, PE timestamp 2001-08-02, image base 0x400000).
 *
 * Loaded by DxWrapper (LoadPlugins = 1) before the game's entry point runs.
 * Reads the requested resolution: one chosen in the game's Options menu
 * ([ZaxHiRes] in Zax.CFG), else the same way the game does (/ScreenWidth and
 * /ScreenHeight on the command line, else [Options] in Zax.CFG). Always:
 *
 *   - display mode list: every mode the display reports (640x480, or at
 *     least 800x600, any aspect ratio), for the Options menu's resolution
 *     slider; a new choice is saved and applies at the next start
 *
 * and, for anything other than the native 640x480 / 800x600, patches the
 * game's code:
 *
 *   - menu GUI: size the Background, menu window, canvas and its buffer to
 *     the screen instead of a fixed 800x600
 *   - menu centring: children placed with layout mode 5 (parent origin +
 *     offset, coordinates designed for 800x600) on a known full-screen menu
 *     screen get ((W-800)/2, (H-600)/2) added; the Background uses mode 6
 *     (centred) instead so it keeps covering the screen
 *
 * Every patch site is verified against the original bytes first; if any
 * differs, nothing is patched (e.g. when run against an already-patched exe).
 * Results are written to ZaxHiRes.log next to the exe.
 *
 * Built without a C runtime (kernel32/user32 only) so it can't fail to load
 * on a prefix that lacks the UCRT forwarder DLLs.
 */
#include <windows.h>
#include <stdarg.h>
#include <cpuid.h>
#include <immintrin.h>

#define MAX_PATCHES   48
#define MAX_PARENTS   64
#define ZAX_TIMESTAMP 0x3b68d52c

typedef struct {
    DWORD va;
    BYTE  orig[16];
    BYTE  repl[16];
    int   len;
    const char *what;
} Patch;

static Patch patches[MAX_PATCHES];
static int npatches;
static HANDLE logfile = INVALID_HANDLE_VALUE;
static char exedir[MAX_PATH];

/* Menu centring offset, read by the mode 5 hook. */
int center_dx __asm__("zh_center_dx");
int center_dy __asm__("zh_center_dy");
DWORD mode5_resume __asm__("zh_mode5_resume") = 0x40df6b;

/* Parent window classes (vtables) seen by the hook, logged once each. */
static DWORD seen_parents[MAX_PARENTS];
static int nseen;

static void copy(void *dst, const void *src, int n)
{
    BYTE *d = dst;
    const BYTE *s = src;
    while (n--)
        *d++ = *s++;
}

static int same(const void *a, const void *b, int n)
{
    const BYTE *x = a, *y = b;
    while (n--)
        if (*x++ != *y++)
            return 0;
    return 1;
}

static void logmsg(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    DWORD n;

    if (logfile == INVALID_HANDLE_VALUE)
        return;
    va_start(ap, fmt);
    n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    buf[n++] = '\r';
    buf[n++] = '\n';
    WriteFile(logfile, buf, n, &n, NULL);
}

static void add(DWORD va, const BYTE *orig, const BYTE *repl, int len, const char *what)
{
    Patch *p = &patches[npatches++];
    p->va = va;
    copy(p->orig, orig, len);
    copy(p->repl, repl, len);
    p->len = len;
    p->what = what;
}

/* Replace the trailing imm32 of an instruction (opcode bytes + imm32). */
static void add_imm32(DWORD va, const BYTE *opcode, int oplen, DWORD oldv, DWORD newv, const char *what)
{
    BYTE o[8], r[8];
    copy(o, opcode, oplen);
    copy(r, opcode, oplen);
    copy(o + oplen, &oldv, 4);
    copy(r + oplen, &newv, 4);
    add(va, o, r, oplen + 4, what);
}

static void add_nops(DWORD va, const BYTE *orig, int len, const char *what)
{
    BYTE r[8];
    int i;
    for (i = 0; i < len; i++)
        r[i] = 0x90;
    add(va, orig, r, len, what);
}

/*
 * Full-screen menu screens whose children are laid out in 800x600 design
 * coordinates, identified by vtable. Matching by size doesn't work: screens
 * add their children before they are resized (main menu is 677x11 then), and
 * dialogs are top-level too but already centred by the game.
 */
static const DWORD centered_screens[] = {
    0x6055d4,   /* main menu (constructor 0x5be4a0) */
};

/*
 * Window objects keep their vtable at +0x00, rect at +0x04 (left, top,
 * right, bottom) and parent pointer at +0xe8. Called from the mode 5 hook
 * for every child placed that way; returns non-zero if the child's offsets
 * should get the centring offset.
 */
int should_center(const DWORD *parent) __asm__("zh_should_center");
int should_center(const DWORD *parent)
{
    int left = parent[1], top = parent[2];
    int w = parent[3] - left, h = parent[4] - top;
    int center = 0;
    int i;

    for (i = 0; i < (int)(sizeof centered_screens / sizeof centered_screens[0]); i++)
        if (parent[0] == centered_screens[i])
            center = 1;

    for (i = 0; i < nseen; i++)
        if (seen_parents[i] == parent[0])
            return center;
    if (nseen < MAX_PARENTS) {
        seen_parents[nseen++] = parent[0];
        logmsg("mode 5 parent: vtable 0x%06lx  rect %d,%d %dx%d  parent 0x%08lx  -> %s",
               parent[0], left, top, w, h, parent[0x3a], center ? "centred" : "unchanged");
    }
    return center;
}

/*
 * Replaces 0x40df61..0x40df6a in layout mode 5 (0x40df53), where
 *   ebx = parent y origin, eax at entry; ebp = y offset; edi = x offset.
 * Re-executes the overwritten instructions, then adds the centring offset to
 * the child's x/y offsets when should_center() says so.
 */
__attribute__((naked)) static void mode5_hook(void)
{
    __asm__ volatile(
        "movl 0xe8(%esi), %ecx\n\t"     /* original: mov ecx,[esi+0xe8] */
        "movl %eax, %ebx\n\t"           /* original: mov ebx,eax */
        "addl %ebp, %ebx\n\t"           /* original: add ebx,ebp */
        "pushl %ecx\n\t"
        "pushl %edx\n\t"
        "pushl %ecx\n\t"
        "call zh_should_center\n\t"
        "addl $4, %esp\n\t"
        "popl %edx\n\t"
        "popl %ecx\n\t"
        "testl %eax, %eax\n\t"
        "jz 1f\n\t"
        "addl zh_center_dy, %ebx\n\t"
        "addl zh_center_dx, %edi\n\t"
        "1:\n\t"
        "jmp *zh_mode5_resume\n\t");
}

static void add_jmp(DWORD va, const BYTE *orig, int len, void *target, const char *what)
{
    BYTE r[16];
    DWORD rel = (DWORD)target - (va + 5);
    int i;

    r[0] = 0xe9;
    copy(r + 1, &rel, 4);
    for (i = 5; i < len; i++)
        r[i] = 0x90;
    add(va, orig, r, len, what);
}

static const BYTE OP_PUSH[]    = { 0x68 };
static const BYTE OP_MOV_ESI[] = { 0xbe };
static const BYTE OP_MOV_EBX[] = { 0xbb };

/*
 * HUD bar (drawn every frame by 0x59e730). Designed for 640 wide: left panel
 * (179) + right group (461, drawn at W - 461). At exactly 800 wide the game
 * draws one 160-wide filler panel between them (0x59ed7a) and sets two
 * content offsets in its frame: [ebp-0x1c] = filler width (= W - 640) for
 * the right group's contents, [ebp-0x20] = 0x50 (= (W - 640) / 2) passed to
 * the inventory renderers (item, weapon), which place themselves at W/2 +
 * constant; one renderer type gets -0x50 instead (0x59fc2c). The 800 check
 * (0x59ed43) is forced by build_patches; here the filler blit is replaced
 * so the offsets are set for the real width and the filler is repeated
 * across the whole W - 640 gap.
 */
typedef void (__attribute__((thiscall)) *Blit4Fn)(void *dst, void *src, int x, int y, int flags);
int hud_dx __asm__("zh_hud_dx");
int hud_half_dx __asm__("zh_hud_half_dx");

void __attribute__((thiscall)) hud_filler(void *dst, void *src, int x, int y, int flags) __asm__("zh_hud_filler");
void __attribute__((thiscall)) hud_filler(void *dst, void *src, int x, int y, int flags)
{
    Blit4Fn blit = (Blit4Fn)0x4b3280;
    RECT r;
    int fw, end = x + hud_dx, px;

    ((void (__attribute__((thiscall)) *)(void *, RECT *))(*(DWORD **)src)[0x90 / 4])(src, &r);
    fw = r.right - r.left;
    if (fw <= 0)
        return;
    for (px = x; px < end; px += fw)
        blit(dst, src, px + fw > end ? end - fw : px, y, flags);
}

/* Replaces the call at 0x59ed7a; ebp is still the HUD function's frame. */
__attribute__((naked)) static void hud_filler_hook(void)
{
    __asm__ volatile(
        "movl zh_hud_dx, %eax\n\t"
        "movl %eax, -0x1c(%ebp)\n\t"
        "movl zh_hud_half_dx, %eax\n\t"
        "movl %eax, -0x20(%ebp)\n\t"
        "jmp zh_hud_filler\n\t");
}

static void add_call(DWORD va, const BYTE *orig, void *target, const char *what)
{
    BYTE r[5];
    DWORD rel = (DWORD)target - (va + 5);
    r[0] = 0xe8;
    copy(r + 1, &rel, 4);
    add(va, orig, r, 5, what);
}

/*
 * Inventory screen (CZaxInventoryScreen, vtable 0x5ee340, created 0x428c00):
 * art 800 wide with an 80-px rock margin on each side. It slides up from the
 * bottom; 0x4295be moves it to x = -80 on 640-wide screens (hiding the
 * margins) and x = 0 otherwise. At higher resolutions nothing is drawn beside
 * it. inv_move centres it; inv_fill (just before the finished frame is
 * presented, 0x4f3124) repeats its rock margins out to the screen edges.
 */
#define INV_MARGIN 80
static const DWORD *surf_rows(const void *surf);
static BYTE *surf_base(const void *surf);
typedef void (__attribute__((thiscall)) *MoveToFn)(void *win, int x, int y);
static DWORD *inv_window;
static int inv_screen_w, inv_screen_h;

void __attribute__((thiscall)) inv_move(DWORD *win, int x, int y) __asm__("zh_inv_move");
void __attribute__((thiscall)) inv_move(DWORD *win, int x, int y)
{
    int width = (int)(win[3] - win[1]);
    inv_window = win;
    if (x == 0 && width > 0 && width < inv_screen_w)
        x = (inv_screen_w - width) / 2;
    ((MoveToFn)0x40d630)(win, x, y);
}

static void inv_fill_row(WORD *row, int x0, int x1)
{
    int dx, i;
    /* left margin [x0, x0 + 80) repeated leftwards */
    for (dx = x0 - INV_MARGIN; dx > -INV_MARGIN; dx -= INV_MARGIN)
        for (i = dx < 0 ? -dx : 0; i < INV_MARGIN; i++)
            row[dx + i] = row[x0 + i];
    /* right margin [x1 - 80, x1) repeated rightwards */
    for (dx = x1; dx < inv_screen_w; dx += INV_MARGIN)
        for (i = 0; i < INV_MARGIN && dx + i < inv_screen_w; i++)
            row[dx + i] = row[x1 - INV_MARGIN + i];
}

void __attribute__((thiscall)) inv_present(void *dst, void *src, int x, int y, int flags) __asm__("zh_inv_present");
void __attribute__((thiscall)) inv_present(void *dst, void *src, int x, int y, int flags)
{
    const DWORD *win = inv_window;

    if (win && (int)win[2] < inv_screen_h && (int)win[1] >= INV_MARGIN &&
        (int)win[3] <= inv_screen_w - INV_MARGIN && (int)(win[3] - win[1]) >= 2 * INV_MARGIN) {
        const DWORD *rows = surf_rows(src);
        BYTE *base = surf_base(src);
        int yy, top = (int)win[2] < 0 ? 0 : (int)win[2];
        if (rows && base)
            for (yy = top; yy < inv_screen_h; yy++)
                inv_fill_row((WORD *)(base + rows[yy] * 2), (int)win[1], (int)win[3]);
    }
    ((Blit4Fn)0x4b3280)(dst, src, x, y, flags);
}

static int cmdline_has(const char *cmdline, const char *name);

static void build_patches(DWORD w, DWORD h)
{
    /* Menu screen setup (0x5be4a0): Background was only resized when the
       screen was exactly 800 wide; menu window/canvas/buffer were 800x600. */
    add_nops(0x5be637, (const BYTE[]){ 0x75, 0x23 }, 2, "Background: always resize");
    add_imm32(0x5be641, OP_PUSH, 1, 600, h, "Background height");
    add_imm32(0x5be8b9, OP_PUSH, 1, 600, h, "menu window height");
    add_imm32(0x5be8be, OP_PUSH, 1, 800, w, "menu window width");
    add_imm32(0x5beb31, OP_PUSH, 1, 800, w, "menu canvas width");
    add_imm32(0x5beb3d, OP_PUSH, 1, 600, h, "menu canvas height");
    add_imm32(0x5beb83, OP_PUSH, 1, 600, h, "menu buffer height");
    add_imm32(0x5beb88, OP_PUSH, 1, 800, w, "menu buffer width");

    /* Version label ([screen+0x154]) is moved to (800 - w, 600 - h), i.e.
       the bottom-right of an 800x600 screen (0x5bf2ec; 720x540 on 640). */
    add_imm32(0x5bf2ec, OP_MOV_ESI, 1, 800, w, "version label: right edge");
    add_imm32(0x5bf2f1, OP_MOV_EBX, 1, 600, h, "version label: bottom edge");

    /* Terrain tile cache: tiles are composited into a pool of fixed size
       0x480000 (4.5 MB = 576 tiles of 64x64x16 bit, shared with other
       users), created at 0x53f4f0. A 1920x1080 view needs ~527 tiles, so the
       pool thrashes and tiles get recomposited every frame. Size it for the
       resolution with 3x headroom, never below the original. */
    if (!cmdline_has(GetCommandLineA(), "/NoPoolFix")) {
        DWORD pool = (w / 64 + 2) * (h / 64 + 2) * 64 * 64 * 2 * 3;
        if (pool < 0x480000)
            pool = 0x480000;
        add_imm32(0x53f4f0, OP_PUSH, 1, 0x480000, pool, "terrain tile cache pool size");
        logmsg("terrain tile cache pool: %lu KB (was 4608 KB)", pool / 1024);
    }

    /* Layout branches taken only when the screen is exactly 800 wide; both
       read real widths from their objects. */
    add_nops(0x59de0f, (const BYTE[]){ 0x75, 0x0c }, 2, "layout: width==800 branch (0x59de0a)");
    add_nops(0x59ed48, (const BYTE[]){ 0x75, 0x35 }, 2, "layout: width==800 branch (0x59ed43)");

    /* HUD bar: offsets for the real width, filler repeated across the gap. */
    hud_dx = w - 640;
    hud_half_dx = (w - 640) / 2;
    add_call(0x59ed7a, (const BYTE[]){ 0xe8, 0x01, 0x45, 0xf1, 0xff }, hud_filler_hook,
             "HUD: offsets for the real width, filler across the gap");
    add_imm32(0x59fc2c, (const BYTE[]){ 0xc7, 0x45, 0xe0 }, 3, (DWORD)-0x50, (DWORD)-hud_half_dx,
              "HUD: left-side inventory renderer offset");

    /* Inventory screen: centred, rock margins repeated to the screen edges. */
    inv_screen_w = w;
    inv_screen_h = h;
    add_call(0x4295be, (const BYTE[]){ 0xe8, 0x6d, 0x40, 0xfe, 0xff }, (void *)inv_move,
             "inventory: centre horizontally");
    add_call(0x4f3124, (const BYTE[]){ 0xe8, 0x57, 0x01, 0xfc, 0xff }, (void *)inv_present,
             "inventory: rock margins to the screen edges");

    /* Menu centring: Background switches from mode 5 to mode 6 (centre in
       parent), everything else placed with mode 5 goes through the hook. */
    add(0x5be66a, (const BYTE[]){ 0x6a, 0x05 }, (const BYTE[]){ 0x6a, 0x06 }, 2, "Background: layout mode 5 -> 6");
    add_jmp(0x40df61, (const BYTE[]){ 0x8b, 0x8e, 0xe8, 0x00, 0x00, 0x00, 0x8b, 0xd8, 0x03, 0xdd }, 10,
            mode5_hook, "layout mode 5: centring hook");
}

/*
 * Resolution in the Options menu. The game's display object
 * ([0x6c02cc]+4) keeps up to 10 modes (count +0x4c, entries (width, height,
 * format) at +0x54 + i*12), rebuilt by every mode set (0x567f20: count = 0,
 * then EnumDisplayModes with the callback 0x567d60, which only took 4:3
 * 640x480 / 800x600). The Options dialog showed them on a slider (label
 * "%d X %d"); closing it (0x591750) switched to the chosen mode at once, which
 * the patches made for the start-up resolution can't follow. Instead:
 *   - the slider becomes a drop-down (the class of the Colors selector above
 *     it, ctor 0x408cd0) listing every collected mode, not just the game's 10;
 *     it opens on the resolution pending for the next start
 *   - mode_cb replaces 0x567d60: all reported modes at the colour depth, 640x480
 *     or at least 800x600, deduplicated; more than 10: the current mode, the
 *     largest and 640x480 (movies), then the largest's aspect ratio first,
 *     then larger first; sorted ascending
 *   - res_chosen (from 0x5917d3, the "slider moved" branch): saves the choice
 *     to Zax.CFG ([Options] and [ZaxHiRes]) and shows the game's
 *     restart-required message, like a colour depth change does
 *   - at start a [ZaxHiRes] choice wins over /ScreenWidth /ScreenHeight: the
 *     game's command-line reads (0x590d73, 0x590d81) return it
 */
#define MAX_MODES  64
#define GAME_MODES 10
typedef struct { DWORD w, h, fmt; } Mode;
static Mode modes[MAX_MODES];
static int nmodes;
static Mode menu[MAX_MODES];        /* the drop-down's entries, ascending */
static int nmenu;
static DWORD menu_list[5];          /* the game's string list (0x14 bytes) for the drop-down */
static void *res_combo __asm__("zh_res_combo");
static DWORD start_w, start_h;      /* the resolution this run starts with */
static char cfg_path[MAX_PATH];

static BYTE *display_obj(void)
{
    return *(BYTE **)(*(BYTE **)0x6c02cc + 4);
}

static int mode_area_cmp(const Mode *a, const Mode *b, const Mode *big)
{
    /* -1 if a should be kept before b: big's aspect ratio, then larger */
    int sa = a->w * big->h == a->h * big->w, sb = b->w * big->h == b->h * big->w;
    if (sa != sb)
        return sa ? -1 : 1;
    return a->w * a->h > b->w * b->h ? -1 : a->w * a->h < b->w * b->h ? 1 : 0;
}

/* Copy up to GAME_MODES of the collected modes into the game's list. */
static void fill_game_modes(void)
{
    BYTE *disp = display_obj();
    Mode pick[GAME_MODES], *big = &modes[0], t;
    int keep[MAX_MODES] = { 0 }, n = 0, i, j, best;

    for (i = 1; i < nmodes; i++)
        if (modes[i].w * modes[i].h > big->w * big->h)
            big = &modes[i];
    for (i = 0; i < nmodes; i++)
        if (&modes[i] == big || (modes[i].w == start_w && modes[i].h == start_h) ||
            (modes[i].w == 640 && modes[i].h == 480))     /* movies switch to 640x480 */
            keep[i] = 1;
    for (i = 0; i < nmodes && n < GAME_MODES; i++)
        if (keep[i])
            pick[n++] = modes[i];
    while (n < GAME_MODES) {
        best = -1;
        for (i = 0; i < nmodes; i++)
            if (!keep[i] && (best < 0 || mode_area_cmp(&modes[i], &modes[best], big) < 0))
                best = i;
        if (best < 0)
            break;
        keep[best] = 1;
        pick[n++] = modes[best];
    }
    for (i = 1; i < n; i++)                 /* ascending: width, then height */
        for (j = i; j > 0 && (pick[j].w < pick[j - 1].w ||
                              (pick[j].w == pick[j - 1].w && pick[j].h < pick[j - 1].h)); j--) {
            t = pick[j]; pick[j] = pick[j - 1]; pick[j - 1] = t;
        }
    for (i = 0; i < n; i++)
        copy(disp + 0x54 + i * 12, &pick[i], 12);
    *(DWORD *)(disp + 0x4c) = n;
}

/* EnumDisplayModes callback (replaces 0x567d60): desc = DDSURFACEDESC,
   want_bpp = the colour depth being set. */
static HRESULT __attribute__((stdcall)) mode_cb(const BYTE *desc, const DWORD *want_bpp)
{
    DWORD h = *(const DWORD *)(desc + 0x08), w = *(const DWORD *)(desc + 0x0c);
    DWORD bpp = *(const DWORD *)(desc + 0x54), gmask = *(const DWORD *)(desc + 0x5c);
    BYTE *disp = display_obj();
    int i;

    if (*(DWORD *)(disp + 0x4c) == 0)       /* a new enumeration */
        nmodes = 0;
    if (bpp != *want_bpp || !((w == 640 && h == 480) || (w >= 800 && h >= 600)))
        return 1;
    for (i = 0; i < nmodes; i++)
        if (modes[i].w == w && modes[i].h == h)
            return 1;
    if (nmodes == MAX_MODES)
        return 1;
    modes[nmodes].w = w;
    modes[nmodes].h = h;
    modes[nmodes].fmt = bpp == 16 ? (gmask == 0x7e0 ? 0x10 : 0x0f) : bpp;   /* as the game stores it */
    nmodes++;
    fill_game_modes();
    return 1;
}

typedef void *(__attribute__((fastcall)) *AllocFn)(DWORD size);
typedef void (__attribute__((thiscall)) *MessageCtorFn)(void *win, const char *title, const char *text, int flags);
typedef void (__attribute__((thiscall)) *AddChildFn)(void *parent, void *child, int mode);

/* Called from the Options dialog's close handler when the resolution slider
   moved: save the choice for the next start and say so. */
void __attribute__((stdcall)) res_chosen(BYTE *dialog) __asm__("zh_res_chosen");
void __attribute__((stdcall)) res_chosen(BYTE *dialog)
{
    static const char title[] = "Screen Resolution Change";
    static const char text[] = "The change to the screen resolution will not take\n"
                               "effect until next time the game is restarted.";
    DWORD idx = *(DWORD *)(dialog + 0x178);
    DWORD w, h;
    char num[16];
    void *msg;

    if (idx >= (DWORD)nmenu)
        return;
    w = menu[idx].w;
    h = menu[idx].h;
    logmsg("Options menu: resolution %lux%lu chosen, applies at the next start", w, h);
    wsprintfA(num, "%lu", w);
    WritePrivateProfileStringA("Options", "Screen Width", num, cfg_path);
    WritePrivateProfileStringA("ZaxHiRes", "Screen Width", num, cfg_path);
    wsprintfA(num, "%lu", h);
    WritePrivateProfileStringA("Options", "Screen Height", num, cfg_path);
    WritePrivateProfileStringA("ZaxHiRes", "Screen Height", num, cfg_path);
    msg = ((AllocFn)0x417710)(0x11c);       /* as the colour depth message, 0x59177a */
    if (msg)
        ((MessageCtorFn)0x403b30)(msg, title, text, 0);
    ((AddChildFn)0x40c6e0)(dialog, msg, 6);
}

typedef void (__attribute__((thiscall)) *ListCtorFn)(void *list);
typedef int (__attribute__((thiscall)) *ListAddFn)(void *list, const char *text);
typedef void (__attribute__((thiscall)) *DropDownCtorFn)(void *win, void *parent, void *list, int rows);
typedef void (__attribute__((thiscall)) *DropDownSelectFn)(void *win, int id, int flags);
typedef void (__attribute__((thiscall)) *SetTextFn)(void *win, const char *text);
typedef void (__attribute__((thiscall)) *SetWidthFn)(void *win, int width);

/* Options dialog constructor, where the slider was made (0x59142d): fill the
   drop-down list once (modes don't change during a run), set the dialog's
   count / original / selected index (+0x170, +0x174, +0x178) to the
   pending resolution, and create the drop-down (left in zh_res_combo). */
void __attribute__((stdcall)) res_menu_open(BYTE *dialog) __asm__("zh_res_menu_open");
void __attribute__((stdcall)) res_menu_open(BYTE *dialog)
{
    DWORD pw = GetPrivateProfileIntA("ZaxHiRes", "Screen Width", start_w, cfg_path);
    DWORD ph = GetPrivateProfileIntA("ZaxHiRes", "Screen Height", start_h, cfg_path);
    char text[32];
    int i, j, cur = 0;
    Mode t;

    if (!nmenu) {
        for (i = 0; i < nmodes; i++)
            menu[nmenu++] = modes[i];
        for (i = 1; i < nmenu; i++)
            for (j = i; j > 0 && (menu[j].w < menu[j - 1].w ||
                                  (menu[j].w == menu[j - 1].w && menu[j].h < menu[j - 1].h)); j--) {
                t = menu[j]; menu[j] = menu[j - 1]; menu[j - 1] = t;
            }
        ((ListCtorFn)0x592200)(menu_list);
        menu_list[0] = 0x5f0240;        /* as 0x590cbf builds the Colors list */
        for (i = 0; i < nmenu; i++) {
            wsprintfA(text, "%lu X %lu", menu[i].w, menu[i].h);
            ((ListAddFn)0x54f470)(menu_list, text);
        }
        logmsg("Options menu: %d resolutions in the drop-down", nmenu);
    }
    for (i = 0; i < nmenu; i++)                 /* pending, else the current one */
        if (menu[i].w == start_w && menu[i].h == start_h)
            cur = i;
    for (i = 0; i < nmenu; i++)
        if (menu[i].w == pw && menu[i].h == ph)
            cur = i;
    *(DWORD *)(dialog + 0x170) = nmenu;
    *(DWORD *)(dialog + 0x174) = cur;
    *(DWORD *)(dialog + 0x178) = cur;
    res_combo = ((AllocFn)0x417710)(0x118);
    if (res_combo)
        ((DropDownCtorFn)0x408cd0)(res_combo, dialog, menu_list, 0xf423f);
}

/* Replaces 0x59142d: drop-down instead of the slider, then back into the
   game's code that lays the control out (0x591459). */
__attribute__((naked)) static void res_menu_open_hook(void)
{
    __asm__ volatile(
        "pushal\n\t"
        "push %esi\n\t"
        "call zh_res_menu_open\n\t"
        "popal\n\t"
        "pop %ebp\n\t"                    /* as 0x591443 */
        "pop %ebx\n\t"
        "mov zh_res_combo, %eax\n\t"
        "push $0x591459\n\t"
        "ret\n\t");
}

/* Replaces 0x5914f5 (slider range / value / label): select the entry and
   clear the label beside it (it showed the slider's "W X H"). */
void __attribute__((stdcall)) res_menu_select(BYTE *dialog) __asm__("zh_res_menu_select");
void __attribute__((stdcall)) res_menu_select(BYTE *dialog)
{
    void *combo = *(void **)(dialog + 0x14c), *label = *(void **)(dialog + 0x150);
    if (combo)
        ((DropDownSelectFn)0x408bc0)(combo, *(DWORD *)(dialog + 0x178), 0);
    if (label)
        ((SetTextFn)(*(DWORD **)label)[0xac / 4])(label, "");
}

__attribute__((naked)) static void res_menu_select_hook(void)
{
    __asm__ volatile(
        "pushal\n\t"
        "push %esi\n\t"
        "call zh_res_menu_select\n\t"
        "popal\n\t"
        "push $0x591530\n\t"
        "ret\n\t");
}

/*
 * Drop-down popups (ctor 0x407a50, made on every click, up to 14 rows) are
 * list boxes sized to the longest entry (0x404628); 0x407660 then adds the
 * scroll bar (+0x248) when there are more entries than rows and takes its
 * width out of the list's (0x4074f0), cutting the entries short. The game's
 * own drop-downs never have that many entries. Called instead of 0x407660
 * at 0x407af8: give the width back. Window rect: +0x04 left .. +0x0c right.
 */
typedef void (__attribute__((thiscall)) *PopupFitFn)(void *popup);
static void __attribute__((thiscall)) popup_fit(BYTE *popup)
{
    BYTE *bar;
    ((PopupFitFn)0x407660)(popup);
    bar = *(BYTE **)(popup + 0x248);
    if (bar)
        ((SetWidthFn)0x40f5b0)(popup, *(int *)(popup + 0x0c) - *(int *)(popup + 0x04) +
                                      *(int *)(bar + 0x0c) - *(int *)(bar + 0x04));
}

/* Replaces 0x5917d3 (resolution slider moved: set the mode now). */
__attribute__((naked)) static void res_chosen_hook(void)
{
    __asm__ volatile(
        "pushal\n\t"
        "push %ebp\n\t"
        "call zh_res_chosen\n\t"
        "popal\n\t"
        "mov $0x5918b1, %eax\n\t"         /* skip mode set and CFG write */
        "jmp *%eax\n\t");
}

/* Does the display report this mode (any colour depth)? */
static int display_has_mode(DWORD w, DWORD h)
{
    DEVMODEA dm;
    DWORD i;
    for (i = 0; ; i++) {
        dm.dmSize = sizeof dm;
        dm.dmDriverExtra = 0;
        if (!EnumDisplaySettingsA(NULL, i, &dm))
            return 0;
        if (dm.dmPelsWidth == w && dm.dmPelsHeight == h)
            return 1;
    }
}

/* A resolution chosen in the Options menu, if the display still has it. */
static int menu_resolution(DWORD *w, DWORD *h)
{
    DWORD mw = GetPrivateProfileIntA("ZaxHiRes", "Screen Width", 0, cfg_path);
    DWORD mh = GetPrivateProfileIntA("ZaxHiRes", "Screen Height", 0, cfg_path);
    if (!mw || !mh)
        return 0;
    if (!display_has_mode(mw, mh)) {
        logmsg("resolution %lux%lu chosen in the Options menu isn't reported by the display, ignored", mw, mh);
        return 0;
    }
    *w = mw;
    *h = mh;
    return 1;
}

static void add_mov_eax_for_call(DWORD va, DWORD callee, DWORD value, const char *what)
{
    BYTE o[5] = { 0xe8 }, r[5] = { 0xb8 };
    DWORD rel = callee - (va + 5);
    copy(o + 1, &rel, 4);
    copy(r + 1, &value, 4);
    add(va, o, r, 5, what);
}

static void setup_mode_menu(DWORD w, DWORD h, int from_menu)
{
    start_w = w;
    start_h = h;
    add_jmp(0x567d60, (const BYTE[]){ 0x83, 0xec, 0x08, 0x8b, 0x4c, 0x24, 0x0c }, 7,
            mode_cb, "display modes: every reported mode, for the Options menu");
    add_jmp(0x5917d3, (const BYTE[]){ 0x8b, 0x0d, 0xcc, 0x02, 0x6c, 0x00 }, 6,
            res_chosen_hook, "Options menu: resolution applies at the next start");
    add_jmp(0x59142d, (const BYTE[]){ 0x8b, 0x86, 0x74, 0x01, 0x00, 0x00 }, 6,
            res_menu_open_hook, "Options menu: resolution drop-down instead of the slider");
    add_jmp(0x5914f5, (const BYTE[]){ 0x8b, 0x8e, 0x70, 0x01, 0x00, 0x00 }, 6,
            res_menu_select_hook, "Options menu: drop-down shows the pending resolution");
    {
        BYTE o[5] = { 0xe8 };
        DWORD rel = 0x407660 - (0x407af8 + 5);
        copy(o + 1, &rel, 4);
        add_call(0x407af8, o, popup_fit, "drop-down popups: room for the scroll bar");
    }
    add(0x59197c, (const BYTE[]){ 0xff, 0x92, 0x34, 0x01, 0x00, 0x00 },
        (const BYTE[]){ 0xff, 0x92, 0x30, 0x01, 0x00, 0x00 }, 6,
        "Options menu: read the drop-down's selection (slider value before)");
    {   /* 0x591988: skip the "W X H" label update (read the 10-entry list) */
        BYTE r[6] = { 0xe9, 0, 0, 0, 0, 0x90 };
        DWORD rel = 0x5919e2 - (0x591988 + 5);
        copy(r + 1, &rel, 4);
        add(0x591988, (const BYTE[]){ 0x8b, 0x0d, 0xcc, 0x02, 0x6c, 0x00 }, r, 6,
            "Options menu: no slider label update");
    }
    if (from_menu) {
        add_mov_eax_for_call(0x590d73, 0x4d2220, w, "start-up width: Options menu choice over /ScreenWidth");
        add_mov_eax_for_call(0x590d81, 0x4d2220, h, "start-up height: Options menu choice over /ScreenHeight");
    }
}

/* Same parsing as the game (0x4d2220): find the switch, skip blanks, atoi. */
static int cmdline_int(const char *cmdline, const char *name)
{
    const char *p;
    int len = lstrlenA(name), v = 0;

    for (p = cmdline; *p; p++)
        if (same(p, name, len))
            break;
    if (!*p)
        return 0;
    p += len;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p == '"')
        p++;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0');
    return v;
}

/* Returns 1 if the resolution was chosen in the Options menu. */
static int read_resolution(DWORD *w, DWORD *h)
{
    const char *cmdline = GetCommandLineA();

    logmsg("command line: %s", cmdline);
    wsprintfA(cfg_path, "%sZax.CFG", exedir);
    if (menu_resolution(w, h)) {
        logmsg("using the resolution chosen in the Options menu (overrides /ScreenWidth /ScreenHeight)");
        return 1;
    }
    *w = cmdline_int(cmdline, "/ScreenWidth");
    *h = cmdline_int(cmdline, "/ScreenHeight");
    if (!*w)
        *w = GetPrivateProfileIntA("Options", "Screen Width", 640, cfg_path);
    if (!*h)
        *h = GetPrivateProfileIntA("Options", "Screen Height", 480, cfg_path);
    return 0;
}

static int cmdline_has(const char *cmdline, const char *name)
{
    int len = lstrlenA(name);
    for (; *cmdline; cmdline++)
        if (same(cmdline, name, len))
            return 1;
    return 0;
}

/*
 * /CamProbe: research aid for the camera. Every PROBE_EVERY calls of the
 * per-frame sound listener update (0x4f2620) it writes one line to
 * ZaxCamProbe.log with the camera-related state (floats as value*100):
 *   view state: camera top-left +0xc0/+0xc4, target centre +0xc8/+0xcc,
 *               smoothed centre +0xd0/+0xd4, zoom +0xd8
 *   game:       play rect +0x2e4..+0x2f0, map size [+0x2e0]+0x10/+0x14
 *   entity:     player position +0x80/+0x84
 * plus the first dwords of the world surface object [game+0x2a4], whose
 * width/height fields aren't known yet.
 */
#define PROBE_EVERY 30
#define PROBE_SURF  0x80

static HANDLE probefile = INVALID_HANDLE_VALUE;
DWORD probe_resume __asm__("zh_probe_resume") = 0x4f26ee;

static int f100(DWORD bits)
{
    float f;
    copy(&f, &bits, 4);
    return (int)(f * 100.0f);
}

static const DWORD *readable(DWORD p, int bytes)
{
    return p && !IsBadReadPtr((void *)p, bytes) ? (const DWORD *)p : NULL;
}

void probe_sample(const DWORD *view, const DWORD *game) __asm__("zh_probe_sample");
void probe_sample(const DWORD *view, const DWORD *game)
{
    static char buf[PROBE_SURF / 4 * 9 + 512];
    static DWORD calls;
    const DWORD *ent = NULL, *map, *surf;
    DWORD idx = view[0xdc / 4], count = game[0x2c0 / 4];
    int n, i;

    if (calls++ % PROBE_EVERY)
        return;
    if (idx < count)
        ent = readable(((const DWORD *)game[0x2bc / 4])[idx], 0x88);
    map = readable(game[0x2e0 / 4], 0x18);
    surf = readable(game[0x2a4 / 4], PROBE_SURF);

    n = wsprintfA(buf, "S %lu t %lu zoom %d cam %d %d tgt %d %d smooth %d %d player %d %d "
                  "rect %ld %ld %ld %ld map %ld %ld |surf",
                  calls - 1, GetTickCount(), f100(view[0xd8 / 4]),
                  f100(view[0xc0 / 4]), f100(view[0xc4 / 4]),
                  f100(view[0xc8 / 4]), f100(view[0xcc / 4]),
                  f100(view[0xd0 / 4]), f100(view[0xd4 / 4]),
                  ent ? f100(ent[0x80 / 4]) : 0, ent ? f100(ent[0x84 / 4]) : 0,
                  game[0x2e4 / 4], game[0x2e8 / 4], game[0x2ec / 4], game[0x2f0 / 4],
                  map ? map[0x10 / 4] : 0, map ? map[0x14 / 4] : 0);
    for (i = 0; i < PROBE_SURF / 4; i++)
        n += wsprintfA(buf + n, " %lx", surf ? surf[i] : 0);
    buf[n++] = '\r';
    buf[n++] = '\n';
    WriteFile(probefile, buf, n, (DWORD *)&n, NULL);
}

/*
 * Zoom persistence. SetZoom (0x4f6770) clamps the zoom so the view never
 * shows past the map edge; at high resolutions that triggers on small maps
 * (mothership: 0.46 at 1920x1080). Level start (0x4f2551) and player spawn
 * (0x4f4810) then re-apply the current, already clamped zoom, so it sticks.
 *
 * The SetZoom entry hook remembers the zoom the player asked for (calls from
 * the zoom keys, reset and detent). on_frame() re-applies it whenever the
 * view's level or map size changes, letting SetZoom clamp it for that map.
 */
typedef void (__attribute__((thiscall)) *SetZoomFn)(void *game, float zoom, int view);
#define SetZoom ((SetZoomFn)0x4f6770)

static const DWORD user_zoom_returns[] = {
    0x4f328c, 0x4f32eb,             /* zoom in / out keys */
    0x4f3305,                       /* zoom reset (1.0) */
    0x544a7e, 0x544ac1, 0x544b01,   /* script/console zoom */
};

/* The zoom detent (0x4f3312) calls SetZoom every frame while the zoom isn't
   1.0, mostly with the unchanged current (possibly map-clamped) zoom; only
   its final snap to 1.0 is a real change. */
#define DETENT_RETURN 0x4f3400
#define FLOAT_ONE     0x3f800000
static float wanted_zoom = 1.0f;
static int zoomfix;
DWORD zoom_resume __asm__("zh_zoom_resume") = 0x4f6776;

void note_zoom(DWORD ret, DWORD zoom_bits) __asm__("zh_note_zoom");
void note_zoom(DWORD ret, DWORD zoom_bits)
{
    int i;
    if (ret == DETENT_RETURN && zoom_bits == FLOAT_ONE)
        wanted_zoom = 1.0f;
    for (i = 0; i < (int)(sizeof user_zoom_returns / sizeof user_zoom_returns[0]); i++)
        if (ret == user_zoom_returns[i])
            copy(&wanted_zoom, &zoom_bits, 4);
}

/* Replaces SetZoom's prologue 0x4f6770 (push ebp; mov ebp,esp; sub esp,0x10). */
__attribute__((naked)) static void zoom_hook(void)
{
    __asm__ volatile(
        "pushl %ecx\n\t"                /* this (game) */
        "pushl 8(%esp)\n\t"             /* zoom argument */
        "pushl 8(%esp)\n\t"             /* return address */
        "call zh_note_zoom\n\t"
        "addl $8, %esp\n\t"
        "popl %ecx\n\t"
        "pushl %ebp\n\t"                /* original prologue */
        "movl %esp, %ebp\n\t"
        "subl $0x10, %esp\n\t"
        "jmp *zh_zoom_resume\n\t");
}

/*
 * Also checks every frame that the world surface [game+0x2a4] (width in the
 * high word of +0x10, height at +0x14) matches play rect * zoom. Loading a
 * save on a small map leaves it map-fitted (896x477 at 1920x1080) with zoom
 * 1.0; SetZoom then never fixes it because the zoom doesn't change, and the
 * camera centres on the small surface: player top-left, view runs on past.
 */
static void zoom_check(DWORD *view, DWORD *game)
{
    static DWORD last_level, last_w, last_h;
    static int forced;
    const DWORD *ent = NULL, *level, *surf;
    DWORD idx = view[0xdc / 4], count = game[0x2c0 / 4];
    int changed, mismatch = 0;
    float zoom;

    if (idx < count)
        ent = readable(((const DWORD *)game[0x2bc / 4])[idx], 0x80);
    level = ent ? readable(ent[0x7c / 4], 0x2d04) : NULL;
    surf = readable(game[0x2a4 / 4], 0x18);
    if (!level || !surf)
        return;

    changed = (DWORD)level != last_level || level[0x2cfc / 4] != last_w || level[0x2d00 / 4] != last_h;
    if (changed) {
        last_level = (DWORD)level;
        last_w = level[0x2cfc / 4];
        last_h = level[0x2d00 / 4];
        forced = 0;
    } else {
        int sw = surf[0x10 / 4] >> 16, sh = surf[0x14 / 4];
        int ew, eh;
        copy(&zoom, &view[0xd8 / 4], 4);
        ew = (int)((int)(game[0x2ec / 4] - game[0x2e4 / 4]) * zoom);
        eh = (int)((int)(game[0x2f0 / 4] - game[0x2e8 / 4]) * zoom);
        mismatch = sw - ew > 2 || ew - sw > 2 || sh - eh > 2 || eh - sh > 2;
        if (!mismatch || forced)
            return;
        forced = 1;
        logmsg("world surface %dx%d but play rect * zoom is %dx%d", sw, sh, ew, eh);
    }
    logmsg("level %08lx map %lux%lu: applying zoom %d/100%s", last_level, last_w, last_h,
           (int)(wanted_zoom * 100.0f), changed ? " (level change)" : " (surface mismatch)");
    view[0xd8 / 4] = 0;     /* make SetZoom see a change, so it always resizes */
    SetZoom(game, wanted_zoom, 0);
}

/* /LogFps: frames per second (calls of the per-frame listener update),
   written to ZaxHiRes.log once a second while a level is running. */
static int logfps;
static DWORD comp_tiles;     /* terrain tiles composited (when hooked), for /LogFps */

/*
 * /LogSleep: research aid. Hooks the game's Sleep import (IAT 0x5ea07c) and,
 * for the main thread (the one running the per-frame hook), sums calls,
 * requested and actual milliseconds per call site; logged with the fps line,
 * together with the window active / minimised flags of the message loop at
 * 0x5998f0 (0x713e10 / 0x713e11; it sleeps 1 or 100 ms when not active).
 */
#define SLEEP_SITES 16
typedef VOID (WINAPI *SleepFn)(DWORD);
static SleepFn real_sleep;
static DWORD main_tid;
static struct { DWORD site, calls, asked, slept_us; } sleep_sites[SLEEP_SITES];
static DWORD qpf_per_us;    /* performance counter ticks per microsecond */

static VOID WINAPI sleep_hook(DWORD ms)
{
    DWORD site = (DWORD)__builtin_return_address(0);
    LARGE_INTEGER t0, t1;
    int i;

    if (GetCurrentThreadId() != main_tid) {
        real_sleep(ms);
        return;
    }
    QueryPerformanceCounter(&t0);
    real_sleep(ms);
    QueryPerformanceCounter(&t1);
    for (i = 0; i < SLEEP_SITES && sleep_sites[i].site && sleep_sites[i].site != site; i++)
        ;
    if (i == SLEEP_SITES)
        return;
    sleep_sites[i].site = site;
    sleep_sites[i].calls++;
    sleep_sites[i].asked += ms;
    sleep_sites[i].slept_us += (t1.LowPart - t0.LowPart) / qpf_per_us;
}

static void sleep_report(void)
{
    int i;
    for (i = 0; i < SLEEP_SITES && sleep_sites[i].site; i++) {
        if (sleep_sites[i].calls)
            logmsg("  Sleep from 0x%06lx: %lu calls, asked %lu ms, slept %lu ms", sleep_sites[i].site,
                   sleep_sites[i].calls, sleep_sites[i].asked, sleep_sites[i].slept_us / 1000);
        sleep_sites[i].calls = sleep_sites[i].asked = sleep_sites[i].slept_us = 0;
    }
    logmsg("  window active %d, minimised %d", *(const BYTE *)0x713e10, *(const BYTE *)0x713e11);
}

static void setup_sleep_log(void)
{
    DWORD *iat = (DWORD *)0x5ea07c, old;
    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);
    qpf_per_us = qpf.HighPart ? 0 : qpf.LowPart / 1000000;
    if (!qpf_per_us) {
        logmsg("Sleep logging: unexpected counter frequency, disabled");
        return;
    }
    real_sleep = (SleepFn)*iat;
    VirtualProtect(iat, 4, PAGE_READWRITE, &old);
    *iat = (DWORD)sleep_hook;
    VirtualProtect(iat, 4, old, &old);
    logmsg("Sleep logging: IAT 0x5ea07c hooked (was %08lx)", (DWORD)real_sleep);
}

/*
 * /LogHud: research aid. Wraps the generic blit 0x4b3280 (thiscall dst,
 * args src, x, y, flags; src rect from src vtable +0x90; ret 0x10) and logs
 * each distinct (call site, x, y, size) of blits that land in the bottom 300
 * rows of the screen, to map out the HUD bar.
 */
typedef void (__attribute__((thiscall)) *BlitFn)(void *dst, void *src, int x, int y, int flags);
typedef void (__attribute__((thiscall)) *GetRectFn)(void *surf, RECT *r);
#define HUD_SEEN 2000
static BlitFn blit_orig;
static int hud_screen_h;
static DWORD hud_seen[HUD_SEEN][5];
static int hud_nseen;

static void __attribute__((thiscall)) blit_hook(void *dst, void *src, int x, int y, int flags)
{
    DWORD site = (DWORD)__builtin_return_address(0);
    RECT r;
    int i, w, h;

    ((GetRectFn)(*(DWORD **)src)[0x90 / 4])(src, &r);
    w = r.right - r.left;
    h = r.bottom - r.top;
    /* 0x4a6693 blits single text glyphs: too many, and text has its own
       positioning code */
    if (site != 0x4a6693 && y + h > hud_screen_h - 300 && hud_nseen < HUD_SEEN) {
        for (i = 0; i < hud_nseen; i++)
            if (hud_seen[i][0] == site && hud_seen[i][1] == (DWORD)x && hud_seen[i][2] == (DWORD)y &&
                hud_seen[i][3] == (DWORD)w && hud_seen[i][4] == (DWORD)h)
                break;
        if (i == hud_nseen) {
            hud_seen[i][0] = site;
            hud_seen[i][1] = x;
            hud_seen[i][2] = y;
            hud_seen[i][3] = w;
            hud_seen[i][4] = h;
            hud_nseen++;
            logmsg("hud blit from 0x%06lx: dst %08lx src %08lx (vtable %06lx) at %d,%d size %dx%d flags %x",
                   site, (DWORD)dst, (DWORD)src, **(DWORD **)&src, x, y, w, h, flags);
        }
    }
    blit_orig(dst, src, x, y, flags);
}

/* Surface Blt (vtable 0x5f4814 + 0x5c = 0x4a8f40): thiscall dst, args src,
   rect a, rect b, flags. Every blit ends here; logged by call site and both
   rects when one of them reaches into the bottom 300 rows. */
typedef void (__attribute__((thiscall)) *BltFn)(void *dst, void *src, RECT *a, RECT *b, DWORD flags);
#define BLT_SEEN 2000
static DWORD blt_seen[BLT_SEEN][5];
static int blt_nseen;

static void __attribute__((thiscall)) blt_hook(void *dst, void *src, RECT *a, RECT *b, DWORD flags)
{
    DWORD site = (DWORD)__builtin_return_address(0);
    int i;

    if (site != 0x4b32e7 && a && b && blt_nseen < BLT_SEEN &&
        (a->bottom > hud_screen_h - 300 || b->bottom > hud_screen_h - 300)) {
        for (i = 0; i < blt_nseen; i++)
            if (blt_seen[i][0] == site && blt_seen[i][1] == (DWORD)a->left && blt_seen[i][2] == (DWORD)a->top &&
                blt_seen[i][3] == (DWORD)b->left && blt_seen[i][4] == (DWORD)b->top)
                break;
        if (i == blt_nseen) {
            blt_seen[i][0] = site;
            blt_seen[i][1] = a->left;
            blt_seen[i][2] = a->top;
            blt_seen[i][3] = b->left;
            blt_seen[i][4] = b->top;
            blt_nseen++;
            logmsg("blt from 0x%06lx: dst %08lx src %08lx rect a %ld,%ld-%ld,%ld rect b %ld,%ld-%ld,%ld flags %lx",
                   site, (DWORD)dst, (DWORD)src, a->left, a->top, a->right, a->bottom,
                   b->left, b->top, b->right, b->bottom, flags);
        }
    }
    ((BltFn)0x4a8f40)(dst, src, a, b, flags);
}

static void setup_hud_log(DWORD screen_h)
{
    static const BYTE prologue[5] = { 0x83, 0xec, 0x20, 0x53, 0x55 };  /* sub esp,0x20; push ebx; push ebp */
    BYTE *tramp = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    DWORD rel;

    if (!tramp)
        return;
    copy(tramp, prologue, 5);
    tramp[5] = 0xe9;
    rel = 0x4b3285 - ((DWORD)tramp + 10);
    copy(tramp + 6, &rel, 4);
    blit_orig = (BlitFn)tramp;
    hud_screen_h = screen_h;
    add_jmp(0x4b3280, prologue, 5, (void *)blit_hook, "HUD log: generic blit 0x4b3280");
    {
        DWORD *slot = (DWORD *)(0x5f4814 + 0x5c), old;
        if (*slot == 0x4a8f40) {
            VirtualProtect(slot, 4, PAGE_READWRITE, &old);
            *slot = (DWORD)blt_hook;
            VirtualProtect(slot, 4, old, &old);
            logmsg("HUD log: surface Blt vtable slot hooked");
        }
    }
}

static void fps_tick(void)
{
    static DWORD start, frames;
    DWORD now = GetTickCount();

    if (!start)
        start = now;
    frames++;
    if (now - start >= 1000) {
        logmsg("fps %lu  tiles composited/s %lu", frames * 1000 / (now - start),
               comp_tiles * 1000 / (now - start));
        if (real_sleep)
            sleep_report();
        start = now;
        frames = 0;
        comp_tiles = 0;
    }
}

/*
 * Pixel row copy 0x437890 (10% of CPU at 1920x1080): ecx = dst, edx = src,
 * [esp+4] = pixel count (16-bit pixels), ret 0x10; copied 4 bytes per loop.
 * Only called from the row loops at 0x434703 and 0x434b1f, neither of which
 * reads eax afterwards. Replaced with the CPU's fast string copy.
 */
__attribute__((naked)) static void copy16_hook(void)
{
    __asm__ volatile(
        "pushl %esi\n\t"
        "pushl %edi\n\t"
        "movl %ecx, %edi\n\t"           /* dst */
        "movl %edx, %esi\n\t"           /* src */
        "movl 12(%esp), %ecx\n\t"       /* pixel count */
        "addl %ecx, %ecx\n\t"           /* bytes */
        "movl %ecx, %edx\n\t"
        "shrl $2, %ecx\n\t"
        "cld\n\t"
        "rep movsl\n\t"
        "movl %edx, %ecx\n\t"
        "andl $3, %ecx\n\t"
        "rep movsb\n\t"
        "movl %edi, %eax\n\t"           /* end of dst, like the original */
        "popl %edi\n\t"
        "popl %esi\n\t"
        "ret $0x10\n\t");
}

/*
 * 3D sound listener: the listener update (0x4f2620) puts the listener at the
 * view's top-left world position + (320, 240), the centre of a 640x480 view
 * (0x4f2712 / 0x4f2763 add the shared constants 0x5eff98 / 0x5eff9c). The two
 * fadd instructions are pointed at these instead, updated every frame (the
 * frame hook runs earlier in the same function) to half the world view:
 * play rect * zoom / 2, so sound pans from the centre of the screen.
 */
static float listener_half_w = 320.0f, listener_half_h = 240.0f;

static void listener_update(const DWORD *view, const DWORD *game)
{
    float zoom;
    copy(&zoom, &view[0xd8 / 4], 4);
    listener_half_w = (float)(int)(game[0x2ec / 4] - game[0x2e4 / 4]) * zoom * 0.5f;
    listener_half_h = (float)(int)(game[0x2f0 / 4] - game[0x2e8 / 4]) * zoom * 0.5f;
}

void on_frame(DWORD *view, DWORD *game) __asm__("zh_on_frame");
void on_frame(DWORD *view, DWORD *game)
{
    main_tid = GetCurrentThreadId();
    if (logfps)
        fps_tick();
    if (zoomfix)
        zoom_check(view, game);
    if (zoomfix)
        listener_update(view, game);
    if (probefile != INVALID_HANDLE_VALUE)
        probe_sample(view, game);
}

/* Replaces 0x4f26e8 (mov eax,[eax+0xdc]) in the per-frame listener update
   0x4f2620; eax = view state, esi = game. */
__attribute__((naked)) static void frame_hook(void)
{
    __asm__ volatile(
        "pushal\n\t"
        "pushl %esi\n\t"
        "pushl %eax\n\t"
        "call zh_on_frame\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "movl 0xdc(%eax), %eax\n\t"     /* original instruction */
        "jmp *zh_probe_resume\n\t");
}

/*
 * Terrain compositor 0x541250 (28% of the game's CPU at 1920x1080), 16-bit
 * destination path 0x54148d..0x54160b. thiscall, this = terrain object,
 * args (frame offsets at 0x54148d): +0x40 dst surface, +0x44 8-bit layer mask
 * surface, +0x48 source x (& 0x7f), +0x4c source row phase, +0x50 width,
 * +0x54 height; +0x14 = 128-entry source row offset table (pixels).
 * Surfaces: vtable +0xb8 = row offset table (pixels), +0x7c = base address.
 * Per pixel: idx = mask byte; base texture this+0x64[idx], overlay +0x464[idx]
 * (0 = none), weight +0xc64[idx]; overlay pixels are blended over the base.
 *
 * /CompC or /CompAvx2 replace the path with comp16(); /CompVerify instead
 * lets the original run, computes the same rows into a private buffer with
 * the selected implementation and compares them afterwards.
 */
typedef const DWORD *(__attribute__((thiscall)) *SurfRowsFn)(const void *surf);
typedef BYTE *(__attribute__((thiscall)) *SurfBaseFn)(const void *surf);

static const DWORD *surf_rows(const void *surf)
{
    return ((SurfRowsFn)(*(DWORD **)surf)[0xb8 / 4])(surf);
}

static BYTE *surf_base(const void *surf)
{
    return ((SurfBaseFn)(*(DWORD **)surf)[0x7c / 4])(surf);
}

#include "comp16.h"

typedef void (*RowFn)(c16_u16 *dst, const c16_u8 *mask, c16_u32 n, c16_u32 off, const Layers *l);
static RowFn comp_row;
static int comp_verify, comp_stats;

/*
 * /CompStats: research aid. Logs the layer tables of the first terrain object
 * seen, and classifies 16-pixel groups: one layer index; different indices
 * but the same base and overlay textures (only the weight differs); mixed.
 */
static DWORD stat_calls, stat_same_idx, stat_same_tex, stat_mixed, stat_dumped;

static void comp_stats_row(const c16_u8 *mask, DWORD n, const Layers *l)
{
    DWORD x, i;
    for (x = 0; x + 16 <= n; x += 16) {
        DWORD b = l->base[mask[x]], o = l->over[mask[x]];
        int same_idx = 1, same_tex = 1;
        for (i = 1; i < 16; i++) {
            DWORD k = mask[x + i];
            same_idx &= k == mask[x];
            same_tex &= l->base[k] == b && l->over[k] == o;
        }
        if (same_idx)
            stat_same_idx++;
        else if (same_tex)
            stat_same_tex++;
        else
            stat_mixed++;
    }
}

static void comp_stats_call(const DWORD *self, const Layers *l)
{
    DWORD i;
    if (!stat_dumped) {
        stat_dumped = 1;
        logmsg("compositor tables of terrain object %08lx (idx: base overlay weight):", (DWORD)self);
        for (i = 0; i < 256; i++)
            if (l->base[i] || l->over[i] || l->weight[i])
                logmsg("  %3lu: %08lx %08lx %lu", i, (DWORD)l->base[i], (DWORD)l->over[i], (DWORD)l->weight[i]);
    }
    if (++stat_calls % 2000 == 0)
        logmsg("compositor groups of 16: %lu same index, %lu same textures, %lu mixed (%lu calls)",
               stat_same_idx, stat_same_tex, stat_mixed, stat_calls);
}

/* One call of the 16-bit path. out == NULL writes to the real destination,
   otherwise rows go to out (width * height, packed). */
static void comp16_run(const DWORD *self, const DWORD *frame, WORD *out)
{
    const void *dst = (const void *)frame[0x40 / 4], *msk = (const void *)frame[0x44 / 4];
    const DWORD *srcrows = (const DWORD *)frame[0x14 / 4];
    DWORD sx = frame[0x48 / 4], phase = frame[0x4c / 4], w = frame[0x50 / 4], h = frame[0x54 / 4];
    const DWORD *drows = surf_rows(dst), *mrows = surf_rows(msk);
    BYTE *dbase = surf_base(dst), *mbase = surf_base(msk);
    static C16Prep prep;
    Layers l = { (const c16_u32 *)self + 0x64 / 4, (const c16_u32 *)self + 0x464 / 4,
                 (const c16_u32 *)self + 0xc64 / 4, NULL };
    DWORD y;

    if (comp_row == row_avx2) {
        c16_prepare(l.base, l.over, l.weight, &prep);
        l.prep = &prep;
    }

    if (comp_stats)
        comp_stats_call(self, &l);
    for (y = 0; y < h; y++) {
        DWORD off = (srcrows[(y + phase) & 0x7f] + sx) * 2;
        WORD *row = out ? out + y * w : (WORD *)(dbase + drows[y] * 2);
        if (comp_stats)
            comp_stats_row(mbase + mrows[y], w, &l);
        comp_row(row, mbase + mrows[y], w, off, &l);
    }
}

void comp16(const DWORD *self, const DWORD *frame) __asm__("zh_comp16");
void comp16(const DWORD *self, const DWORD *frame)
{
    comp_tiles++;
    comp16_run(self, frame, NULL);
}

/* Verify mode: state of the call in progress, between begin and end. */
#define VERIFY_MAX (1 << 20)            /* pixels */
static WORD *verify_buf;
static const DWORD *verify_frame;
static DWORD verify_calls, verify_skipped, verify_pixels, verify_bad;

void comp_verify_begin(const DWORD *self, const DWORD *frame) __asm__("zh_comp_verify_begin");
void comp_verify_begin(const DWORD *self, const DWORD *frame)
{
    verify_frame = NULL;
    if (frame[0x50 / 4] * frame[0x54 / 4] > VERIFY_MAX) {
        verify_skipped++;
        return;
    }
    comp_tiles++;
    comp16_run(self, frame, verify_buf);
    verify_frame = frame;
}

void comp_verify_end(void) __asm__("zh_comp_verify_end");
void comp_verify_end(void)
{
    const DWORD *frame = verify_frame;
    const void *dst;
    const DWORD *drows;
    BYTE *dbase;
    DWORD w, h, x, y;

    if (!frame)
        return;
    verify_frame = NULL;
    dst = (const void *)frame[0x40 / 4];
    w = frame[0x50 / 4];
    h = frame[0x54 / 4];
    drows = surf_rows(dst);
    dbase = surf_base(dst);
    for (y = 0; y < h; y++) {
        const WORD *got = (const WORD *)(dbase + drows[y] * 2), *mine = verify_buf + y * w;
        for (x = 0; x < w; x++) {
            if (got[x] != mine[x] && verify_bad++ < 20)
                logmsg("compositor MISMATCH call %lu at %lu,%lu: game %04x, ours %04x",
                       verify_calls, x, y, (DWORD)got[x], (DWORD)mine[x]);
        }
        verify_pixels += w;
    }
    if (++verify_calls % 2000 == 0)
        logmsg("compositor verify: %lu calls, %lu pixels, %lu mismatches, %lu skipped",
               verify_calls, verify_pixels, verify_bad, verify_skipped);
}

DWORD comp_epilogue __asm__("zh_comp_epilogue") = 0x54160b;
DWORD comp_resume __asm__("zh_comp_resume") = 0x541495;

/* Replaces 0x54148d: run comp16() instead of the original 16-bit path. */
__attribute__((naked)) static void comp_hook(void)
{
    __asm__ volatile(
        "pushal\n\t"
        "leal 32(%esp), %eax\n\t"       /* frame as at 0x54148d */
        "pushl %eax\n\t"
        "pushl %esi\n\t"                /* this */
        "call zh_comp16\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "jmp *zh_comp_epilogue\n\t");
}

/* Replaces 0x54148d in verify mode: compute into the buffer, then run the
   original path (re-executing the 8 overwritten bytes). */
__attribute__((naked)) static void comp_verify_hook(void)
{
    __asm__ volatile(
        "pushal\n\t"
        "leal 32(%esp), %eax\n\t"
        "pushl %eax\n\t"
        "pushl %esi\n\t"
        "call zh_comp_verify_begin\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "movl 0x54(%esp), %eax\n\t"     /* original: mov eax,[esp+0x54] */
        "movl %edi, 0x10(%esp)\n\t"     /* original: mov [esp+0x10],edi */
        "jmp *zh_comp_resume\n\t");
}

/* Replaces 0x54160b (16-bit path exit) in verify mode: compare, then the
   original epilogue. */
__attribute__((naked)) static void comp_verify_exit_hook(void)
{
    __asm__ volatile(
        "pushal\n\t"
        "call zh_comp_verify_end\n\t"
        "popal\n\t"
        "popl %edi\n\t"
        "popl %esi\n\t"
        "popl %ebp\n\t"
        "popl %ebx\n\t"
        "addl $0x2c, %esp\n\t"
        "ret $0x18\n\t");
}

static int cpu_has_avx2(void)
{
    unsigned a, b, c, d, lo, hi;
    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 27)))    /* OSXSAVE */
        return 0;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6) != 6)                                           /* XMM + YMM state */
        return 0;
    return __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & (1u << 5));
}

static void setup_compositor(const char *cmdline)
{
    int want_c = cmdline_has(cmdline, "/CompC"), want_avx2 = cmdline_has(cmdline, "/CompAvx2");

    comp_verify = cmdline_has(cmdline, "/CompVerify");
    comp_stats = cmdline_has(cmdline, "/CompStats");
    if (!want_c && !want_avx2 && !comp_verify)
        return;
    if (want_c || !cpu_has_avx2()) {
        comp_row = row_c;
        logmsg("compositor: scalar C%s", want_c ? "" : " (no AVX2)");
    } else {
        comp_row = row_avx2;
        logmsg("compositor: AVX2");
    }
    if (comp_verify) {
        verify_buf = VirtualAlloc(NULL, VERIFY_MAX * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!verify_buf) {
            logmsg("compositor verify: no buffer, disabled");
            return;
        }
        add_jmp(0x54148d, (const BYTE[]){ 0x8b, 0x44, 0x24, 0x54, 0x89, 0x7c, 0x24, 0x10 }, 8,
                comp_verify_hook, "compositor: verify (start)");
        add_jmp(0x54160b, (const BYTE[]){ 0x5f, 0x5e, 0x5d, 0x5b, 0x83, 0xc4, 0x2c }, 7,
                comp_verify_exit_hook, "compositor: verify (exit)");
    } else {
        add_jmp(0x54148d, (const BYTE[]){ 0x8b, 0x44, 0x24, 0x54, 0x89, 0x7c, 0x24, 0x10 }, 8,
                comp_hook, "compositor: replaced");
    }
}

static void apply(void)
{
    const char *cmdline = GetCommandLineA();
    DWORD w, h, old;
    int i, bad = 0, native, probe, from_menu;

    from_menu = read_resolution(&w, &h);
    logmsg("requested resolution: %lux%lu", w, h);
    native = (w == 640 && h == 480) || (w == 800 && h == 600);
    probe = cmdline_has(cmdline, "/CamProbe");
    logfps = cmdline_has(cmdline, "/LogFps") || cmdline_has(cmdline, "/LogSleep");
    if (!native && (w < 800 || h < 600)) {
        logmsg("resolutions below 800x600 are not supported, nothing patched");
        return;
    }
    setup_mode_menu(w, h, from_menu);

    if (!native) {
        center_dx = (w - 800) / 2;
        center_dy = (h - 600) / 2;
        logmsg("menu centring offset: %d,%d", center_dx, center_dy);
        build_patches(w, h);

        /* Zoom: max (zoom-out) limit was 960/W capped at 1.0, i.e. 0.5 at
           1920 wide; always use 1.0. Remember and re-apply the chosen zoom. */
        add_nops(0x4f253d, (const BYTE[]){ 0x75, 0x0a }, 2, "zoom limit: max always 1.0");
        {
            BYTE rw[6] = { 0xd8, 0x05 }, rh[6] = { 0xd8, 0x05 };
            DWORD aw = (DWORD)&listener_half_w, ah = (DWORD)&listener_half_h;
            copy(rw + 2, &aw, 4);
            copy(rh + 2, &ah, 4);
            add(0x4f2712, (const BYTE[]){ 0xd8, 0x05, 0x98, 0xff, 0x5e, 0x00 }, rw, 6,
                "sound listener: x at view centre");
            add(0x4f2763, (const BYTE[]){ 0xd8, 0x05, 0x9c, 0xff, 0x5e, 0x00 }, rh, 6,
                "sound listener: y at view centre");
        }
        add_jmp(0x4f6770, (const BYTE[]){ 0x55, 0x8b, 0xec, 0x83, 0xec, 0x10 }, 6,
                zoom_hook, "SetZoom: remember the player's zoom");
        zoomfix = 1;

        /* Performance experiments, opt-in: /FastCopy (no measurable gain,
           the copy is memory-bound), /CompAvx2 /CompC /CompVerify (terrain
           compositor; rarely runs once the tile cache is big enough). */
        if (cmdline_has(cmdline, "/FastCopy"))
            add_jmp(0x437890, (const BYTE[]){ 0x8b, 0xc1, 0x56, 0x8b, 0x4c, 0x24, 0x08 }, 7,
                    copy16_hook, "pixel row copy: rep movs");
        setup_compositor(cmdline);
        if (cmdline_has(cmdline, "/LogSleep"))
            setup_sleep_log();
        if (cmdline_has(cmdline, "/LogHud"))
            setup_hud_log(h);
    }
    if (probe) {
        char path[MAX_PATH];
        wsprintfA(path, "%sZaxCamProbe.log", exedir);
        probefile = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (!native || probe || logfps)
        add_jmp(0x4f26e8, (const BYTE[]){ 0x8b, 0x80, 0xdc, 0x00, 0x00, 0x00 }, 6,
                frame_hook, "per-frame hook (listener update)");
    for (i = 0; i < npatches; i++) {
        if (!same((void *)patches[i].va, patches[i].orig, patches[i].len)) {
            logmsg("MISMATCH at 0x%06lx (%s) - not the original Zax.exe?", patches[i].va, patches[i].what);
            bad = 1;
        }
    }
    if (bad) {
        logmsg("nothing patched");
        return;
    }
    for (i = 0; i < npatches; i++) {
        void *p = (void *)patches[i].va;
        VirtualProtect(p, patches[i].len, PAGE_EXECUTE_READWRITE, &old);
        copy(p, patches[i].repl, patches[i].len);
        VirtualProtect(p, patches[i].len, old, &old);
        logmsg("patched 0x%06lx  %s", patches[i].va, patches[i].what);
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    logmsg("%d patches applied", npatches);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    char path[MAX_PATH], *slash, *p;

    (void)inst;
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    GetModuleFileNameA(NULL, exedir, sizeof exedir);
    slash = NULL;
    for (p = exedir; *p; p++)
        if (*p == '\\')
            slash = p;
    if (slash)
        slash[1] = '\0';
    wsprintfA(path, "%sZaxHiRes.log", exedir);
    {
        char prev[MAX_PATH];        /* keep the previous run's log */
        wsprintfA(prev, "%sZaxHiRes.prev.log", exedir);
        MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
    }
    logfile = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    logmsg("ZaxHiRes loaded into %s", exedir);

    /* Only touch the image we were written for. */
    {
        BYTE *base = (BYTE *)GetModuleHandleA(NULL);
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        if (base != (BYTE *)0x400000 || nt->FileHeader.TimeDateStamp != ZAX_TIMESTAMP) {
            logmsg("not Zax.exe build 2001-08-02 (base 0x%08lx, timestamp 0x%08lx), nothing patched",
                   (DWORD)base, nt->FileHeader.TimeDateStamp);
            return TRUE;
        }
    }
    apply();
    return TRUE;
}
