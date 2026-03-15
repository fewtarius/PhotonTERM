/* PhotonTERM VTE - Clean-room ANSI/VT terminal emulator (implementation)
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * State machine based on Paul Flo Williams' DEC STD-070 parser description:
 *   https://vt100.net/emu/dec_ansi_parser
 * That document is a public-domain analysis of the DEC specification.
 * Clean-room ANSI/VT terminal emulator for PhotonTERM.
 */

#include "photon_vte.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── CP437 → Unicode table ─────────────────────────────────────────────────
 * Standard IBM PC Code Page 437.  Index = byte value 0x00-0xFF.
 * Entries for C0 control characters use their "ANSI art" display glyphs
 * (the characters BBS ANSI art uses when outputting them as graphics).
 * When the byte is used as a control character the state machine handles
 * it before consulting this table.
 */
static const uint32_t cp437[256] = {
    /* 0x00 */ 0x0000, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    /* 0x08 */ 0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
    /* 0x10 */ 0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,
    /* 0x18 */ 0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
    /* 0x20 */ 0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    /* 0x28 */ 0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    /* 0x30 */ 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    /* 0x38 */ 0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    /* 0x40 */ 0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    /* 0x48 */ 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    /* 0x50 */ 0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    /* 0x58 */ 0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    /* 0x60 */ 0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    /* 0x68 */ 0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    /* 0x70 */ 0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    /* 0x78 */ 0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x2302,
    /* 0x80 */ 0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    /* 0x88 */ 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    /* 0x90 */ 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    /* 0x98 */ 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    /* 0xA0 */ 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    /* 0xA8 */ 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    /* 0xB0 */ 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    /* 0xB8 */ 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    /* 0xC0 */ 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    /* 0xC8 */ 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    /* 0xD0 */ 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    /* 0xD8 */ 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    /* 0xE0 */ 0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    /* 0xE8 */ 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    /* 0xF0 */ 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    /* 0xF8 */ 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

/* ── DEC Special Graphics (line-drawing) charset ──────────────────────── */
/* Maps bytes 0x60-0x7E (when G0 = line-drawing via ESC(0) to Unicode.
 * Source: https://vt100.net/docs/vt100-ug/table3-9.html */
static const uint32_t dec_linedraw[0x1F] = {
    /* 0x60 */ 0x25C6, /* ◆ diamond */
    /* 0x61 */ 0x2592, /* ▒ checkerboard */
    /* 0x62 */ 0x2409, /* HT symbol */
    /* 0x63 */ 0x240C, /* FF symbol */
    /* 0x64 */ 0x240D, /* CR symbol */
    /* 0x65 */ 0x240A, /* LF symbol */
    /* 0x66 */ 0x00B0, /* ° degree */
    /* 0x67 */ 0x00B1, /* ± plus-minus */
    /* 0x68 */ 0x2424, /* NL symbol */
    /* 0x69 */ 0x240B, /* VT symbol */
    /* 0x6A */ 0x2518, /* ┘ lower-right corner */
    /* 0x6B */ 0x2510, /* ┐ upper-right corner */
    /* 0x6C */ 0x250C, /* ┌ upper-left corner */
    /* 0x6D */ 0x2514, /* └ lower-left corner */
    /* 0x6E */ 0x253C, /* ┼ crossing lines */
    /* 0x6F */ 0x23BA, /* ⎺ scan line 1 */
    /* 0x70 */ 0x23BB, /* ⎻ scan line 3 */
    /* 0x71 */ 0x2500, /* ─ horizontal line */
    /* 0x72 */ 0x23BC, /* ⎼ scan line 7 */
    /* 0x73 */ 0x23BD, /* ⎽ scan line 9 */
    /* 0x74 */ 0x251C, /* ├ left T */
    /* 0x75 */ 0x2524, /* ┤ right T */
    /* 0x76 */ 0x2534, /* ┴ bottom T */
    /* 0x77 */ 0x252C, /* ┬ top T */
    /* 0x78 */ 0x2502, /* │ vertical line */
    /* 0x79 */ 0x2264, /* ≤ */
    /* 0x7A */ 0x2265, /* ≥ */
    /* 0x7B */ 0x03C0, /* π */
    /* 0x7C */ 0x2260, /* ≠ */
    /* 0x7D */ 0x00A3, /* £ */
    /* 0x7E */ 0x00B7, /* · middle dot */
};

/* ── Parser state machine ──────────────────────────────────────────────── */

typedef enum {
    ST_GROUND = 0,   /* Normal character output */
    ST_ESCAPE,       /* Got ESC */
    ST_ESC_INTER,    /* ESC + intermediate byte(s) */
    ST_CSI_ENTRY,    /* Got ESC [ */
    ST_CSI_PARAM,    /* Collecting CSI parameters */
    ST_CSI_INTER,    /* CSI + intermediate byte */
    ST_CSI_IGNORE,   /* Bad CSI, discard until final */
    ST_OSC,          /* OSC string (ESC ]) - absorb and ignore */
    ST_OSC_STRING,   /* OSC string body */
    ST_DCS,          /* DCS (ESC P) - absorb and ignore */
    ST_DCS_STRING,
    ST_SOS_PM_APC,   /* SOS/PM/APC - absorb and ignore */
    ST_UTF8_1,       /* Collecting UTF-8 continuation bytes */
    ST_UTF8_2,
    ST_UTF8_3,
} parser_state_t;

#define MAX_PARAMS   16
#define MAX_INTER     2

/* ── Scrollback ────────────────────────────────────────────────────────── */

typedef struct {
    vte_cell_t *cells;  /* cols cells per line */
    int         cols;
} sb_line_t;

/* ── VTE struct ────────────────────────────────────────────────────────── */

struct vte {
    /* Grid */
    int         cols;
    int         rows;
    vte_cell_t *screen;     /* rows * cols cells, row-major */

    /* Cursor */
    int  cx;    /* 1-based column */
    int  cy;    /* 1-based row */
    bool cursor_visible;

    /* Current drawing attributes */
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;   /* VTE_ATTR_* */
    uint32_t fg_rgb;    /* truecolor fg 0x00RRGGBB (only when VTE_ATTR_FG_RGB set) */
    uint32_t bg_rgb;    /* truecolor bg 0x00RRGGBB (only when VTE_ATTR_BG_RGB set) */

    /* Saved cursor (DECSC/DECRC) */
    int     saved_cx, saved_cy;
    uint8_t saved_fg, saved_bg, saved_attr;
    uint32_t saved_fg_rgb, saved_bg_rgb;

    /* Scroll region (1-based, inclusive) */
    int margin_top;
    int margin_bot;

    /* Autowrap state */
    bool autowrap;
    bool pending_wrap;  /* next printable wraps first */

    /* LNM - Line Feed/New Line Mode (ESC[20h/20l).
     * When true, LF/VT/FF also performs CR (moves to column 1).
     * Most BBS systems expect this behaviour. */
    bool lnm;

    /* Callbacks */
    vte_callbacks_t cb;

    /* CP437 mode */
    bool cp437;

    /* G0/G1 charset designation and locking shift.
     * In BBS mode, ESC(0 designates line-drawing to G0;
     * ESC(B restores ASCII.  SO (0x0E) shifts to G1 (unused here);
     * SI (0x0F) shifts back to G0. */
    bool g0_linedraw;  /* true: G0 is DEC line-drawing; false: ASCII */
    bool using_g1;     /* true: currently shifted to G1 */

    /* OSC accumulation buffer */
    char osc_buf[256];
    int  osc_len;

    /* Parser state machine */
    parser_state_t  state;
    int             params[MAX_PARAMS];
    int             nparams;
    char            inter[MAX_INTER + 1];  /* intermediate bytes */
    int             ninter;
    uint32_t        utf8_accum;   /* partially assembled codepoint */
    int             utf8_remain;  /* continuation bytes still needed */

    /* Scrollback */
    sb_line_t *sb_lines;   /* circular buffer */
    int        sb_cap;     /* total allocated lines */
    int        sb_head;    /* next write position */
    int        sb_count;   /* lines filled */
};

/* ── Helpers ───────────────────────────────────────────────────────────── */

static inline vte_cell_t *cell_at(vte_t *v, int col, int row)
{
    /* col/row are 1-based */
    return &v->screen[(row - 1) * v->cols + (col - 1)];
}

static inline vte_cell_t blank_cell(const vte_t *v)
{
    vte_cell_t c = { .codepoint = 0x20, .fg = v->fg, .bg = v->bg, .attr = 0 };
    return c;
}

static void emit_cell(vte_t *v, int col, int row, const vte_cell_t *cell)
{
    if (v->cb.draw)
        v->cb.draw(v, col, row, cell, v->cb.user);
}

static void emit_cursor(vte_t *v)
{
    if (v->cb.cursor)
        v->cb.cursor(v, v->cx, v->cy, v->cb.user);
}

static void emit_clear(vte_t *v, int col1, int row1, int col2, int row2)
{
    if (v->cb.clear)
        v->cb.clear(v, col1, row1, col2, row2, v->cb.user);
}

/* ── Scrollback ────────────────────────────────────────────────────────── */

static void sb_push_line(vte_t *v, const vte_cell_t *row_cells)
{
    if (v->sb_cap == 0)
        return;
    sb_line_t *sl = &v->sb_lines[v->sb_head];
    /* (re)allocate if columns changed */
    if (sl->cols != v->cols) {
        free(sl->cells);
        sl->cells = malloc(sizeof(vte_cell_t) * (size_t)v->cols);
        sl->cols  = v->cols;
    }
    if (sl->cells)
        memcpy(sl->cells, row_cells, sizeof(vte_cell_t) * (size_t)v->cols);
    v->sb_head = (v->sb_head + 1) % v->sb_cap;
    if (v->sb_count < v->sb_cap)
        v->sb_count++;
}

/* ── Screen operations ─────────────────────────────────────────────────── */

/* Fill range of cells with blank (does NOT emit callbacks). */
static void clear_cells_quiet(vte_t *v, int col1, int row1, int col2, int row2)
{
    vte_cell_t blank = blank_cell(v);
    for (int r = row1; r <= row2; r++)
        for (int c = col1; c <= col2; c++)
            *cell_at(v, c, r) = blank;
}

/* Scroll up within current scroll region by n lines. */
static void scroll_up(vte_t *v, int n)
{
    if (n <= 0) return;
    int top = v->margin_top;
    int bot = v->margin_bot;
    int region = bot - top + 1;
    if (n > region) n = region;

    /* Push departing lines into scrollback */
    for (int i = 0; i < n; i++)
        sb_push_line(v, cell_at(v, 1, top + i));

    /* Shift lines up */
    int move = region - n;
    if (move > 0)
        memmove(cell_at(v, 1, top),
                cell_at(v, 1, top + n),
                sizeof(vte_cell_t) * (size_t)(v->cols * move));

    /* Blank the vacated lines at the bottom */
    clear_cells_quiet(v, 1, bot - n + 1, v->cols, bot);
}

/* Scroll down within current scroll region by n lines. */
static void scroll_down(vte_t *v, int n)
{
    if (n <= 0) return;
    int top = v->margin_top;
    int bot = v->margin_bot;
    int region = bot - top + 1;
    if (n > region) n = region;

    int move = region - n;
    if (move > 0)
        memmove(cell_at(v, 1, top + n),
                cell_at(v, 1, top),
                sizeof(vte_cell_t) * (size_t)(v->cols * move));

    clear_cells_quiet(v, 1, top, v->cols, top + n - 1);
}

/* Clamp cursor to grid. */
static void clamp_cursor(vte_t *v)
{
    if (v->cx < 1)       v->cx = 1;
    if (v->cx > v->cols) v->cx = v->cols;
    if (v->cy < 1)       v->cy = 1;
    if (v->cy > v->rows) v->cy = v->rows;
}

/* Move cursor, clear pending-wrap flag. */
static void move_cursor(vte_t *v, int col, int row)
{
    v->cx = col;
    v->cy = row;
    v->pending_wrap = false;
    clamp_cursor(v);
}

/* ── Print a single codepoint at cursor ────────────────────────────────── */

static void print_char(vte_t *v, uint32_t cp)
{
    /* Handle autowrap */
    if (v->pending_wrap) {
        /* wrap: move to next line */
        v->pending_wrap = false;
        v->cx = 1;
        if (v->cy == v->margin_bot) {
            scroll_up(v, 1);
            /* repaint entire scroll region after scroll */
            for (int r = v->margin_top; r <= v->margin_bot; r++)
                for (int c = 1; c <= v->cols; c++)
                    emit_cell(v, c, r, cell_at(v, c, r));
        } else {
            v->cy++;
        }
    }

    vte_cell_t cell = {
        .codepoint = cp,
        .fg        = v->fg,
        .bg        = v->bg,
        .attr      = v->attr,
    };
    cell.fg_rgb = v->fg_rgb;
    cell.bg_rgb = v->bg_rgb;
    *cell_at(v, v->cx, v->cy) = cell;
    emit_cell(v, v->cx, v->cy, &cell);

    if (v->cx >= v->cols) {
        if (v->autowrap)
            v->pending_wrap = true;
        /* else cursor stays at last column */
    } else {
        v->cx++;
    }
}

/* ── SGR (colour/attribute) handler ────────────────────────────────────── */

static void do_sgr(vte_t *v)
{
    /* With no params, SGR 0 (reset). */
    if (v->nparams == 0) {
        v->params[0] = 0;
        v->nparams   = 1;
    }
    for (int i = 0; i < v->nparams; i++) {
        int p = v->params[i];
        if (p == 0) {
            v->fg   = VTE_COLOR_DEFAULT_FG;
            v->bg   = VTE_COLOR_DEFAULT_BG;
            v->attr = 0;
            v->fg_rgb = 0;
            v->bg_rgb = 0;
        } else if (p == 1) {
            v->attr |= VTE_ATTR_BOLD;
        } else if (p == 2) {
            v->attr &= ~VTE_ATTR_BOLD; /* dim = unbold for BBS */
        } else if (p == 4) {
            v->attr |= VTE_ATTR_UNDERLINE;
        } else if (p == 5) {
            v->attr |= VTE_ATTR_BLINK;
        } else if (p == 7) {
            v->attr |= VTE_ATTR_REVERSE;
        } else if (p == 8) {
            v->attr |= VTE_ATTR_CONCEALED;
        } else if (p == 22) {
            v->attr &= ~VTE_ATTR_BOLD;
        } else if (p == 24) {
            v->attr &= ~VTE_ATTR_UNDERLINE;
        } else if (p == 25) {
            v->attr &= ~VTE_ATTR_BLINK;
        } else if (p == 27) {
            v->attr &= ~VTE_ATTR_REVERSE;
        } else if (p >= 30 && p <= 37) {
            v->fg = (uint8_t)(p - 30);
        } else if (p == 38 && i + 2 < v->nparams && v->params[i + 1] == 5) {
            /* 256-colour fg: ESC[38;5;Nm */
            int idx = v->params[i + 2];
            v->fg = (uint8_t)(idx & 0xFF);
            i += 2;
        } else if (p == 38 && i + 4 < v->nparams && v->params[i + 1] == 2) {
            /* Truecolor fg: ESC[38;2;R;G;Bm */
            v->fg_rgb = ((uint32_t)v->params[i + 2] << 16)
                      | ((uint32_t)v->params[i + 3] << 8)
                      |  (uint32_t)v->params[i + 4];
            v->attr |= VTE_ATTR_FG_RGB;
            i += 4;
        } else if (p == 39) {
            v->fg = VTE_COLOR_DEFAULT_FG;
            v->attr &= ~VTE_ATTR_FG_RGB;
            v->fg_rgb = 0;
        } else if (p >= 40 && p <= 47) {
            v->bg = (uint8_t)(p - 40);
        } else if (p == 48 && i + 2 < v->nparams && v->params[i + 1] == 5) {
            /* 256-colour bg: ESC[48;5;Nm */
            int idx = v->params[i + 2];
            v->bg = (uint8_t)(idx & 0xFF);
            i += 2;
        } else if (p == 48 && i + 4 < v->nparams && v->params[i + 1] == 2) {
            /* Truecolor bg: ESC[48;2;R;G;Bm */
            v->bg_rgb = ((uint32_t)v->params[i + 2] << 16)
                      | ((uint32_t)v->params[i + 3] << 8)
                      |  (uint32_t)v->params[i + 4];
            v->attr |= VTE_ATTR_BG_RGB;
            i += 4;
        } else if (p == 49) {
            v->bg = VTE_COLOR_DEFAULT_BG;
            v->attr &= ~VTE_ATTR_BG_RGB;
            v->bg_rgb = 0;
        } else if (p >= 90 && p <= 97) {
            /* Bright fg (xterm 256-colour extension) */
            v->fg = (uint8_t)(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            /* Bright bg */
            v->bg = (uint8_t)(p - 100 + 8);
        }
        /* Unrecognised params are silently ignored per spec. */
    }
}

/* ── CSI dispatch ───────────────────────────────────────────────────────── */

#define PARAM(n, def) (v->nparams > (n) && v->params[n] > 0 ? v->params[n] : (def))

static void dispatch_csi(vte_t *v, uint8_t final)
{
    /* Private marker from intermediate (e.g. '?' from DEC private modes) */
    bool priv = (v->ninter > 0 && v->inter[0] == '?');

    switch (final) {
    /* ── Cursor movement ── */
    case 'A': /* CUU - cursor up */
        move_cursor(v, v->cx, v->cy - PARAM(0, 1));
        break;
    case 'B': /* CUD - cursor down */
        move_cursor(v, v->cx, v->cy + PARAM(0, 1));
        break;
    case 'C': /* CUF - cursor forward */
        move_cursor(v, v->cx + PARAM(0, 1), v->cy);
        break;
    case 'D': /* CUB - cursor backward */
        move_cursor(v, v->cx - PARAM(0, 1), v->cy);
        break;
    case 'E': /* CNL - cursor next line */
        move_cursor(v, 1, v->cy + PARAM(0, 1));
        break;
    case 'F': /* CPL - cursor previous line */
        move_cursor(v, 1, v->cy - PARAM(0, 1));
        break;
    case 'G': /* CHA - cursor horizontal absolute */
        move_cursor(v, PARAM(0, 1), v->cy);
        break;
    case 'H': /* CUP - cursor position */
    case 'f': /* HVP - horizontal vertical position */
        move_cursor(v, PARAM(1, 1), PARAM(0, 1));
        break;

    case 'I': /* CHT - cursor forward tabulation */
    {
        int n   = PARAM(0, 1);
        int col = v->cx;
        for (int i = 0; i < n; i++) {
            col = ((col / 8) + 1) * 8 + 1;
            if (col > v->cols) { col = v->cols; break; }
        }
        move_cursor(v, col, v->cy);
        break;
    }

    case 'Z': /* CBT - cursor backward tabulation */
    {
        int n   = PARAM(0, 1);
        int col = v->cx;
        for (int i = 0; i < n; i++) {
            col = ((col - 2) / 8) * 8 + 1;
            if (col < 1) { col = 1; break; }
        }
        move_cursor(v, col, v->cy);
        break;
    }

    /* ── Erase ── */
    case 'J': /* ED - erase in display */
    {
        int p = PARAM(0, 0);
        if (p == 0) { /* below */
            clear_cells_quiet(v, v->cx, v->cy, v->cols, v->cy);
            clear_cells_quiet(v, 1, v->cy + 1, v->cols, v->rows);
            emit_clear(v, v->cx, v->cy, v->cols, v->rows);
        } else if (p == 1) { /* above */
            clear_cells_quiet(v, 1, 1, v->cx, v->cy);
            emit_clear(v, 1, 1, v->cx, v->cy);
        } else if (p == 2) { /* all */
            clear_cells_quiet(v, 1, 1, v->cols, v->rows);
            emit_clear(v, 1, 1, v->cols, v->rows);
        }
        break;
    }
    case 'K': /* EL - erase in line */
    {
        int p = PARAM(0, 0);
        if (p == 0) { /* to end */
            clear_cells_quiet(v, v->cx, v->cy, v->cols, v->cy);
            emit_clear(v, v->cx, v->cy, v->cols, v->cy);
        } else if (p == 1) { /* to start */
            clear_cells_quiet(v, 1, v->cy, v->cx, v->cy);
            emit_clear(v, 1, v->cy, v->cx, v->cy);
        } else if (p == 2) { /* whole line */
            clear_cells_quiet(v, 1, v->cy, v->cols, v->cy);
            emit_clear(v, 1, v->cy, v->cols, v->cy);
        }
        break;
    }
    case 'L': /* IL - insert lines */
    {
        int n = PARAM(0, 1);
        scroll_down(v, n);
        /* repaint scroll region */
        for (int r = v->margin_top; r <= v->margin_bot; r++)
            for (int c = 1; c <= v->cols; c++)
                emit_cell(v, c, r, cell_at(v, c, r));
        break;
    }
    case 'M': /* DL - delete lines */
    {
        int n = PARAM(0, 1);
        scroll_up(v, n);
        for (int r = v->margin_top; r <= v->margin_bot; r++)
            for (int c = 1; c <= v->cols; c++)
                emit_cell(v, c, r, cell_at(v, c, r));
        break;
    }
    case 'P': /* DCH - delete characters */
    {
        int n = PARAM(0, 1);
        int avail = v->cols - v->cx + 1;
        if (n > avail) n = avail;
        int move = avail - n;
        if (move > 0)
            memmove(cell_at(v, v->cx, v->cy),
                    cell_at(v, v->cx + n, v->cy),
                    sizeof(vte_cell_t) * (size_t)move);
        vte_cell_t blank = blank_cell(v);
        for (int c = v->cx + move; c <= v->cols; c++)
            *cell_at(v, c, v->cy) = blank;
        for (int c = v->cx; c <= v->cols; c++)
            emit_cell(v, c, v->cy, cell_at(v, c, v->cy));
        break;
    }
    case '@': /* ICH - insert characters */
    {
        int n = PARAM(0, 1);
        int avail = v->cols - v->cx + 1;
        if (n > avail) n = avail;
        int move = avail - n;
        if (move > 0)
            memmove(cell_at(v, v->cx + n, v->cy),
                    cell_at(v, v->cx, v->cy),
                    sizeof(vte_cell_t) * (size_t)move);
        vte_cell_t blank = blank_cell(v);
        for (int c = v->cx; c < v->cx + n; c++)
            *cell_at(v, c, v->cy) = blank;
        for (int c = v->cx; c <= v->cols; c++)
            emit_cell(v, c, v->cy, cell_at(v, c, v->cy));
        break;
    }
    case 'X': /* ECH - erase characters */
    {
        int n = PARAM(0, 1);
        if (n > v->cols - v->cx + 1) n = v->cols - v->cx + 1;
        clear_cells_quiet(v, v->cx, v->cy, v->cx + n - 1, v->cy);
        emit_clear(v, v->cx, v->cy, v->cx + n - 1, v->cy);
        break;
    }

    /* ── Scroll ── */
    case 'S': /* SU - scroll up */
    {
        int n = PARAM(0, 1);
        scroll_up(v, n);
        for (int r = v->margin_top; r <= v->margin_bot; r++)
            for (int c = 1; c <= v->cols; c++)
                emit_cell(v, c, r, cell_at(v, c, r));
        break;
    }
    case 'T': /* SD - scroll down */
    {
        int n = PARAM(0, 1);
        scroll_down(v, n);
        for (int r = v->margin_top; r <= v->margin_bot; r++)
            for (int c = 1; c <= v->cols; c++)
                emit_cell(v, c, r, cell_at(v, c, r));
        break;
    }

    /* ── Modes ── */
    case 'h': /* SM / DECSET */
        if (!priv) {
            /* Standard modes: 4=IRM (insert mode) - ignore for now */
            for (int i = 0; i < v->nparams; i++) {
                if (v->params[i] == 7)
                    v->autowrap = true;
                if (v->params[i] == 20)
                    v->lnm = true;
            }
        } else {
            /* DEC private modes */
            for (int i = 0; i < v->nparams; i++) {
                int p = v->params[i];
                if (p == 7)  v->autowrap = true;
                if (p == 25) v->cursor_visible = true;
            }
        }
        break;
    case 'l': /* RM / DECRST */
        if (!priv) {
            for (int i = 0; i < v->nparams; i++) {
                if (v->params[i] == 7)
                    v->autowrap = false;
                if (v->params[i] == 20)
                    v->lnm = false;
            }
        } else {
            for (int i = 0; i < v->nparams; i++) {
                int p = v->params[i];
                if (p == 7)  v->autowrap = false;
                if (p == 25) v->cursor_visible = false;
            }
        }
        break;

    /* ── SGR ── */
    case 'm':
        do_sgr(v);
        break;

    /* ── Device status / position ── */
    case 'n': /* DSR */
        if (PARAM(0, 0) == 6) {
            /* CPR - cursor position report */
            char buf[32];
            int  n = snprintf(buf, sizeof(buf), "\033[%d;%dR", v->cy, v->cx);
            if (v->cb.response)
                v->cb.response(v, buf, (size_t)n, v->cb.user);
        } else if (PARAM(0, 0) == 5) {
            /* Status report - OK */
            if (v->cb.response)
                v->cb.response(v, "\033[0n", 4, v->cb.user);
        }
        break;
    case 'c': /* DA - device attributes */
        /* Respond as VT100 */
        if (v->cb.response)
            v->cb.response(v, "\033[?1;0c", 7, v->cb.user);
        break;

    /* ── Cursor save/restore ── */
    case 's': /* DECSC */
        v->saved_cx   = v->cx;
        v->saved_cy   = v->cy;
        v->saved_fg   = v->fg;
        v->saved_bg   = v->bg;
        v->saved_attr = v->attr;
        v->saved_fg_rgb = v->fg_rgb;
        v->saved_bg_rgb = v->bg_rgb;
        break;
    case 'u': /* DECRC */
        v->cx   = v->saved_cx ? v->saved_cx : 1;
        v->cy   = v->saved_cy ? v->saved_cy : 1;
        v->fg   = v->saved_fg;
        v->bg   = v->saved_bg;
        v->attr = v->saved_attr;
        v->fg_rgb = v->saved_fg_rgb;
        v->bg_rgb = v->saved_bg_rgb;
        clamp_cursor(v);
        v->pending_wrap = false;
        break;

    /* ── Scroll region ── */
    case 'r': /* DECSTBM */
    {
        int top = PARAM(0, 1);
        int bot = PARAM(1, v->rows);
        if (top < 1)       top = 1;
        if (bot > v->rows) bot = v->rows;
        if (top < bot) {
            v->margin_top = top;
            v->margin_bot = bot;
            move_cursor(v, 1, 1);
        }
        break;
    }

    /* Anything else: silently ignored. */
    default:
        break;
    }
}

/* ── ESC dispatch ──────────────────────────────────────────────────────── */

static void dispatch_esc(vte_t *v, uint8_t final)
{
    if (v->ninter == 0) {
        switch (final) {
        case '7': /* DECSC - save cursor */
            v->saved_cx   = v->cx;
            v->saved_cy   = v->cy;
            v->saved_fg   = v->fg;
            v->saved_bg   = v->bg;
            v->saved_attr = v->attr;
            v->saved_fg_rgb = v->fg_rgb;
            v->saved_bg_rgb = v->bg_rgb;
            break;
        case '8': /* DECRC - restore cursor */
            v->cx   = v->saved_cx ? v->saved_cx : 1;
            v->cy   = v->saved_cy ? v->saved_cy : 1;
            v->fg   = v->saved_fg;
            v->bg   = v->saved_bg;
            v->attr = v->saved_attr;
            v->fg_rgb = v->saved_fg_rgb;
            v->bg_rgb = v->saved_bg_rgb;
            clamp_cursor(v);
            v->pending_wrap = false;
            break;
        case 'D': /* IND - index (scroll down one) */
            if (v->cy == v->margin_bot) {
                scroll_up(v, 1);
                for (int r = v->margin_top; r <= v->margin_bot; r++)
                    for (int c = 1; c <= v->cols; c++)
                        emit_cell(v, c, r, cell_at(v, c, r));
            } else {
                v->cy++;
            }
            break;
        case 'E': /* NEL - next line */
            v->cx = 1;
            if (v->cy == v->margin_bot) {
                scroll_up(v, 1);
                for (int r = v->margin_top; r <= v->margin_bot; r++)
                    for (int c = 1; c <= v->cols; c++)
                        emit_cell(v, c, r, cell_at(v, c, r));
            } else {
                v->cy++;
            }
            v->pending_wrap = false;
            break;
        case 'M': /* RI - reverse index */
            if (v->cy == v->margin_top) {
                scroll_down(v, 1);
                for (int r = v->margin_top; r <= v->margin_bot; r++)
                    for (int c = 1; c <= v->cols; c++)
                        emit_cell(v, c, r, cell_at(v, c, r));
            } else {
                v->cy--;
            }
            break;
        case 'c': /* RIS - reset to initial state */
            vte_reset(v, true);
            break;
        default:
            break;
        }
    }
    /* Intermediate sequences (e.g. ESC ( B = USASCII charset) - ignore */
    if (v->ninter == 1) {
        /* ESC ( X = designate G0 charset; ESC ) X = G1 */
        if (v->inter[0] == '(' || v->inter[0] == ')') {
            /* Only track G0 for now. */
            if (v->inter[0] == '(') {
                if (final == '0') v->g0_linedraw = true;  /* DEC line-drawing */
                else              v->g0_linedraw = false; /* ASCII (B or A) */
            }
        }
    }
}

/* ── Control character handling ────────────────────────────────────────── */

static void execute(vte_t *v, uint8_t byte)
{
    switch (byte) {
    case 0x08: /* BS */
        if (v->cx > 1) { v->cx--; v->pending_wrap = false; }
        break;
    case 0x09: /* HT - horizontal tab */
    {
        int col = ((v->cx / 8) + 1) * 8 + 1;
        if (col > v->cols) col = v->cols;
        move_cursor(v, col, v->cy);
        break;
    }
    case 0x0A: /* LF */
    case 0x0B: /* VT */
    case 0x0C: /* FF */
        if (v->cy == v->margin_bot) {
            scroll_up(v, 1);
            for (int r = v->margin_top; r <= v->margin_bot; r++)
                for (int c = 1; c <= v->cols; c++)
                    emit_cell(v, c, r, cell_at(v, c, r));
        } else {
            v->cy++;
        }
        if (v->lnm) v->cx = 1;  /* LNM: treat LF as newline (CR+LF) */
        v->pending_wrap = false;
        break;
    case 0x0D: /* CR */
        v->cx = 1;
        v->pending_wrap = false;
        break;
    case 0x07: /* BEL - ignore (no beep in VTE, caller can add) */
        if (v->cb.bell) v->cb.bell(v, v->cb.user);
        break;
    case 0x0E: /* SO - shift out: switch to G1 (alt) charset */
        v->using_g1 = true;
        break;
    case 0x0F: /* SI - shift in: switch back to G0 charset */
        v->using_g1 = false;
        break;
    default:
        break;
    }
}

/* ── DEC STD-070 state machine ─────────────────────────────────────────── */

/* Reset parser intermediate/parameter state. */
static void parser_clear(vte_t *v)
{
    v->nparams = 0;
    v->ninter  = 0;
    memset(v->params, 0, sizeof(v->params));
    memset(v->inter,  0, sizeof(v->inter));
}

/* Accumulate a CSI parameter digit. */
static void param_digit(vte_t *v, uint8_t digit)
{
    if (v->nparams == 0) v->nparams = 1;
    if (v->nparams <= MAX_PARAMS)
        v->params[v->nparams - 1] = v->params[v->nparams - 1] * 10 + (digit - '0');
}

/* CSI parameter separator. */
static void param_sep(vte_t *v)
{
    /* Ensure at least param[0] is counted before advancing. */
    if (v->nparams == 0) v->nparams = 1;
    if (v->nparams < MAX_PARAMS) v->nparams++;
}

static void feed_byte(vte_t *v, uint8_t b)
{
    parser_state_t st = v->state;

    /* Handle C1 controls (0x80-0x9F) as their 7-bit equivalents in GROUND */
    if (st == ST_GROUND && b >= 0x80 && b <= 0x9F) {
        /* Treat as ESC + (b - 0x40), e.g. 0x9B = CSI */
        uint8_t equiv = b - 0x40;
        feed_byte(v, 0x1B);  /* ESC */
        feed_byte(v, equiv);
        return;
    }

    /* Any state: ESC cancels current sequence and starts new one */
    if (b == 0x1B && st != ST_OSC_STRING && st != ST_DCS_STRING && st != ST_SOS_PM_APC) {
        parser_clear(v);
        v->state = ST_ESCAPE;
        return;
    }

    switch (st) {
    /* ── GROUND ── */
    case ST_GROUND:
        if (b <= 0x1F) {
            execute(v, b);
        } else if (b >= 0x20 && b <= 0x7E) {
            /* Printable ASCII */
            uint32_t cp = v->cp437 ? cp437[b] : (uint32_t)b;
            /* Apply DEC line-drawing charset if G0 is so designated and not shifted out */
            if (!v->using_g1 && v->g0_linedraw && b >= 0x60 && b <= 0x7E)
                cp = dec_linedraw[b - 0x60];
            print_char(v, cp);
        } else if (b == 0x7F) {
            /* DEL - ignore */
        } else if (b >= 0x80 && v->cp437) {
            /* High CP437 */
            print_char(v, cp437[b]);
        } else if (b >= 0xC0 && b <= 0xDF) {
            v->utf8_accum  = b & 0x1F;
            v->utf8_remain = 1;
            v->state = ST_UTF8_1;
        } else if (b >= 0xE0 && b <= 0xEF) {
            v->utf8_accum  = b & 0x0F;
            v->utf8_remain = 2;
            v->state = ST_UTF8_2;
        } else if (b >= 0xF0 && b <= 0xF7) {
            v->utf8_accum  = b & 0x07;
            v->utf8_remain = 3;
            v->state = ST_UTF8_3;
        }
        break;

    /* ── UTF-8 continuation ── */
    case ST_UTF8_1:
    case ST_UTF8_2:
    case ST_UTF8_3:
        if ((b & 0xC0) == 0x80) {
            v->utf8_accum = (v->utf8_accum << 6) | (b & 0x3F);
            if (--v->utf8_remain == 0) {
                print_char(v, v->utf8_accum);
                v->state = ST_GROUND;
            }
        } else {
            /* Bad continuation - restart */
            v->state = ST_GROUND;
            feed_byte(v, b);
        }
        break;

    /* ── ESC ── */
    case ST_ESCAPE:
        if (b >= 0x20 && b <= 0x2F) {
            /* Intermediate */
            if (v->ninter < MAX_INTER) v->inter[v->ninter++] = (char)b;
            v->state = ST_ESC_INTER;
        } else if (b == '[') {
            parser_clear(v);
            v->state = ST_CSI_ENTRY;
        } else if (b == ']') {
            v->state = ST_OSC;
        } else if (b == 'P') {
            v->state = ST_DCS;
        } else if (b == 'X' || b == '^' || b == '_') {
            v->state = ST_SOS_PM_APC;
        } else if (b >= 0x30 && b <= 0x7E) {
            dispatch_esc(v, b);
            v->state = ST_GROUND;
        } else {
            /* C0 in ESC state: execute and stay */
            execute(v, b);
        }
        break;

    case ST_ESC_INTER:
        if (b >= 0x20 && b <= 0x2F) {
            if (v->ninter < MAX_INTER) v->inter[v->ninter++] = (char)b;
        } else if (b >= 0x30 && b <= 0x7E) {
            dispatch_esc(v, b);
            v->state = ST_GROUND;
        }
        break;

    /* ── CSI ── */
    case ST_CSI_ENTRY:
        if (b >= '0' && b <= '9') {
            param_digit(v, b);
            v->state = ST_CSI_PARAM;
        } else if (b == ';') {
            param_sep(v);
            v->state = ST_CSI_PARAM;
        } else if (b >= 0x20 && b <= 0x2F) {
            if (v->ninter < MAX_INTER) v->inter[v->ninter++] = (char)b;
            v->state = ST_CSI_INTER;
        } else if (b >= 0x40 && b <= 0x7E) {
            dispatch_csi(v, b);
            v->state = ST_GROUND;
        } else if (b >= 0x3C && b <= 0x3F) {
            /* Private marker ('?', '>', '<', '=') */
            if (v->ninter < MAX_INTER) v->inter[v->ninter++] = (char)b;
            v->state = ST_CSI_PARAM;
        } else {
            execute(v, b);
        }
        break;

    case ST_CSI_PARAM:
        if (b >= '0' && b <= '9') {
            param_digit(v, b);
        } else if (b == ';') {
            param_sep(v);
        } else if (b >= 0x40 && b <= 0x7E) {
            dispatch_csi(v, b);
            v->state = ST_GROUND;
        } else if (b >= 0x20 && b <= 0x2F) {
            if (v->ninter < MAX_INTER) v->inter[v->ninter++] = (char)b;
            v->state = ST_CSI_INTER;
        } else if (b <= 0x1F) {
            execute(v, b);
        }
        break;

    case ST_CSI_INTER:
        if (b >= 0x40 && b <= 0x7E) {
            dispatch_csi(v, b);
            v->state = ST_GROUND;
        } else if (b >= 0x20 && b <= 0x2F) {
            if (v->ninter < MAX_INTER) v->inter[v->ninter++] = (char)b;
        } else if (b >= 0x30 && b <= 0x3F) {
            v->state = ST_CSI_IGNORE;
        } else if (b <= 0x1F) {
            execute(v, b);
        }
        break;

    case ST_CSI_IGNORE:
        if (b >= 0x40 && b <= 0x7E)
            v->state = ST_GROUND;
        break;

    /* ── OSC / DCS / SOS/PM/APC: absorb until ST (ESC \) or BEL ── */
    case ST_OSC:
        /* First byte enters OSC_STRING, reset accumulation buffer */
        v->osc_len = 0;
        v->state = ST_OSC_STRING;
        if (b == 0x07) { v->state = ST_GROUND; break; } /* empty OSC + BEL */
        if (b != 0x1B)
            v->osc_buf[v->osc_len++] = (char)b;
        break;
    case ST_OSC_STRING:
        if (b == 0x07 || b == '\\') {
            /* OSC string complete - fire title callback if appropriate */
            v->osc_buf[v->osc_len < (int)sizeof(v->osc_buf) ? v->osc_len : (int)sizeof(v->osc_buf) - 1] = '\0';
            /* OSC format: "<code>;<text>" - codes 0 (icon+title) and 2 (title only) */
            if (v->cb.title && v->osc_len > 1) {
                int code = 0;
                int i = 0;
                while (i < v->osc_len && v->osc_buf[i] >= '0' && v->osc_buf[i] <= '9')
                    code = code * 10 + (v->osc_buf[i++] - '0');
                if (v->osc_buf[i] == ';' && (code == 0 || code == 2))
                    v->cb.title(v, v->osc_buf + i + 1, v->cb.user);
            }
            v->state = ST_GROUND;
        } else if (b == 0x1B) {
            /* ESC seen - next byte should be '\' to complete ST, handled above */
        } else {
            if (v->osc_len < (int)sizeof(v->osc_buf) - 1)
                v->osc_buf[v->osc_len++] = (char)b;
        }
        break;

    case ST_DCS:
        if (b == 0x1B) { v->state = ST_DCS_STRING; break; }
        break;
    case ST_DCS_STRING:
        if (b == '\\') { v->state = ST_GROUND; break; }
        break;

    case ST_SOS_PM_APC:
        if (b == 0x1B) { v->state = ST_GROUND; break; }
        break;
    }
}

/* ── Public API implementations ────────────────────────────────────────── */

vte_t *vte_create(int cols, int rows, int backlines, const vte_callbacks_t *cb, bool cp437_mode)
{
    if (cols <= 0 || rows <= 0) return NULL;

    vte_t *v = calloc(1, sizeof(*v));
    if (!v) return NULL;

    v->screen = calloc((size_t)(cols * rows), sizeof(vte_cell_t));
    if (!v->screen) { free(v); return NULL; }

    v->cols = cols;
    v->rows = rows;

    if (cb) v->cb = *cb;
    v->cp437 = cp437_mode;

    v->autowrap      = true;
    v->cursor_visible = true;
    v->lnm           = true;   /* LF = newline (CR+LF) - BBS default */
    v->fg = VTE_COLOR_DEFAULT_FG;
    v->bg = VTE_COLOR_DEFAULT_BG;
    v->cx = 1;
    v->cy = 1;
    v->margin_top = 1;
    v->margin_bot = rows;
    v->state = ST_GROUND;

    if (backlines > 0) {
        v->sb_lines = calloc((size_t)backlines, sizeof(sb_line_t));
        if (v->sb_lines) v->sb_cap = backlines;
    }

    /* Fill screen with blank cells */
    vte_cell_t blank = blank_cell(v);
    for (int i = 0; i < cols * rows; i++) v->screen[i] = blank;

    return v;
}

void vte_free(vte_t *v)
{
    if (!v) return;
    free(v->screen);
    if (v->sb_lines) {
        for (int i = 0; i < v->sb_cap; i++) free(v->sb_lines[i].cells);
        free(v->sb_lines);
    }
    free(v);
}

size_t vte_input(vte_t *v, const uint8_t *data, size_t len)
{
    if (!v || !data) return 0;
    for (size_t i = 0; i < len; i++)
        feed_byte(v, data[i]);
    emit_cursor(v);
    return len;
}

void vte_reset(vte_t *v, bool hard)
{
    if (!v) return;
    v->cx = v->cy = 1;
    v->fg = VTE_COLOR_DEFAULT_FG;
    v->bg = VTE_COLOR_DEFAULT_BG;
    v->attr = 0;
    v->pending_wrap = false;
    v->margin_top = 1;
    v->margin_bot = v->rows;
    v->state = ST_GROUND;
    parser_clear(v);
    if (hard) {
        v->autowrap = true;
        v->cursor_visible = true;
        v->saved_cx = v->saved_cy = 0;
    }
    clear_cells_quiet(v, 1, 1, v->cols, v->rows);
    emit_clear(v, 1, 1, v->cols, v->rows);
    emit_cursor(v);
}

void vte_resize(vte_t *v, int cols, int rows)
{
    if (!v || cols <= 0 || rows <= 0) return;
    if (cols == v->cols && rows == v->rows) return;

    vte_cell_t *ns = calloc((size_t)(cols * rows), sizeof(vte_cell_t));
    if (!ns) return;

    /* Fill new buffer with blanks, then copy old content */
    vte_cell_t blank = blank_cell(v);
    for (int i = 0; i < cols * rows; i++) ns[i] = blank;

    int copy_rows = rows < v->rows ? rows : v->rows;
    int copy_cols = cols < v->cols ? cols : v->cols;
    for (int r = 0; r < copy_rows; r++)
        memcpy(&ns[r * cols], &v->screen[r * v->cols], sizeof(vte_cell_t) * (size_t)copy_cols);

    free(v->screen);
    v->screen = ns;
    v->cols   = cols;
    v->rows   = rows;

    /* Clamp margins */
    if (v->margin_top > rows) v->margin_top = 1;
    if (v->margin_bot > rows) v->margin_bot = rows;

    clamp_cursor(v);
    vte_repaint(v);
}

void vte_cursor_pos(const vte_t *v, int *col, int *row)
{
    if (!v) return;
    if (col) *col = v->cx;
    if (row) *row = v->cy;
}

bool vte_get_cell(const vte_t *v, int col, int row, vte_cell_t *out)
{
    if (!v || col < 1 || col > v->cols || row < 1 || row > v->rows) return false;
    if (out) *out = *cell_at((vte_t *)v, col, row);
    return true;
}

int vte_cols(const vte_t *v) { return v ? v->cols : 0; }
int vte_rows(const vte_t *v) { return v ? v->rows : 0; }

bool vte_scrollback_get(const vte_t *v, int line, vte_cell_t *row_out, int *ncols)
{
    if (!v || !v->sb_lines || line < 0 || line >= v->sb_count) return false;
    /* line 0 = oldest */
    int idx;
    if (v->sb_count < v->sb_cap)
        idx = line;  /* buffer not yet wrapped */
    else
        idx = (v->sb_head + line) % v->sb_cap;
    const sb_line_t *sl = &v->sb_lines[idx];
    if (row_out && sl->cells) {
        int nc = sl->cols < v->cols ? sl->cols : v->cols;
        memcpy(row_out, sl->cells, sizeof(vte_cell_t) * (size_t)nc);
        /* Pad with blanks if narrower */
        vte_cell_t blank = { .codepoint = 0x20, .fg = VTE_COLOR_DEFAULT_FG,
                             .bg = VTE_COLOR_DEFAULT_BG, .attr = 0 };
        for (int c = nc; c < v->cols; c++) row_out[c] = blank;
    }
    if (ncols) *ncols = sl->cols;
    return true;
}

int vte_scrollback_lines(const vte_t *v)
{
    return v ? v->sb_count : 0;
}

void vte_repaint(vte_t *v)
{
    if (!v) return;
    for (int r = 1; r <= v->rows; r++)
        for (int c = 1; c <= v->cols; c++)
            emit_cell(v, c, r, cell_at(v, c, r));
    emit_cursor(v);
}

void vte_set_cp437(vte_t *v, bool cp437_mode)
{
    if (v) v->cp437 = cp437_mode;
}

void vte_set_user(vte_t *v, void *user)
{
    if (v) v->cb.user = user;
}
