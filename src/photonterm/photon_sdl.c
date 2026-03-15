/* photon_sdl.c - PhotonTERM SDL2 display / keyboard layer
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "photon_sdl.h"
#include "photon_compat.h"
#include "photon_cp437_font.h"
#include "photon_terminus_font.h"
#define PHOTON_DEBUG_BUILD
#include "photon_debug.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SDL2 headers.  We require SDL2 and SDL2_ttf. */
#include <SDL2/SDL.h>
#include <SDL_ttf.h>   /* 3rdp/prefix/include/SDL_ttf.h (not in SDL2 subdir) */

/* ── ANSI SGR 16-colour palette ─────────────────────────────────────── */
/* Named type for an RGB palette entry - avoids anonymous-struct type mismatch */
typedef struct { uint8_t r, g, b; } photon_rgb_t;

/* Canonical order per ECMA-48 / xterm: SGR 30-37 = indices 0-7, 90-97 = 8-15.
 * This is the norm. CGA hardware had a different index order; that is the deviation. */

static const photon_rgb_t ansi_sgr_16[16] = {
    {   0,   0,   0 },   /*  0 SGR 30/40  black          */
    { 170,   0,   0 },   /*  1 SGR 31/41  red            */
    {   0, 170,   0 },   /*  2 SGR 32/42  green          */
    { 170, 170,   0 },   /*  3 SGR 33/43  yellow/brown   */
    {   0,   0, 170 },   /*  4 SGR 34/44  blue           */
    { 170,   0, 170 },   /*  5 SGR 35/45  magenta        */
    {   0, 170, 170 },   /*  6 SGR 36/46  cyan           */
    { 170, 170, 170 },   /*  7 SGR 37/47  white/grey     */
    {  85,  85,  85 },   /*  8 SGR 90/100 bright black   */
    { 255,  85,  85 },   /*  9 SGR 91/101 bright red     */
    {  85, 255,  85 },   /* 10 SGR 92/102 bright green   */
    { 255, 255,  85 },   /* 11 SGR 93/103 bright yellow  */
    {  85,  85, 255 },   /* 12 SGR 94/104 bright blue    */
    { 255,  85, 255 },   /* 13 SGR 95/105 bright magenta */
    {  85, 255, 255 },   /* 14 SGR 96/106 bright cyan    */
    { 255, 255, 255 },   /* 15 SGR 97/107 bright white   */
};

/* ── xterm 256-colour palette init ─────────────────────────────────── */

/* Fill pal[0..255] with the standard xterm-256 colours:
 *   0-15:    system colours (xterm's exact values)
 *  16-231:   6x6x6 colour cube
 * 232-255:   24-step greyscale ramp */
static void init_xterm_256(photon_rgb_t pal[256])
{
    /* 0-15: standard xterm system colours */
    static const photon_rgb_t xterm16[16] = {
        {   0,   0,   0 }, { 128,   0,   0 }, {   0, 128,   0 }, { 128, 128,   0 },
        {   0,   0, 128 }, { 128,   0, 128 }, {   0, 128, 128 }, { 192, 192, 192 },
        { 128, 128, 128 }, { 255,   0,   0 }, {   0, 255,   0 }, { 255, 255,   0 },
        {   0,   0, 255 }, { 255,   0, 255 }, {   0, 255, 255 }, { 255, 255, 255 },
    };
    for (int i = 0; i < 16; i++) {
        pal[i].r = xterm16[i].r;
        pal[i].g = xterm16[i].g;
        pal[i].b = xterm16[i].b;
    }
    /* 16-231: 6x6x6 colour cube */
    for (int i = 16; i < 232; i++) {
        int n = i - 16;
        int b = n % 6; n /= 6;
        int g = n % 6; n /= 6;
        int r = n % 6;
        pal[i].r = r ? (uint8_t)(55 + r * 40) : 0;
        pal[i].g = g ? (uint8_t)(55 + g * 40) : 0;
        pal[i].b = b ? (uint8_t)(55 + b * 40) : 0;
    }
    /* 232-255: greyscale ramp */
    for (int i = 232; i < 256; i++) {
        uint8_t v = (uint8_t)(8 + (i - 232) * 10);
        pal[i].r = pal[i].g = pal[i].b = v;
    }
}

/* Fill pal[0..15] with the ANSI SGR 16-colour defaults. */
static void init_ansi_sgr_16(photon_rgb_t pal[256])
{
    for (int i = 0; i < 16; i++) {
        pal[i].r = ansi_sgr_16[i].r;
        pal[i].g = ansi_sgr_16[i].g;
        pal[i].b = ansi_sgr_16[i].b;
    }
}

/* ── Key queue ─────────────────────────────────────────────────────── */

#define KEY_QUEUE_CAP 64

typedef struct {
    photon_key_t items[KEY_QUEUE_CAP];
    int          head, tail, count;
} key_queue_t;

static void kq_push(key_queue_t *q, photon_key_t k)
{
    if (q->count >= KEY_QUEUE_CAP) return;
    q->items[q->tail] = k;
    q->tail = (q->tail + 1) % KEY_QUEUE_CAP;
    q->count++;
}

static bool kq_pop(key_queue_t *q, photon_key_t *out)
{
    if (q->count == 0) return false;
    *out = q->items[q->head];
    q->head = (q->head + 1) % KEY_QUEUE_CAP;
    q->count--;
    return true;
}

static bool kq_peek(const key_queue_t *q)
{
    return q->count > 0;
}

/* ── Context struct ─────────────────────────────────────────────────── */

struct photon_sdl {
    SDL_Window   *win;
    SDL_Renderer *ren;
    TTF_Font     *font;
    SDL_Texture  *texture;   /* full-screen streaming texture */
    /* CP437 bitmap glyph atlas: 16x16 grid of 8xFH glyphs, white on black.
     * SDL_TEXTUREACCESS_STATIC, SDL_PIXELFORMAT_RGBA8888.
     * Color-mod + alpha-mod used at draw time to tint the glyph. */
    SDL_Texture  *cp437_atlas; /* NULL if not built */
    int           cp437_fh;   /* font height used when atlas was built */

    int           cols, rows;
    int           cell_w, cell_h;
    int           win_w, win_h;
    int           draw_w, draw_h; /* physical drawable pixels (Retina 2x etc.) */
    float         retina_scale;   /* draw_w / win_w; typically 1.0 or 2.0 */

    /* palette */
    photon_rgb_t pal[256];

    /* cursor state */
    int           cur_col, cur_row;    /* 1-based; 0 = hidden */
    bool          cur_visible;
    Uint32        cur_blink_ms;
    bool          cur_blink_on;

    /* input */
    key_queue_t   keys;
    bool          quit;
    bool          expose_pending;  /* set on SDL_WINDOWEVENT_EXPOSED */

    /* Shadow cell buffer - mirrors what's currently displayed.
     * Allocated as cols*rows vte_cell_t.  Updated by photon_sdl_draw_cell.
     * Enables correct save/restore even for direct-draw UIs (bbslist). */
    vte_cell_t   *shadow;       /* heap: cols*rows, or NULL */
    int           shadow_cols;  /* dimensions when shadow was allocated */
    int           shadow_rows;

    /* Pending window-driven resize.  Set by the SDL event handler when the
     * user drags the window to a new size; consumed by photon_sdl_check_resize().
     * new_cols/new_rows are 0 until a resize occurs. */
    bool          resize_pending;
    int           pending_cols;
    int           pending_rows;

    /* Rendering mode.  When true, skip the CP437 bitmap atlas and render all
     * glyphs via TTF (Unicode/UTF-8 mode).  When false (default), use the
     * CP437 atlas for ASCII+CP437 chars and fall back to TTF for anything else. */
    bool          use_ttf;

    /* Mouse text selection.  sel_active: mouse button 1 currently held.
     * sel_start/sel_end are cell grid coords (0-based, col/row).
     * sel_have: at least one cell is selected (after button-up). */
    bool          sel_active;   /* dragging */
    bool          sel_have;     /* selection exists */
    int           sel_start_col, sel_start_row;
    int           sel_end_col,   sel_end_row;
};

/* ── Static error buffer ────────────────────────────────────────────── */

static char s_last_error[256];

const char *photon_sdl_last_error(void)
{
    return s_last_error;
}

static void set_error(const char *msg)
{
    snprintf(s_last_error, sizeof(s_last_error), "%s", msg);
}

/* ── Texture management ─────────────────────────────────────────────── */

static SDL_Texture *make_texture(SDL_Renderer *ren, int w, int h)
{
    SDL_Texture *t = SDL_CreateTexture(ren,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        w, h);
    return t;
}

/* ── Font search ────────────────────────────────────────────────────── */

/* Check if a font path is accessible. */
static bool font_accessible(const char *path) {
    SDL_RWops *rw = SDL_RWFromFile(path, "rb");
    if (rw) { SDL_RWclose(rw); return true; }
    return false;
}

/* Walk up from dir, appending suffix, return heap string if found. */
static char *walk_up_find(const char *start_dir, const char *suffix, int max_levels)
{
    char dir[4096];
    strlcpy(dir, start_dir, sizeof(dir));
    for (int i = 0; i <= max_levels; i++) {
        char candidate[4096];
        snprintf(candidate, sizeof(candidate), "%s/%s", dir, suffix);
        if (font_accessible(candidate)) return strdup(candidate);
        char *last = strrchr(dir, '/');
        if (!last || last == dir) break;
        *last = '\0';
    }
    return NULL;
}

/* Try to find a bundled Terminus TTF.  Returns a static or heap path. */
static const char *find_default_font(void)
{
    static const char *cached = NULL;
    if (cached) return cached;

    /* 1. Walk up from executable directory to find 3rdp/prefix tree (dev builds) */
    char *base = SDL_GetBasePath();
    if (base) {
        size_t bl = strlen(base);
        if (bl > 1 && base[bl-1] == '/') base[bl-1] = '\0'; /* trim trailing slash */
        const char *rel = "3rdp/prefix/share/fonts/terminus-ttf/TerminusTTF.ttf";
        char *found = walk_up_find(base, rel, 4);
        SDL_free(base);
        if (found) { cached = found; return found; }
    }

    /* 2. CWD-relative and absolute system/bundle paths */
    static const char *candidates[] = {
        "3rdp/prefix/share/fonts/terminus-ttf/TerminusTTF.ttf",
        "3rdp/prefix/share/fonts/terminus-ttf/TerminusTTF-4.49.3.ttf",
        "../Resources/Fonts/TerminusTTF.ttf",  /* macOS bundle relative */
        "/usr/share/fonts/truetype/terminus/TerminusTTF.ttf",
        "/usr/local/share/fonts/TerminusTTF.ttf",
        /* macOS system monospace fallbacks */
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Monaco.ttf",
        /* Linux common monospace fallbacks */
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (font_accessible(candidates[i])) {
            cached = candidates[i];
            return candidates[i];
        }
    }
    return NULL;
}

/* ── Create / free ──────────────────────────────────────────────────── */

photon_sdl_t *photon_sdl_create(const char *title,
                                int cols, int rows,
                                const char *font_path, int font_pt)
{
    if (cols <= 0 || rows <= 0) {
        set_error("cols and rows must be positive");
        return NULL;
    }
    if (font_pt <= 0) font_pt = 16;

    /* SDL init - caller may have already called SDL_Init; we OR in VIDEO */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        snprintf(s_last_error, sizeof(s_last_error),
                 "SDL_Init: %s", SDL_GetError());
        return NULL;
    }
    if (TTF_Init() < 0) {
        snprintf(s_last_error, sizeof(s_last_error),
                 "TTF_Init: %s", TTF_GetError());
        return NULL;
    }

    /* Find font: prefer embedded Terminus TTF (known-good regular weight),
     * fall back to external file only if embedded data is unavailable. */
    TTF_Font *font = NULL;
    if (font_path) {
        /* Caller explicitly requested a specific font file */
        font = TTF_OpenFont(font_path, font_pt);
        if (!font) {
            snprintf(s_last_error, sizeof(s_last_error),
                     "TTF_OpenFont(%s, %d): %s", font_path, font_pt, TTF_GetError());
            return NULL;
        }
    } else if (photon_terminus_ttf && photon_terminus_ttf_size > 0) {
        /* Use embedded Terminus TTF */
        SDL_RWops *rw = SDL_RWFromConstMem(photon_terminus_ttf,
                                           (int)photon_terminus_ttf_size);
        if (rw) font = TTF_OpenFontRW(rw, 1, font_pt);
        if (!font) {
            set_error("failed to load embedded TTF font");
            return NULL;
        }
    } else {
        /* No embedded font - search for external file */
        const char *found = find_default_font();
        if (found) {
            font = TTF_OpenFont(found, font_pt);
        }
        if (!font) {
            set_error("no TTF font available");
            return NULL;
        }
    }
    TTF_SetFontHinting(font, TTF_HINTING_LIGHT);

    /* Cell size: based on the CP437 bitmap font (8 wide, 16 tall) scaled.
     * font_pt acts as the desired cell pixel height; we compute integer
     * scale so the bitmap font fits cleanly (nearest multiple of 16). */
    int cell_h = (font_pt > 0) ? font_pt : 16;
    /* Round cell_h to a multiple of 16 for clean bitmap scaling */
    {
        int scale = cell_h / 16;
        if (scale < 1) scale = 1;
        cell_h = scale * 16;
    }
    int cell_w = cell_h / 2;   /* 8:16 = 1:2 aspect ratio */
    if (cell_w < 8) cell_w = 8;

    int win_w = cols * cell_w;
    int win_h = rows * cell_h;

    SDL_Window *win = SDL_CreateWindow(
        title ? title : "PhotonTERM",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        snprintf(s_last_error, sizeof(s_last_error),
                 "SDL_CreateWindow: %s", SDL_GetError());
        TTF_CloseFont(font);
        return NULL;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        snprintf(s_last_error, sizeof(s_last_error),
                 "SDL_CreateRenderer: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        TTF_CloseFont(font);
        return NULL;
    }

    /* Retina / HiDPI: get physical drawable pixels vs logical window size.
     * With SDL_WINDOW_ALLOW_HIGHDPI, the renderer outputs at full Retina resolution
     * but all SDL_Render* draw-calls use logical coordinates.  We set the render
     * scale so the renderer maps logical -> physical automatically; the font is
     * opened at logical pt so SDL_ttf glyph surfaces match logical coords. */
    int draw_w = win_w, draw_h = win_h;
    SDL_GetRendererOutputSize(ren, &draw_w, &draw_h);
    float retina_scale = (draw_w > 0 && win_w > 0)
                         ? (float)draw_w / (float)win_w : 1.0f;

    /* Tell the renderer to scale logical -> physical automatically */
    SDL_RenderSetScale(ren, retina_scale, retina_scale);

    SDL_Texture *tex = make_texture(ren, win_w, win_h);
    if (!tex) {
        snprintf(s_last_error, sizeof(s_last_error),
                 "SDL_CreateTexture: %s", SDL_GetError());
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        TTF_CloseFont(font);
        return NULL;
    }

    photon_sdl_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        set_error("calloc failed");
        SDL_DestroyTexture(tex);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        TTF_CloseFont(font);
        return NULL;
    }

    ctx->win    = win;
    ctx->ren    = ren;
    ctx->font   = font;
    ctx->texture = tex;
    ctx->cols   = cols;
    ctx->rows   = rows;
    ctx->cell_w = cell_w;
    ctx->cell_h = cell_h;
    ctx->win_w  = win_w;
    ctx->win_h  = win_h;
    ctx->draw_w = draw_w;
    ctx->draw_h = draw_h;
    ctx->retina_scale = retina_scale;
    ctx->cur_visible  = true;
    ctx->cur_blink_on = true;
    ctx->cur_blink_ms = SDL_GetTicks();

    /* Build CP437 8xFH bitmap glyph atlas (white glyphs on transparent) */
    {
        /* Build at 1x (8x16 per glyph); SDL_RenderCopy will scale to cell size */
        int gw = 8, gh = 16;
        /* Atlas layout: 16 cols x 16 rows = 256 glyphs */
        int atlas_w = gw * 16;
        int atlas_h = gh * 16;
        SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(
            0, atlas_w, atlas_h, 32, SDL_PIXELFORMAT_RGBA8888);
        if (surf) {
            SDL_FillRect(surf, NULL, SDL_MapRGBA(surf->format, 0, 0, 0, 0));
            for (int ch = 0; ch < 256; ch++) {
                int gcol = ch % 16;
                int grow = ch / 16;
                int ox = gcol * gw;
                int oy = grow * gh;
                /* Render each row of glyph bits at 1x */
                for (int y = 0; y < 16; y++) {
                    uint8_t bits = photon_cp437_8x16[ch][y];
                    for (int x = 0; x < gw; x++) {
                        if (bits & (0x80 >> x)) {
                            Uint32 *px = (Uint32 *)((uint8_t *)surf->pixels
                                + (oy + y) * surf->pitch
                                + (ox + x) * 4);
                            *px = SDL_MapRGBA(surf->format, 255, 255, 255, 255);
                        }
                    }
                }
            }
            SDL_Texture *atlas_tex = SDL_CreateTextureFromSurface(ren, surf);
            SDL_FreeSurface(surf);
            if (atlas_tex) {
                /* Use nearest-neighbour scaling to keep pixels crisp */
                SDL_SetTextureScaleMode(atlas_tex, SDL_ScaleModeNearest);
                SDL_SetTextureBlendMode(atlas_tex, SDL_BLENDMODE_BLEND);
                ctx->cp437_atlas = atlas_tex;
                ctx->cp437_fh    = gh;  /* always 16 (1x glyph height in atlas) */
            }
        }
    }

    /* Allocate shadow cell buffer */
    ctx->shadow = calloc((size_t)(cols * rows), sizeof(vte_cell_t));
    ctx->shadow_cols = cols;
    ctx->shadow_rows = rows;

    /* Load ANSI SGR 16-colour palette (0-15); indices 16-255 left zero */
    init_ansi_sgr_16(ctx->pal);

    /* Clear to black */
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderPresent(ren);

    return ctx;
}

void photon_sdl_free(photon_sdl_t *ctx)
{
    if (!ctx) return;
    if (ctx->texture) SDL_DestroyTexture(ctx->texture);
    if (ctx->cp437_atlas) SDL_DestroyTexture(ctx->cp437_atlas);
    if (ctx->ren)     SDL_DestroyRenderer(ctx->ren);
    if (ctx->win)     SDL_DestroyWindow(ctx->win);
    if (ctx->font)    TTF_CloseFont(ctx->font);
    free(ctx->shadow);
    free(ctx);
}

/* ── Palette ────────────────────────────────────────────────────────── */

void photon_sdl_set_palette(photon_sdl_t *ctx, int index,
                            uint8_t r, uint8_t g, uint8_t b)
{
    if (!ctx || index < 0 || index > 255) return;
    ctx->pal[index].r = r;
    ctx->pal[index].g = g;
    ctx->pal[index].b = b;
}

void photon_sdl_set_ttf_mode(photon_sdl_t *ctx, bool enable)
{
    if (ctx) {
        PHOTON_DBG("set_ttf_mode: %d -> %d", ctx->use_ttf, enable);
        ctx->use_ttf = enable;
    }
}

bool photon_sdl_get_ttf_mode(const photon_sdl_t *ctx)
{
    return ctx ? ctx->use_ttf : false;
}

void photon_sdl_save_palette(const photon_sdl_t *ctx, uint8_t buf[768])
{
    if (!ctx || !buf) return;
    for (int i = 0; i < 256; i++) {
        buf[i * 3 + 0] = ctx->pal[i].r;
        buf[i * 3 + 1] = ctx->pal[i].g;
        buf[i * 3 + 2] = ctx->pal[i].b;
    }
}

void photon_sdl_restore_palette(photon_sdl_t *ctx, const uint8_t buf[768])
{
    if (!ctx || !buf) return;
    for (int i = 0; i < 256; i++) {
        ctx->pal[i].r = buf[i * 3 + 0];
        ctx->pal[i].g = buf[i * 3 + 1];
        ctx->pal[i].b = buf[i * 3 + 2];
    }
}

void photon_sdl_load_xterm_palette(photon_sdl_t *ctx)
{
    if (!ctx) return;
    init_xterm_256(ctx->pal);
    PHOTON_DBG("palette: loaded xterm-256 (pal[1]=#%02x%02x%02x pal[2]=#%02x%02x%02x)",
               ctx->pal[1].r, ctx->pal[1].g, ctx->pal[1].b,
               ctx->pal[2].r, ctx->pal[2].g, ctx->pal[2].b);
}

void photon_sdl_load_ansi_palette(photon_sdl_t *ctx)
{
    if (!ctx) return;
    init_ansi_sgr_16(ctx->pal);
}

/* Alias: old name kept so existing callers still compile. */
void photon_sdl_load_cga_palette(photon_sdl_t *ctx)
{
    photon_sdl_load_ansi_palette(ctx);
}

bool photon_sdl_get_selection(const photon_sdl_t *ctx,
                               int *c0, int *r0, int *c1, int *r1)
{
    if (!ctx || !ctx->sel_have) return false;
    /* Normalize: start <= end (row-major) */
    int sr = ctx->sel_start_row, sc = ctx->sel_start_col;
    int er = ctx->sel_end_row,   ec = ctx->sel_end_col;
    if (sr > er || (sr == er && sc > ec)) {
        int tr = sr; sr = er; er = tr;
        int tc = sc; sc = ec; ec = tc;
    }
    if (c0) *c0 = sc;
    if (r0) *r0 = sr;
    if (c1) *c1 = ec;
    if (r1) *r1 = er;
    return true;
}

void photon_sdl_clear_selection(photon_sdl_t *ctx)
{
    if (!ctx) return;
    ctx->sel_have   = false;
    ctx->sel_active = false;
}

bool photon_sdl_sel_active(const photon_sdl_t *ctx)
{
    return ctx && ctx->sel_active;
}

/* ── Colour helpers ─────────────────────────────────────────────────── */

static SDL_Color pal_color(const photon_sdl_t *ctx, int idx)
{
    if (idx < 0 || idx > 255) idx = 7;
    SDL_Color c = { ctx->pal[idx].r, ctx->pal[idx].g, ctx->pal[idx].b, 255 };
    return c;
}

/* ── Cell destination rect ──────────────────────────────────────────── */

static SDL_Rect cell_rect(const photon_sdl_t *ctx, int col, int row)
{
    SDL_Rect r = {
        (col - 1) * ctx->cell_w,
        (row - 1) * ctx->cell_h,
        ctx->cell_w,
        ctx->cell_h
    };
    return r;
}

/* ── Draw one cell ──────────────────────────────────────────────────── */

void photon_sdl_draw_cell(photon_sdl_t *ctx, int col, int row,
                          const vte_cell_t *cell)
{
    if (!ctx || !cell) return;
    if (col < 1 || col > ctx->cols) return;
    if (row < 1 || row > ctx->rows) return;

    /* Update shadow buffer */
    if (ctx->shadow && col <= ctx->shadow_cols && row <= ctx->shadow_rows)
        ctx->shadow[(row - 1) * ctx->shadow_cols + (col - 1)] = *cell;

    int fg_idx = cell->fg;   /* full 0-255 xterm palette index */
    int bg_idx = cell->bg;   /* full 0-255 xterm palette index */

    /* Bold maps colour index 0-7 to high-intensity 8-15 */
    if ((cell->attr & VTE_ATTR_BOLD) && fg_idx < 8)   /* only for base 16 */
        fg_idx += 8;

    bool reversed = (cell->attr & VTE_ATTR_REVERSE) != 0;
    if (reversed) {
        int tmp = fg_idx;
        fg_idx  = bg_idx;
        bg_idx  = tmp;
    }

    SDL_Color fg = pal_color(ctx, fg_idx);
    SDL_Color bg = pal_color(ctx, bg_idx);

    /* Debug: log first few colored cells to verify palette */
    {
        static int color_log_count = 0;
        if (color_log_count < 20 && (fg_idx != 7 || bg_idx != 0)) {
            PHOTON_DBG("draw_cell: col=%d row=%d cp=U+%04X fg_idx=%d bg_idx=%d "
                       "fg=#%02x%02x%02x bg=#%02x%02x%02x attr=0x%02x ttf=%d",
                       col, row, cell->codepoint, fg_idx, bg_idx,
                       fg.r, fg.g, fg.b, bg.r, bg.g, bg.b,
                       cell->attr, ctx->use_ttf);
            color_log_count++;
        }
    }

    /* Override palette lookup with truecolor values if set */
    if (!reversed) {
        if (cell->attr & VTE_ATTR_FG_RGB) {
            fg.r = (cell->fg_rgb >> 16) & 0xFF;
            fg.g = (cell->fg_rgb >>  8) & 0xFF;
            fg.b =  cell->fg_rgb        & 0xFF;
        }
        if (cell->attr & VTE_ATTR_BG_RGB) {
            bg.r = (cell->bg_rgb >> 16) & 0xFF;
            bg.g = (cell->bg_rgb >>  8) & 0xFF;
            bg.b =  cell->bg_rgb        & 0xFF;
        }
    } else {
        /* reversed: fg/bg already swapped above by index; apply rgb override */
        if (cell->attr & VTE_ATTR_BG_RGB) {
            fg.r = (cell->bg_rgb >> 16) & 0xFF;
            fg.g = (cell->bg_rgb >>  8) & 0xFF;
            fg.b =  cell->bg_rgb        & 0xFF;
        }
        if (cell->attr & VTE_ATTR_FG_RGB) {
            bg.r = (cell->fg_rgb >> 16) & 0xFF;
            bg.g = (cell->fg_rgb >>  8) & 0xFF;
            bg.b =  cell->fg_rgb        & 0xFF;
        }
    }

    SDL_Rect dst = cell_rect(ctx, col, row);

    /* Draw background */
    SDL_SetRenderDrawColor(ctx->ren, bg.r, bg.g, bg.b, 255);
    SDL_RenderFillRect(ctx->ren, &dst);

    /* Skip blank/space codepoints */
    Uint32 cp = cell->codepoint;
    if (cp == 0 || cp == 0x20) return;
    if (cell->attr & VTE_ATTR_CONCEALED) return;

    /* Render glyph: CP437 bitmap atlas (pixel-perfect) unless TTF mode is on */
    bool glyph_drawn = false;
    if (ctx->cp437_atlas && !ctx->use_ttf) {
        uint8_t cp437_idx;
        if (cp < 0x80) {
            cp437_idx = (uint8_t)cp;
        } else {
            cp437_idx = photon_unicode_to_cp437(cp);
        }
        int atlas_col = cp437_idx % 16;
        int atlas_row = cp437_idx / 16;
        SDL_Rect src = {
            atlas_col * 8,
            atlas_row * 16,
            8,
            16
        };
        SDL_Rect gdst = {
            dst.x,
            dst.y,
            ctx->cell_w,
            ctx->cell_h
        };
        /* Tint the white atlas glyph with the foreground color */
        SDL_SetTextureColorMod(ctx->cp437_atlas, fg.r, fg.g, fg.b);
        SDL_RenderCopy(ctx->ren, ctx->cp437_atlas, &src, &gdst);
        glyph_drawn = true;
    }
    if (!glyph_drawn) {
        /* TTF fallback */
        SDL_Surface *surf = TTF_RenderGlyph32_Blended(ctx->font, cp, fg);
        if (surf) {
            SDL_Texture *tex = SDL_CreateTextureFromSurface(ctx->ren, surf);
            SDL_FreeSurface(surf);
            if (tex) {
                int gw, gh;
                SDL_QueryTexture(tex, NULL, NULL, &gw, &gh);
                SDL_Rect src  = { 0, 0, gw, gh };
                SDL_Rect gdst = {
                    dst.x,
                    dst.y + (ctx->cell_h - gh) / 2,
                    (gw < ctx->cell_w) ? gw : ctx->cell_w,
                    gh
                };
                SDL_RenderCopy(ctx->ren, tex, &src, &gdst);
                SDL_DestroyTexture(tex);
            }
        }
    }

    /* Underline */
    if (cell->attr & VTE_ATTR_UNDERLINE) {
        SDL_SetRenderDrawColor(ctx->ren, fg.r, fg.g, fg.b, 255);
        SDL_RenderDrawLine(ctx->ren,
            dst.x, dst.y + ctx->cell_h - 1,
            dst.x + ctx->cell_w - 1, dst.y + ctx->cell_h - 1);
    }

    /* Selection highlight: semi-transparent inversion overlay */
    if (ctx->sel_have || ctx->sel_active) {
        int c0, r0, c1, r1;
        if (photon_sdl_get_selection(ctx, &c0, &r0, &c1, &r1)) {
            int cc = col - 1;  /* convert to 0-based */
            int cr = row - 1;
            bool in_sel;
            if (r0 == r1)
                in_sel = (cr == r0 && cc >= c0 && cc <= c1);
            else if (cr == r0)
                in_sel = (cc >= c0);
            else if (cr == r1)
                in_sel = (cc <= c1);
            else
                in_sel = (cr > r0 && cr < r1);
            if (in_sel) {
                SDL_SetRenderDrawBlendMode(ctx->ren, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(ctx->ren, 255, 255, 255, 80);
                SDL_RenderFillRect(ctx->ren, &dst);
                SDL_SetRenderDrawBlendMode(ctx->ren, SDL_BLENDMODE_NONE);
            }
        }
    }
}

/* ── Draw cursor ────────────────────────────────────────────────────── */

void photon_sdl_draw_cursor(photon_sdl_t *ctx, int col, int row,
                            const vte_cell_t *cell)
{
    if (!ctx) return;

    /* Save cursor position */
    ctx->cur_col = col;
    ctx->cur_row = row;

    if (col < 1 || col > ctx->cols || row < 1 || row > ctx->rows) return;
    if (!ctx->cur_visible) return;

    /* Toggle blink every 500ms */
    Uint32 now = SDL_GetTicks();
    if (now - ctx->cur_blink_ms >= 500) {
        ctx->cur_blink_on = !ctx->cur_blink_on;
        ctx->cur_blink_ms = now;
    }

    /* Draw underlying cell first */
    if (cell) photon_sdl_draw_cell(ctx, col, row, cell);

    if (!ctx->cur_blink_on) return;

    /* Draw a block cursor using inverted palette entry 7 */
    SDL_Rect dst = cell_rect(ctx, col, row);
    SDL_SetRenderDrawBlendMode(ctx->ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ctx->ren, 170, 170, 170, 160);
    SDL_RenderFillRect(ctx->ren, &dst);
    SDL_SetRenderDrawBlendMode(ctx->ren, SDL_BLENDMODE_NONE);
}

/* ── Clear rect ─────────────────────────────────────────────────────── */

void photon_sdl_clear_rect(photon_sdl_t *ctx,
                           int col1, int row1, int col2, int row2,
                           uint8_t fg, uint8_t bg)
{
    (void)fg;
    if (!ctx) return;
    if (col1 < 1) col1 = 1;
    if (row1 < 1) row1 = 1;
    if (col2 > ctx->cols) col2 = ctx->cols;
    if (row2 > ctx->rows) row2 = ctx->rows;

    SDL_Color bgc = pal_color(ctx, bg & 0x07);
    SDL_SetRenderDrawColor(ctx->ren, bgc.r, bgc.g, bgc.b, 255);

    SDL_Rect r = {
        (col1 - 1) * ctx->cell_w,
        (row1 - 1) * ctx->cell_h,
        (col2 - col1 + 1) * ctx->cell_w,
        (row2 - row1 + 1) * ctx->cell_h
    };
    SDL_RenderFillRect(ctx->ren, &r);
}

/* ── Present ────────────────────────────────────────────────────────── */

void photon_sdl_present(photon_sdl_t *ctx)
{
    if (!ctx) return;
    SDL_RenderPresent(ctx->ren);
}

/* ── Connecting splash ──────────────────────────────────────────────── */

void photon_sdl_show_connecting(photon_sdl_t *ctx, const char *bbs_name)
{
    if (!ctx) return;

    /* Black screen */
    SDL_SetRenderDrawColor(ctx->ren, 0, 0, 0, 255);
    SDL_RenderClear(ctx->ren);

    /* Build message string */
    char msg[128];
    if (bbs_name && bbs_name[0])
        snprintf(msg, sizeof(msg), "Connecting to %s ...", bbs_name);
    else
        snprintf(msg, sizeof(msg), "Connecting ...");

    /* Use the TTF font to render centered text */
    if (ctx->font) {
        SDL_Color white = {255, 255, 255, 255};
        SDL_Surface *surf = TTF_RenderUTF8_Blended(ctx->font, msg, white);
        if (surf) {
            SDL_Texture *tex = SDL_CreateTextureFromSurface(ctx->ren, surf);
            if (tex) {
                int tw, th;
                SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
                /* Center in window */
                SDL_Rect dst = {
                    (ctx->cols * ctx->cell_w - tw) / 2,
                    (ctx->rows * ctx->cell_h - th) / 2,
                    tw, th
                };
                SDL_RenderCopy(ctx->ren, tex, NULL, &dst);
                SDL_DestroyTexture(tex);
            }
            SDL_FreeSurface(surf);
        }
    }

    SDL_RenderPresent(ctx->ren);
}

bool photon_sdl_get_cell(const photon_sdl_t *ctx, int col, int row,
                         vte_cell_t *cell)
{
    if (!ctx || !cell) return false;
    if (col < 1 || col > ctx->shadow_cols) return false;
    if (row < 1 || row > ctx->shadow_rows) return false;
    if (!ctx->shadow) return false;
    *cell = ctx->shadow[(row - 1) * ctx->shadow_cols + (col - 1)];
    return true;
}

/* ── Full repaint from VTE ──────────────────────────────────────────── */

/* Repaint from shadow buffer (used when window is exposed/uncovered) */
void photon_sdl_repaint_shadow(photon_sdl_t *ctx)
{
    if (!ctx || !ctx->shadow) return;
    int cols = ctx->shadow_cols;
    int rows = ctx->shadow_rows;
    for (int r = 1; r <= rows; r++) {
        for (int c = 1; c <= cols; c++) {
            const vte_cell_t *cell = &ctx->shadow[(r - 1) * cols + (c - 1)];
            photon_sdl_draw_cell(ctx, c, r, cell);
        }
    }
}

void photon_sdl_repaint(photon_sdl_t *ctx, vte_t *vte)
{
    if (!ctx || !vte) return;

    SDL_SetRenderDrawColor(ctx->ren, 0, 0, 0, 255);
    SDL_RenderClear(ctx->ren);

    int rows = (vte_rows(vte) < ctx->rows) ? vte_rows(vte) : ctx->rows;
    int cols = (vte_cols(vte) < ctx->cols) ? vte_cols(vte) : ctx->cols;

    for (int r = 1; r <= rows; r++) {
        for (int c = 1; c <= cols; c++) {
            vte_cell_t cell;
            if (vte_get_cell(vte, c, r, &cell))
                photon_sdl_draw_cell(ctx, c, r, &cell);
        }
    }

    /* Draw cursor on top of rendered content */
    if (ctx->cur_col >= 1 && ctx->cur_row >= 1 &&
        ctx->cur_col <= cols && ctx->cur_row <= rows) {
        vte_cell_t under_cell;
        vte_get_cell(vte, ctx->cur_col, ctx->cur_row, &under_cell);
        photon_sdl_draw_cursor(ctx, ctx->cur_col, ctx->cur_row, &under_cell);
    }
}

/* ── VTE callback adapter ───────────────────────────────────────────── */

static void vte_cb_cursor(vte_t *vte, int col, int row, void *user)
{
    (void)vte;
    photon_sdl_t *ctx = (photon_sdl_t *)user;
    ctx->cur_col = col;
    ctx->cur_row = row;
}

/* response callback is set by the connection layer, not by sdl */

vte_callbacks_t photon_sdl_make_vte_callbacks(photon_sdl_t *ctx)
{
    vte_callbacks_t cbs = {
        .draw     = NULL,   /* VTE screen[] is the source of truth; repaint reads it */
        .cursor   = vte_cb_cursor,
        .clear    = NULL,   /* same: no per-char SDL calls during vte_input */
        .response = NULL,
        .title    = NULL,   /* set via photon_sdl_set_vte_title_cb() if desired */
        .bell     = NULL,
        .user     = ctx,
    };
    return cbs;
}

void photon_sdl_set_title(photon_sdl_t *ctx, const char *title)
{
    if (ctx && ctx->win && title)
        SDL_SetWindowTitle(ctx->win, title);
}

void photon_sdl_bell_flash(photon_sdl_t *ctx)
{
    if (!ctx || !ctx->ren) return;
    /* Draw a brief white overlay then remove it.  This is called from the
     * SDL main thread context (via event or callback fired from vte_input). */
    SDL_SetRenderDrawBlendMode(ctx->ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ctx->ren, 255, 255, 255, 100);
    SDL_RenderFillRect(ctx->ren, NULL);  /* full window */
    SDL_RenderPresent(ctx->ren);
    SDL_Delay(80);
    /* Repaint will restore normal content on next frame */
}

/* ── SDL event -> key translation ───────────────────────────────────── */

static void translate_sdl_event(photon_sdl_t *ctx, const SDL_Event *ev)
{
    if (ev->type == SDL_QUIT) {
        ctx->quit = true;
        photon_key_t k = { .code = PHOTON_KEY_QUIT };
        kq_push(&ctx->keys, k);
        return;
    }

    if (ev->type == SDL_WINDOWEVENT) {
        if (ev->window.event == SDL_WINDOWEVENT_RESIZED ||
            ev->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            /* On Retina with ALLOW_HIGHDPI, event data1/data2 are physical pixels.
             * Compute grid from logical size (physical / retina_scale). */
            int draw_w = ev->window.data1;
            int draw_h = ev->window.data2;
            /* Update physical size */
            ctx->draw_w = draw_w;
            ctx->draw_h = draw_h;
            /* Derive logical window size and retina scale from renderer */
            int log_w = draw_w, log_h = draw_h;
            SDL_GetWindowSize(ctx->win, &log_w, &log_h);
            ctx->win_w = log_w;
            ctx->win_h = log_h;
            ctx->retina_scale = (log_w > 0) ? (float)draw_w / (float)log_w : 1.0f;
            if (ctx->cell_w > 0 && ctx->cell_h > 0) {
                int nc = log_w / ctx->cell_w;
                int nr = log_h / ctx->cell_h;
                if (nc < 1) nc = 1;
                if (nr < 1) nr = 1;
                if (nc != ctx->cols || nr != ctx->rows) {
                    ctx->resize_pending = true;
                    ctx->pending_cols   = nc;
                    ctx->pending_rows   = nr;
                }
            }
        }
        if (ev->window.event == SDL_WINDOWEVENT_EXPOSED) {
            ctx->expose_pending = true;
        }
        return;
    }

    if (ev->type == SDL_TEXTINPUT) {
        /* Suppress text input when Alt/Option, Ctrl, or Cmd is held - those are
         * hotkeys, not text.  The SDL_KEYDOWN handler below will process them. */
        SDL_Keymod km = SDL_GetModState();
        if ((km & KMOD_ALT) || (km & KMOD_CTRL) || (km & KMOD_GUI))
            return;
        /* Printable text (handles compose, dead keys, IME) */
        photon_key_t k = { .code = (unsigned char)ev->text.text[0] };
        strncpy(k.text, ev->text.text, sizeof(k.text) - 1);
        kq_push(&ctx->keys, k);
        return;
    }

    /* Mouse events for text selection */
    if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT) {
        /* Clear any prior selection and start a new drag */
        ctx->sel_have   = false;
        ctx->sel_active = true;
        int col = ev->button.x / ctx->cell_w;
        int row = ev->button.y / ctx->cell_h;
        if (col >= ctx->cols) col = ctx->cols - 1;
        if (row >= ctx->rows) row = ctx->rows - 1;
        ctx->sel_start_col = ctx->sel_end_col = col;
        ctx->sel_start_row = ctx->sel_end_row = row;
        return;
    }

    if (ev->type == SDL_MOUSEMOTION && ctx->sel_active) {
        int col = ev->motion.x / ctx->cell_w;
        int row = ev->motion.y / ctx->cell_h;
        if (col < 0) col = 0;
        if (row < 0) row = 0;
        if (col >= ctx->cols) col = ctx->cols - 1;
        if (row >= ctx->rows) row = ctx->rows - 1;
        ctx->sel_end_col = col;
        ctx->sel_end_row = row;
        ctx->sel_have    = true;  /* live highlight while dragging */
        return;
    }

    if (ev->type == SDL_MOUSEBUTTONUP && ev->button.button == SDL_BUTTON_LEFT
            && ctx->sel_active) {
        ctx->sel_active = false;
        int col = ev->button.x / ctx->cell_w;
        int row = ev->button.y / ctx->cell_h;
        if (col < 0) col = 0;
        if (row < 0) row = 0;
        if (col >= ctx->cols) col = ctx->cols - 1;
        if (row >= ctx->rows) row = ctx->rows - 1;
        ctx->sel_end_col = col;
        ctx->sel_end_row = row;
        /* Treat a single-cell click with no movement as a deselect */
        if (ctx->sel_start_col == col && ctx->sel_start_row == row)
            ctx->sel_have = false;
        else
            ctx->sel_have = true;
        /* Signal: push a special key so term loop can copy selection */
        if (ctx->sel_have) {
            photon_key_t k = { .code = PHOTON_KEY_COPY_SEL };
            kq_push(&ctx->keys, k);
        }
        return;
    }

    /* Middle or right click: paste from clipboard */
    if (ev->type == SDL_MOUSEBUTTONDOWN &&
        (ev->button.button == SDL_BUTTON_MIDDLE || ev->button.button == SDL_BUTTON_RIGHT)) {
        photon_key_t k = { .code = PHOTON_KEY_PASTE };
        kq_push(&ctx->keys, k);
        return;
    }

    if (ev->type != SDL_KEYDOWN) return;

    SDL_Keycode sym = ev->key.keysym.sym;
    int mod = 0;
    SDL_Keymod km = SDL_GetModState();
    if (km & KMOD_SHIFT)  mod |= PHOTON_MOD_SHIFT;
    if (km & KMOD_CTRL)   mod |= PHOTON_MOD_CTRL;
    if (km & KMOD_ALT)    mod |= PHOTON_MOD_ALT;
    if (km & KMOD_GUI)    mod |= PHOTON_MOD_META;

    /* Alt-Enter: toggle fullscreen */
    if ((mod & PHOTON_MOD_ALT) && (sym == SDLK_RETURN || sym == SDLK_KP_ENTER)) {
        Uint32 flags = SDL_GetWindowFlags(ctx->win);
        if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP)
            SDL_SetWindowFullscreen(ctx->win, 0);
        else
            SDL_SetWindowFullscreen(ctx->win, SDL_WINDOW_FULLSCREEN_DESKTOP);
        return;
    }

    /* Printable ASCII with Ctrl - SDL doesn't send TEXTINPUT for these */
    if ((mod & PHOTON_MOD_CTRL) && sym >= SDLK_a && sym <= SDLK_z) {
        photon_key_t k = {
            .code = (sym - SDLK_a) + 1,  /* Ctrl-A=1, Ctrl-B=2 ... */
            .mod  = mod
        };
        kq_push(&ctx->keys, k);
        return;
    }

    /* Special keys */
    struct { SDL_Keycode sym; int code; } specials[] = {
        { SDLK_UP,       PHOTON_KEY_UP    },
        { SDLK_DOWN,     PHOTON_KEY_DOWN  },
        { SDLK_LEFT,     PHOTON_KEY_LEFT  },
        { SDLK_RIGHT,    PHOTON_KEY_RIGHT },
        { SDLK_HOME,     PHOTON_KEY_HOME  },
        { SDLK_END,      PHOTON_KEY_END   },
        { SDLK_PAGEUP,   PHOTON_KEY_PGUP  },
        { SDLK_PAGEDOWN, PHOTON_KEY_PGDN  },
        { SDLK_INSERT,   PHOTON_KEY_INS   },
        { SDLK_DELETE,   PHOTON_KEY_DEL   },
        { SDLK_F1,       PHOTON_KEY_F1    },
        { SDLK_F2,       PHOTON_KEY_F2    },
        { SDLK_F3,       PHOTON_KEY_F3    },
        { SDLK_F4,       PHOTON_KEY_F4    },
        { SDLK_F5,       PHOTON_KEY_F5    },
        { SDLK_F6,       PHOTON_KEY_F6    },
        { SDLK_F7,       PHOTON_KEY_F7    },
        { SDLK_F8,       PHOTON_KEY_F8    },
        { SDLK_F9,       PHOTON_KEY_F9    },
        { SDLK_F10,      PHOTON_KEY_F10   },
        { SDLK_F11,      PHOTON_KEY_F11   },
        { SDLK_F12,      PHOTON_KEY_F12   },
        /* Enter, Tab, Backspace, Escape: send as ASCII */
        { SDLK_RETURN,   '\r'             },
        { SDLK_KP_ENTER, '\r'             },
        { SDLK_TAB,      '\t'             },
        { SDLK_BACKSPACE, '\x7f'          },   /* Mac Delete key -> DEL (0x7f) like macOS Terminal */
        { SDLK_ESCAPE,   27              },
        { 0, 0 }
    };

    for (int i = 0; specials[i].sym; i++) {
        if (sym == specials[i].sym) {
            photon_key_t k = { .code = specials[i].code, .mod = mod };
            kq_push(&ctx->keys, k);
            return;
        }
    }

    /* Alt+letter or Alt+digit - generate Alt+code so hotkeys work on macOS
     * (SDL_TEXTINPUT is suppressed when Alt is held on macOS) */
    if (mod & PHOTON_MOD_ALT) {
        if ((sym >= SDLK_a && sym <= SDLK_z) ||
            (sym >= SDLK_0 && sym <= SDLK_9)) {
            photon_key_t k = { .code = (int)sym, .mod = mod };
            kq_push(&ctx->keys, k);
            return;
        }
    }
    /* Cmd+letter (macOS) - generate key with PHOTON_MOD_META */
    if ((mod & PHOTON_MOD_META) && sym >= SDLK_a && sym <= SDLK_z) {
        photon_key_t k = { .code = (int)sym, .mod = mod };
        kq_push(&ctx->keys, k);
        return;
    }
    /* Other keys (e.g. bare modifiers, media keys) - ignored */
}

/* ── Input polling ──────────────────────────────────────────────────── */

/* Global SDL handle */
photon_sdl_t *photon_sdl_global = NULL;

void photon_sdl_flush_keys(photon_sdl_t *ctx)
{
    if (!ctx) return;
    /* Drain SDL event queue - handle QUIT so we don't lose it */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT)
            ctx->quit = true;
    }
    /* Clear internal key ring */
    ctx->keys.head = ctx->keys.tail = 0;
    /* If quit was set, re-enqueue PHOTON_KEY_QUIT so caller sees it */
    if (ctx->quit) {
        photon_key_t k = { .code = PHOTON_KEY_QUIT };
        kq_push(&ctx->keys, k);
    }
}

bool photon_sdl_poll_key(photon_sdl_t *ctx, photon_key_t *key)
{
    if (!ctx || !key) return false;

    /* First drain any pending SDL events */
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        translate_sdl_event(ctx, &ev);

    return kq_pop(&ctx->keys, key);
}

bool photon_sdl_peek_key(photon_sdl_t *ctx, photon_key_t *key)
{
    if (!ctx) return false;
    /* Drain pending events first */
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        translate_sdl_event(ctx, &ev);
    if (key && kq_peek(&ctx->keys)) {
        *key = ctx->keys.items[ctx->keys.head];
        return true;
    }
    return kq_peek(&ctx->keys);
}

bool photon_sdl_wait_key(photon_sdl_t *ctx, photon_key_t *key, int timeout_ms)
{
    if (!ctx || !key) return false;

    /* Already have something queued */
    if (kq_pop(&ctx->keys, key)) return true;

    Uint32 deadline = SDL_GetTicks() + (Uint32)timeout_ms;
    SDL_Event ev;
    while (SDL_GetTicks() < deadline) {
        int remaining = (int)(deadline - SDL_GetTicks());
        if (remaining <= 0) break;
        if (SDL_WaitEventTimeout(&ev, remaining)) {
            translate_sdl_event(ctx, &ev);
            if (kq_pop(&ctx->keys, key)) return true;
        }
    }
    return false;
}

bool photon_sdl_quit_requested(const photon_sdl_t *ctx)
{
    return ctx && ctx->quit;
}

bool photon_sdl_take_expose(photon_sdl_t *ctx)
{
    if (!ctx || !ctx->expose_pending) return false;
    ctx->expose_pending = false;
    return true;
}

/* ── Grid info ──────────────────────────────────────────────────────── */

int photon_sdl_cols(const photon_sdl_t *ctx)      { return ctx ? ctx->cols : 0; }
int photon_sdl_rows(const photon_sdl_t *ctx)      { return ctx ? ctx->rows : 0; }
int photon_sdl_cell_width(const photon_sdl_t *ctx) { return ctx ? ctx->cell_w : 0; }
int photon_sdl_cell_height(const photon_sdl_t *ctx){ return ctx ? ctx->cell_h : 0; }

/* ── Resize ─────────────────────────────────────────────────────────── */

/* Check for a pending window-driven resize (user dragged the window).
 * If a resize is pending, stores the new grid dimensions in nc and nr,
 * updates ctx->cols/rows, reallocates the shadow buffer, and returns true.
 * Returns false if no resize is pending. */
bool photon_sdl_check_resize(photon_sdl_t *ctx, int *nc, int *nr)
{
    if (!ctx || !ctx->resize_pending) return false;
    ctx->resize_pending = false;

    int new_cols = ctx->pending_cols;
    int new_rows = ctx->pending_rows;
    if (new_cols < 1) new_cols = 1;
    if (new_rows < 1) new_rows = 1;

    ctx->cols  = new_cols;
    ctx->rows  = new_rows;
    ctx->win_w = new_cols * ctx->cell_w;
    ctx->win_h = new_rows * ctx->cell_h;

    /* Reallocate shadow buffer */
    free(ctx->shadow);
    ctx->shadow = calloc((size_t)(new_cols * new_rows), sizeof(vte_cell_t));
    ctx->shadow_cols = new_cols;
    ctx->shadow_rows = new_rows;

    if (nc) *nc = new_cols;
    if (nr) *nr = new_rows;
    return true;
}

/* Programmatic resize (user requested a specific grid size via settings).
 * Resizes the SDL window to match and repaints. */
void photon_sdl_notify_resize(photon_sdl_t *ctx, vte_t *vte,
                              int new_cols, int new_rows)
{
    if (!ctx) return;
    ctx->cols  = new_cols;
    ctx->rows  = new_rows;
    ctx->win_w = new_cols * ctx->cell_w;
    ctx->win_h = new_rows * ctx->cell_h;

    /* Reallocate shadow buffer */
    free(ctx->shadow);
    ctx->shadow = calloc((size_t)(new_cols * new_rows), sizeof(vte_cell_t));
    ctx->shadow_cols = new_cols;
    ctx->shadow_rows = new_rows;

    SDL_SetWindowSize(ctx->win, ctx->win_w, ctx->win_h);
    if (vte) {
        vte_resize(vte, new_cols, new_rows);
        photon_sdl_repaint(ctx, vte);
        SDL_RenderPresent(ctx->ren);
    }
}
