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

/* ── TTF glyph texture cache ────────────────────────────────────────── */

/* Open-addressing hash table: key = (codepoint, fg_r, fg_g, fg_b),
 * value = SDL_Texture* + glyph metrics.  Evicted on font/palette change. */

#define GLYPH_CACHE_BITS  12                       /* 4096 slots */
#define GLYPH_CACHE_SIZE  (1 << GLYPH_CACHE_BITS)
#define GLYPH_CACHE_MASK  (GLYPH_CACHE_SIZE - 1)

typedef struct {
    uint32_t     key;       /* packed: codepoint XOR'd with fg color */
    uint32_t     cp;        /* original codepoint (for collision check) */
    uint8_t      fg_r, fg_g, fg_b;
    bool         occupied;
    SDL_Texture *tex;
    int          gw, gh;    /* glyph pixel dimensions */
} glyph_cache_entry_t;

typedef struct {
    glyph_cache_entry_t slots[GLYPH_CACHE_SIZE];
    int count;
} glyph_cache_t;

static uint32_t glyph_cache_hash(uint32_t cp, uint8_t r, uint8_t g, uint8_t b)
{
    /* FNV-1a inspired mix */
    uint32_t h = 2166136261u;
    h ^= cp;        h *= 16777619u;
    h ^= r;         h *= 16777619u;
    h ^= (uint32_t)g << 8;  h *= 16777619u;
    h ^= (uint32_t)b << 16; h *= 16777619u;
    return h;
}

static SDL_Texture *glyph_cache_get(glyph_cache_t *gc, uint32_t cp,
                                     uint8_t r, uint8_t g, uint8_t b,
                                     int *gw, int *gh)
{
    uint32_t h = glyph_cache_hash(cp, r, g, b);
    for (int i = 0; i < 8; i++) {  /* max 8 probes */
        uint32_t idx = (h + (uint32_t)i) & GLYPH_CACHE_MASK;
        glyph_cache_entry_t *e = &gc->slots[idx];
        if (!e->occupied) return NULL;
        if (e->cp == cp && e->fg_r == r && e->fg_g == g && e->fg_b == b) {
            *gw = e->gw;
            *gh = e->gh;
            return e->tex;
        }
    }
    return NULL;
}

static void glyph_cache_put(glyph_cache_t *gc, uint32_t cp,
                              uint8_t r, uint8_t g, uint8_t b,
                              SDL_Texture *tex, int gw, int gh)
{
    /* If cache is > 75% full, don't insert (evict on next clear) */
    if (gc->count > GLYPH_CACHE_SIZE * 3 / 4) return;

    uint32_t h = glyph_cache_hash(cp, r, g, b);
    for (int i = 0; i < 8; i++) {
        uint32_t idx = (h + (uint32_t)i) & GLYPH_CACHE_MASK;
        glyph_cache_entry_t *e = &gc->slots[idx];
        if (!e->occupied) {
            e->cp = cp;
            e->fg_r = r;  e->fg_g = g;  e->fg_b = b;
            e->tex = tex;
            e->gw = gw;   e->gh = gh;
            e->occupied = true;
            e->key = h;
            gc->count++;
            return;
        }
        /* Already cached? Update texture. */
        if (e->cp == cp && e->fg_r == r && e->fg_g == g && e->fg_b == b) {
            if (e->tex) SDL_DestroyTexture(e->tex);
            e->tex = tex;
            e->gw = gw;  e->gh = gh;
            return;
        }
    }
    /* All probe slots occupied; drop this glyph (will retry after eviction) */
    SDL_DestroyTexture(tex);
}

static void glyph_cache_clear(glyph_cache_t *gc)
{
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        if (gc->slots[i].occupied && gc->slots[i].tex)
            SDL_DestroyTexture(gc->slots[i].tex);
        gc->slots[i].occupied = false;
        gc->slots[i].tex = NULL;
    }
    gc->count = 0;
}

/* ── Context struct ─────────────────────────────────────────────────── */

struct photon_sdl {
    SDL_Window   *win;
    SDL_Renderer *ren;
    TTF_Font     *font;
    TTF_Font     *emoji_font; /* fallback for emoji/symbol glyphs */
    SDL_Texture  *texture;   /* full-screen streaming texture */
    /* CP437 bitmap glyph atlas: 16x16 grid of 8xFH glyphs, white on black.
     * SDL_TEXTUREACCESS_STATIC, SDL_PIXELFORMAT_RGBA8888.
     * Color-mod + alpha-mod used at draw time to tint the glyph. */
    SDL_Texture  *cp437_atlas; /* NULL if not built */
    int           cp437_fh;   /* font height used when atlas was built */

    int           cols, rows;
    int           cell_w, cell_h;
    int           font_pt;        /* TTF point size currently in use */
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
    bool          pal_dirty;    /* palette changed: shadow indices are stale */

    /* Pending window-driven resize.  Set by the SDL event handler when the
     * user drags the window to a new size; consumed by photon_sdl_check_resize().
     * new_cols/new_rows are 0 until a resize occurs. */
    bool          resize_pending;
    int           pending_cols;
    int           pending_rows;

    /* Fixed terminal size from settings.  When non-zero, window resize scales
     * the font to maintain this grid size rather than changing cols/rows. */
    int           fixed_cols;
    int           fixed_rows;

    /* Saved windowed dimensions: captured when entering fullscreen so we can
     * restore the correct logical size on exit (especially needed on Wayland,
     * where SDL_GetWindowSize returns the fullscreen size briefly after
     * exiting fullscreen, before the compositor sends the resize event). */
    int           pre_fullscreen_win_w;
    int           pre_fullscreen_win_h;

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

    /* TTF glyph texture cache (heap-allocated, ~200KB) */
    glyph_cache_t *glyph_cache;
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
        SDL_TEXTUREACCESS_TARGET,
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
    } else if (photon_terminus_ttf_size > 0) {
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

    /* Cell size: cell_h matches the requested font point size directly.
     * The CP437 bitmap atlas (8x16) is scaled to fit via SDL_RenderCopy. */
    int cell_h = (font_pt > 0) ? font_pt : 16;
    if (cell_h < 8) cell_h = 8;
    int cell_w = (cell_h + 1) / 2;  /* ~0.5 aspect ratio, rounded up */
    if (cell_w < 4) cell_w = 4;

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
    ctx->emoji_font = NULL;
    ctx->texture = tex;

    /* Try to load a system emoji/symbol fallback font */
    {
        static const char *emoji_paths[] = {
#ifdef __APPLE__
            "/System/Library/Fonts/Apple Color Emoji.ttc",
            "/System/Library/Fonts/AppleColorEmoji.ttc",
#elif defined(_WIN32)
            "C:\\Windows\\Fonts\\seguiemj.ttf",
            "C:\\Windows\\Fonts\\segoeui.ttf",
#else
            "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
            "/usr/share/fonts/noto-emoji/NotoColorEmoji.ttf",
            "/usr/share/fonts/google-noto-emoji/NotoColorEmoji.ttf",
            "/usr/share/fonts/truetype/noto/NotoEmoji-Regular.ttf",
#endif
            NULL
        };
        for (int i = 0; emoji_paths[i]; i++) {
            if (font_accessible(emoji_paths[i])) {
                ctx->emoji_font = TTF_OpenFont(emoji_paths[i], font_pt);
                if (ctx->emoji_font) {
                    PHOTON_DBG("loaded emoji fallback font: %s", emoji_paths[i]);
                    break;
                }
            }
        }
    }
    ctx->cols   = cols;
    ctx->rows   = rows;
    ctx->cell_w = cell_w;
    ctx->cell_h = cell_h;
    ctx->font_pt = (font_pt > 0) ? font_pt : 16;
    PHOTON_DBG("photon_sdl_create: %dx%d cells, cell=%dx%d, font_pt=%d, win=%dx%d",
               cols, rows, cell_w, cell_h, ctx->font_pt, win_w, win_h);
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

    /* Allocate TTF glyph texture cache */
    ctx->glyph_cache = calloc(1, sizeof(glyph_cache_t));

    /* Direct all rendering to the offscreen render-target texture.
     * This persists across SDL_RenderPresent() calls (unlike the default
     * back buffer which is undefined after present with double-buffering).
     * photon_sdl_present() blits this texture to the screen. */
    SDL_SetRenderTarget(ren, ctx->texture);

    /* Clear to black */
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);

    /* Blit the cleared render target to screen */
    SDL_SetRenderTarget(ren, NULL);
    SDL_RenderCopy(ren, ctx->texture, NULL, NULL);
    SDL_RenderPresent(ren);

    /* Leave render target active for subsequent draw calls */
    SDL_SetRenderTarget(ren, ctx->texture);

    return ctx;
}

void photon_sdl_free(photon_sdl_t *ctx)
{
    if (!ctx) return;
    if (ctx->texture) SDL_DestroyTexture(ctx->texture);
    if (ctx->cp437_atlas) SDL_DestroyTexture(ctx->cp437_atlas);
    if (ctx->glyph_cache) {
        glyph_cache_clear(ctx->glyph_cache);
        free(ctx->glyph_cache);
    }
    if (ctx->ren)     SDL_DestroyRenderer(ctx->ren);
    if (ctx->win)     SDL_DestroyWindow(ctx->win);
    if (ctx->font)    TTF_CloseFont(ctx->font);
    if (ctx->emoji_font) TTF_CloseFont(ctx->emoji_font);
    free(ctx->shadow);
    free(ctx);
}

/* ── Palette ────────────────────────────────────────────────────────── */

void photon_sdl_set_palette(photon_sdl_t *ctx, int index,
                            uint8_t r, uint8_t g, uint8_t b)
{
    if (!ctx || index < 0 || index > 255) return;
    if (ctx->pal[index].r == r &&
        ctx->pal[index].g == g &&
        ctx->pal[index].b == b)
        return;  /* no change */
    ctx->pal[index].r = r;
    ctx->pal[index].g = g;
    ctx->pal[index].b = b;
    /* Palette changed: shadow buffer tracks palette indices, not RGB.
     * Mark dirty so the next draw pass re-renders all cells with the
     * new palette mapping.  This fixes UI dialogs rendering with stale
     * terminal colours when the theme palette differs from the session
     * ANSI palette (e.g. black terminal bg vs blue theme bg). */
    ctx->pal_dirty = true;
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

/* ── Live font-size change ──────────────────────────────────────────── */

/* Reload the TTF font at a new point size, recompute cell geometry,
 * resize the SDL window to maintain the same terminal grid (cols x rows),
 * and return the new terminal dimensions via *new_cols / *new_rows.
 * Pass NULL for new_cols/new_rows if you don't need them back.
 * Returns true on success, false if the font could not be reloaded. */
bool photon_sdl_set_font_size(photon_sdl_t *ctx, int pt,
                              int *new_cols, int *new_rows)
{
    if (!ctx || pt <= 0) return false;

    /* Load replacement TTF font from embedded Terminus data */
    TTF_Font *new_font = NULL;
    if (photon_terminus_ttf_size > 0) {
        SDL_RWops *rw = SDL_RWFromConstMem(photon_terminus_ttf,
                                           (int)photon_terminus_ttf_size);
        if (rw) new_font = TTF_OpenFontRW(rw, 1, pt);
    }
    if (!new_font) {
        PHOTON_DBG("set_font_size: failed to reload TTF at %dpt", pt);
        return false;
    }
    TTF_SetFontHinting(new_font, TTF_HINTING_LIGHT);

    /* Optionally reload emoji fallback font at new size */
    TTF_Font *new_emoji = NULL;
    if (ctx->emoji_font) {
        static const char *emoji_paths[] = {
#ifdef __APPLE__
            "/System/Library/Fonts/Apple Color Emoji.ttc",
            "/System/Library/Fonts/AppleColorEmoji.ttc",
#elif defined(_WIN32)
            "C:\\Windows\\Fonts\\seguiemj.ttf",
            "C:\\Windows\\Fonts\\segoeui.ttf",
#else
            "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
            "/usr/share/fonts/noto-emoji/NotoColorEmoji.ttf",
            "/usr/share/fonts/google-noto-emoji/NotoColorEmoji.ttf",
            "/usr/share/fonts/truetype/noto/NotoEmoji-Regular.ttf",
#endif
            NULL
        };
        for (int i = 0; emoji_paths[i]; i++) {
            if (font_accessible(emoji_paths[i])) {
                new_emoji = TTF_OpenFont(emoji_paths[i], pt);
                if (new_emoji) break;
            }
        }
    }

    /* Recompute cell geometry (same logic as photon_sdl_create) */
    int cell_h = pt;
    if (cell_h < 8) cell_h = 8;
    int cell_w = (cell_h + 1) / 2;
    if (cell_w < 4) cell_w = 4;

    /* New window pixel size - keep same grid dimensions */
    bool is_fullscreen = (SDL_GetWindowFlags(ctx->win) &
                          (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
    int new_win_w, new_win_h;
    int new_grid_cols, new_grid_rows;
    if (is_fullscreen) {
        /* In fullscreen the window size is fixed; derive new grid from it */
        SDL_GetWindowSize(ctx->win, &new_win_w, &new_win_h);
        new_grid_cols = new_win_w / cell_w;
        new_grid_rows = new_win_h / cell_h;
        if (new_grid_cols < 1) new_grid_cols = 1;
        if (new_grid_rows < 1) new_grid_rows = 1;
    } else {
        /* Windowed: keep same grid, resize window to fit */
        new_grid_cols = ctx->cols;
        new_grid_rows = ctx->rows;
        new_win_w = new_grid_cols * cell_w;
        new_win_h = new_grid_rows * cell_h;
    }

    /* Replace font, geometry */
    TTF_CloseFont(ctx->font);
    ctx->font   = new_font;

    /* Invalidate glyph texture cache - cell dimensions changed */
    if (ctx->glyph_cache) glyph_cache_clear(ctx->glyph_cache);

    if (new_emoji) {
        if (ctx->emoji_font) TTF_CloseFont(ctx->emoji_font);
        ctx->emoji_font = new_emoji;
    }
    ctx->cell_w = cell_w;
    ctx->cell_h = cell_h;
    ctx->win_w  = new_win_w;
    ctx->font_pt = pt;
    ctx->win_h  = new_win_h;
    ctx->cols   = new_grid_cols;
    ctx->rows   = new_grid_rows;

    /* Resize SDL window */
    if (!is_fullscreen)
        SDL_SetWindowSize(ctx->win, new_win_w, new_win_h);

    /* Update HiDPI scale (window size changed) */
    int draw_w = new_win_w, draw_h = new_win_h;
    SDL_GetRendererOutputSize(ctx->ren, &draw_w, &draw_h);
    ctx->draw_w = draw_w;
    ctx->draw_h = draw_h;
    ctx->retina_scale = (draw_w > 0 && new_win_w > 0)
                        ? (float)draw_w / (float)new_win_w : 1.0f;
    SDL_RenderSetScale(ctx->ren, ctx->retina_scale, ctx->retina_scale);

    /* Rebuild render-target texture to match new window size */
    if (ctx->texture) {
        SDL_DestroyTexture(ctx->texture);
        ctx->texture = make_texture(ctx->ren, new_win_w, new_win_h);
        SDL_SetRenderTarget(ctx->ren, ctx->texture);
    }

    /* Update shadow buffer dimensions.
     * Fullscreen: grid may change; windowed: grid stays same as ctx->cols/rows. */
    if (is_fullscreen) {
        ctx->shadow_cols = new_grid_cols;
        ctx->shadow_rows = new_grid_rows;
    } else {
        ctx->shadow_cols = new_grid_cols;
        ctx->shadow_rows = new_grid_rows;
    }

    /* Clear shadow buffer so expose events trigger full repaint, not stale cache.
     * This fixes display issues on Windows/Linux where SDL doesn't auto-expose
     * the window after a programmatic resize. */
    if (ctx->shadow && new_grid_cols > 0 && new_grid_rows > 0) {
        size_t shadow_n = (size_t)new_grid_cols * (size_t)new_grid_rows;
        void *p = realloc(ctx->shadow, shadow_n * sizeof(vte_cell_t));
        if (p) {
            ctx->shadow = p;
            memset(ctx->shadow, 0, shadow_n * sizeof(vte_cell_t));
        }
    }

    photon_sdl_present(ctx);

    PHOTON_DBG("set_font_size: %dpt -> cell %dx%d, win %dx%d",
               pt, cell_w, cell_h, new_win_w, new_win_h);

    if (new_cols) *new_cols = ctx->cols;
    if (new_rows) *new_rows = ctx->rows;
    return true;
}

int photon_sdl_get_font_size(const photon_sdl_t *ctx)
{
    return ctx ? ctx->font_pt : 0;
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

/* Forward declaration - defined after colour helpers */
static SDL_Rect cell_rect(const photon_sdl_t *ctx, int col, int row);

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

/* Begin a new mouse text selection (sets start anchor).  After this, callers
 * should call photon_sdl_update_selection() on MOTION and
 * photon_sdl_end_selection() on MOUSEUP. */
void photon_sdl_start_selection(photon_sdl_t *ctx, int col, int row)
{
    if (!ctx) return;
    ctx->sel_active     = true;
    ctx->sel_have       = false;
    ctx->sel_start_col  = ctx->sel_end_col = col;
    ctx->sel_start_row  = ctx->sel_end_row = row;
}

/* Extend the live selection to a new endpoint while dragging. */
void photon_sdl_update_selection(photon_sdl_t *ctx, int col, int row)
{
    if (!ctx || !ctx->sel_active) return;
    ctx->sel_end_col = col;
    ctx->sel_end_row = row;
    ctx->sel_have    = true;
}

/* Finalize a mouse selection.  col/row is the release point.  Returns true if
 * a non-empty selection was made (sets PHOTON_KEY_COPY_SEL). */
bool photon_sdl_end_selection(photon_sdl_t *ctx, int col, int row)
{
    if (!ctx || !ctx->sel_active) return false;
    ctx->sel_active = false;
    ctx->sel_end_col = col;
    ctx->sel_end_row = row;
    /* Treat a single-cell click with no movement as a deselect */
    if (ctx->sel_start_col == col && ctx->sel_start_row == row) {
        ctx->sel_have = false;
        return false;
    }
    ctx->sel_have = true;
    photon_key_t k = { .code = PHOTON_KEY_COPY_SEL };
    kq_push(&ctx->keys, k);
    return true;
}

void photon_sdl_clear_selection(photon_sdl_t *ctx)
{
    if (!ctx) return;
    PHOTON_DBG("clear_selection: sel_have=%d sel_active=%d -> cleared",
        ctx->sel_have, ctx->sel_active);
    if (ctx->sel_have && ctx->shadow) {
        /* Erase highlight by forcing those shadow cells to compare dirty. */
        int r0 = ctx->sel_start_row < ctx->sel_end_row ? ctx->sel_start_row : ctx->sel_end_row;
        int r1 = ctx->sel_start_row > ctx->sel_end_row ? ctx->sel_start_row : ctx->sel_end_row;
        photon_sdl_invalidate_range(ctx, r0, r1);
    }
    ctx->sel_have   = false;
    ctx->sel_active = false;
}

bool photon_sdl_sel_active(const photon_sdl_t *ctx)
{
    return ctx && ctx->sel_active;
}

void photon_sdl_draw_selection(photon_sdl_t *ctx, int c0, int r0, int c1, int r1,
                               int visible_rows)
{
    if (!ctx || !ctx->ren) return;

    SDL_SetRenderDrawBlendMode(ctx->ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ctx->ren, 255, 255, 255, 80);

    for (int r = r0; r <= r1 && r < visible_rows; r++) {
        int ca, cb;
        if (r0 == r1) {
            ca = c0; cb = c1;
        } else if (r == r0) {
            ca = c0; cb = ctx->cols - 1;
        } else if (r == r1) {
            ca = 0; cb = c1;
        } else {
            ca = 0; cb = ctx->cols - 1;
        }
        /* 1-based cell rect */
        SDL_Rect left  = cell_rect(ctx, ca + 1, r + 1);
        SDL_Rect right = cell_rect(ctx, cb + 1, r + 1);
        SDL_Rect span = { left.x, left.y, right.x + right.w - left.x, left.h };
        SDL_RenderFillRect(ctx->ren, &span);
    }

    SDL_SetRenderDrawBlendMode(ctx->ren, SDL_BLENDMODE_NONE);
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

    /* Flush palette dirty: invalidate shadow so every cell re-renders
     * with the new palette RGB mapping. */
    if (ctx->pal_dirty) {
        photon_sdl_invalidate(ctx);
        ctx->pal_dirty = false;
    }

    /* Clean up cursor at this position before drawing the cell.
     * Erase the underline from the previous cursor state if needed. */
    if (ctx->cur_visible && col == ctx->cur_col && row == ctx->cur_row) {
        SDL_Rect dst = cell_rect(ctx, col, row);
        /* Draw over the cursor line with the cell's background color */
        SDL_Color bgc;
        if (cell->attr & VTE_ATTR_BG_RGB) {
            bgc.r = (uint8_t)((cell->bg_rgb >> 16) & 0xFF);
            bgc.g = (uint8_t)((cell->bg_rgb >>  8) & 0xFF);
            bgc.b = (uint8_t)( cell->bg_rgb        & 0xFF);
        } else {
            bgc = pal_color(ctx, cell->bg);
        }
        SDL_SetRenderDrawColor(ctx->ren, bgc.r, bgc.g, bgc.b, 255);
        SDL_RenderDrawLine(ctx->ren,
            dst.x, dst.y + ctx->cell_h - 2,
            dst.x + ctx->cell_w - 1, dst.y + ctx->cell_h - 2);
    }

    /* Skip draw if shadow buffer already matches (avoids redundant SDL calls) */
    if (ctx->shadow && col <= ctx->shadow_cols && row <= ctx->shadow_rows) {
        vte_cell_t *sh = &ctx->shadow[(row - 1) * ctx->shadow_cols + (col - 1)];
        if (sh->codepoint == cell->codepoint &&
            sh->fg == cell->fg && sh->bg == cell->bg &&
            sh->attr == cell->attr &&
            sh->fg_rgb == cell->fg_rgb && sh->bg_rgb == cell->bg_rgb)
            return;
        *sh = *cell;
    }

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
        /* TTF rendering with glyph texture cache */
        int gw = 0, gh = 0;
        SDL_Texture *cached = ctx->glyph_cache
            ? glyph_cache_get(ctx->glyph_cache, cp, fg.r, fg.g, fg.b, &gw, &gh)
            : NULL;
        if (cached) {
            /* Cache hit - just blit the cached texture */
            SDL_Rect src  = { 0, 0, gw, gh };
            SDL_Rect gdst = {
                dst.x,
                dst.y + (ctx->cell_h - gh) / 2,
                (gw < ctx->cell_w) ? gw : ctx->cell_w,
                gh
            };
            SDL_RenderCopy(ctx->ren, cached, &src, &gdst);
        } else {
            /* Cache miss - render glyph, cache the texture */
            TTF_Font *render_font = ctx->font;
            if (render_font && !TTF_GlyphIsProvided32(render_font, cp) &&
                ctx->emoji_font && TTF_GlyphIsProvided32(ctx->emoji_font, cp)) {
                render_font = ctx->emoji_font;
            }
            SDL_Surface *surf = render_font
                ? TTF_RenderGlyph32_Blended(render_font, cp, fg) : NULL;
            if (surf) {
                SDL_Texture *tex = SDL_CreateTextureFromSurface(ctx->ren, surf);
                SDL_FreeSurface(surf);
                if (tex) {
                    SDL_QueryTexture(tex, NULL, NULL, &gw, &gh);
                    SDL_Rect src  = { 0, 0, gw, gh };
                    SDL_Rect gdst = {
                        dst.x,
                        dst.y + (ctx->cell_h - gh) / 2,
                        (gw < ctx->cell_w) ? gw : ctx->cell_w,
                        gh
                    };
                    SDL_RenderCopy(ctx->ren, tex, &src, &gdst);
                    /* Cache it (transfers ownership on success) */
                    if (ctx->glyph_cache)
                        glyph_cache_put(ctx->glyph_cache, cp,
                                        fg.r, fg.g, fg.b, tex, gw, gh);
                    else
                        SDL_DestroyTexture(tex);
                }
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

    /* Draw an underline cursor at the bottom of the cell */
    SDL_Rect dst = cell_rect(ctx, col, row);
    SDL_Color cur_fg;
    if (cell && (cell->attr & VTE_ATTR_FG_RGB)) {
        cur_fg.r = (cell->fg_rgb >> 16) & 0xFF;
        cur_fg.g = (cell->fg_rgb >>  8) & 0xFF;
        cur_fg.b =  cell->fg_rgb        & 0xFF;
    } else {
        cur_fg = pal_color(ctx, cell ? cell->fg : 7);  /* default white */
    }
    SDL_SetRenderDrawColor(ctx->ren, cur_fg.r, cur_fg.g, cur_fg.b, 255);
    SDL_RenderDrawLine(ctx->ren,
        dst.x, dst.y + ctx->cell_h - 2,
        dst.x + ctx->cell_w - 1, dst.y + ctx->cell_h - 2);
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

    SDL_Color bgc = pal_color(ctx, bg & 0x0f);
    SDL_SetRenderDrawColor(ctx->ren, bgc.r, bgc.g, bgc.b, 255);

    SDL_Rect r = {
        (col1 - 1) * ctx->cell_w,
        (row1 - 1) * ctx->cell_h,
        (col2 - col1 + 1) * ctx->cell_w,
        (row2 - row1 + 1) * ctx->cell_h
    };
    SDL_RenderFillRect(ctx->ren, &r);
}

/* ── Bulk fill rect (single SDL call + shadow update) ───────────────── */

void photon_sdl_fill_rect(photon_sdl_t *ctx,
                          int col1, int row1, int col2, int row2,
                          uint8_t fg, uint8_t bg)
{
    if (!ctx) return;
    if (col1 < 1) col1 = 1;
    if (row1 < 1) row1 = 1;
    if (col2 > ctx->cols) col2 = ctx->cols;
    if (row2 > ctx->rows) row2 = ctx->rows;
    if (col1 > col2 || row1 > row2) return;

    /* Single GPU fill for the entire region */
    SDL_Color bgc = pal_color(ctx, bg & 0x0f);
    SDL_SetRenderDrawColor(ctx->ren, bgc.r, bgc.g, bgc.b, 255);
    SDL_Rect r = {
        (col1 - 1) * ctx->cell_w,
        (row1 - 1) * ctx->cell_h,
        (col2 - col1 + 1) * ctx->cell_w,
        (row2 - row1 + 1) * ctx->cell_h
    };
    SDL_RenderFillRect(ctx->ren, &r);

    /* Update shadow buffer in bulk */
    vte_cell_t blank = {
        .codepoint = ' ',
        .fg = fg, .bg = bg,
        .attr = 0,
        .fg_rgb = 0, .bg_rgb = 0,
    };
    if (ctx->shadow && ctx->shadow_cols >= col2 && ctx->shadow_rows >= row2) {
        for (int row = row1; row <= row2; row++) {
            vte_cell_t *dst = &ctx->shadow[(row - 1) * ctx->shadow_cols + (col1 - 1)];
            int n = col2 - col1 + 1;
            for (int i = 0; i < n; i++)
                dst[i] = blank;
        }
    }
}

/* ── Clear ───────────────────────────────────────────────────────────── */

/* Clear the render target to black and show it on screen immediately.
 * Erases any stale content (e.g. leftover pixels from a previous tab)
 * so the next repaint starts from a clean slate. */
void photon_sdl_clear(photon_sdl_t *ctx)
{
    if (!ctx) return;
    SDL_SetRenderTarget(ctx->ren, NULL);
    SDL_SetRenderDrawColor(ctx->ren, 0, 0, 0, 255);
    SDL_RenderClear(ctx->ren);
    SDL_RenderPresent(ctx->ren);
    SDL_SetRenderTarget(ctx->ren, ctx->texture);
}

/* ── Present ────────────────────────────────────────────────────────── */

void photon_sdl_present(photon_sdl_t *ctx)
{
    if (!ctx) return;
    /* Blit the persistent render-target texture to the screen back buffer.
     * All draw calls between presents go to ctx->texture (which retains
     * its content across frames), so the shadow-buffer skip optimisation
     * works correctly with double-buffered SDL. */
    SDL_SetRenderTarget(ctx->ren, NULL);
    SDL_RenderCopy(ctx->ren, ctx->texture, NULL, NULL);

    /* Draw selection highlight overlay on the screen backbuffer (ephemeral).
     * This avoids the overlay persisting on the render-target texture,
     * which caused highlight to remain visible after the selection was
     * cleared because repaint() skips cells that match the shadow buffer. */
    if (ctx->sel_have || ctx->sel_active) {
        int c0, r0, c1, r1;
        if (photon_sdl_get_selection(ctx, &c0, &r0, &c1, &r1)) {
            photon_sdl_draw_selection(ctx, c0, r0, c1, r1, ctx->rows);
        }
    }

    SDL_RenderPresent(ctx->ren);
    /* Re-activate the render target for the next frame's draw calls */
    SDL_SetRenderTarget(ctx->ren, ctx->texture);
}

/* ── Connecting splash ──────────────────────────────────────────────── */

void photon_sdl_show_connecting(photon_sdl_t *ctx, const char *bbs_name)
{
    if (!ctx) return;

    /* Use current palette background (respects theme) */
    SDL_Color bgc = pal_color(ctx, 0);  /* palette index 0 = background */
    SDL_SetRenderDrawColor(ctx->ren, bgc.r, bgc.g, bgc.b, 255);
    SDL_RenderClear(ctx->ren);

    /* Build message string */
    char msg[128];
    if (bbs_name && bbs_name[0])
        snprintf(msg, sizeof(msg), "Connecting to %s ...", bbs_name);
    else
        snprintf(msg, sizeof(msg), "Connecting ...");

    /* Use the TTF font to render centered text */
    if (ctx->font) {
        SDL_Color fgc = pal_color(ctx, 7);  /* palette index 7 = foreground */
        SDL_Surface *surf = TTF_RenderUTF8_Blended(ctx->font, msg, fgc);
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

    photon_sdl_present(ctx);
    /* Invalidate shadow so next repaint fully redraws over the splash */
    photon_sdl_invalidate(ctx);
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
/* Invalidate shadow buffer so the next photon_sdl_repaint() does a full redraw */
void photon_sdl_invalidate(photon_sdl_t *ctx)
{
    if (ctx && ctx->shadow && ctx->shadow_cols > 0 && ctx->shadow_rows > 0)
        memset(ctx->shadow, 0,
               (size_t)ctx->shadow_cols * ctx->shadow_rows * sizeof(vte_cell_t));
}

/* Invalidate a row range in the shadow buffer so those cells redraw on the
 * next repaint.  r0/r1 are 0-based inclusive row indices.  Useful for
 * selectively erasing stale overlays (e.g. selection highlight) without
 * redrawing the entire screen. */
void photon_sdl_invalidate_range(photon_sdl_t *ctx, int r0, int r1)
{
    if (!ctx || !ctx->shadow || ctx->shadow_cols == 0 || ctx->shadow_rows == 0)
        return;
    if (r0 < 0) r0 = 0;
    if (r1 >= ctx->shadow_rows) r1 = ctx->shadow_rows - 1;
    if (r0 > r1) return;
    for (int r = r0; r <= r1; r++)
        memset(&ctx->shadow[r * ctx->shadow_cols], 0,
               (size_t)ctx->shadow_cols * sizeof(vte_cell_t));
}

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

    /* Flush palette dirty: invalidate shadow so every cell re-renders
     * with the new palette RGB mapping. */
    if (ctx->pal_dirty) {
        photon_sdl_invalidate(ctx);
        ctx->pal_dirty = false;
    }

    int rows = (vte_rows(vte) < ctx->rows) ? vte_rows(vte) : ctx->rows;
    int cols = (vte_cols(vte) < ctx->cols) ? vte_cols(vte) : ctx->cols;

    bool have_shadow = (ctx->shadow != NULL &&
                        ctx->shadow_cols == ctx->cols &&
                        ctx->shadow_rows == ctx->rows);

    /* Direct pointer to VTE screen buffer avoids per-cell function call overhead */
    const vte_cell_t *screen = vte_screen_ptr(vte);
    int vte_cols_n = vte_cols(vte);

    /* Single-pass dirty-cell repaint: skip cells that already match the
     * shadow buffer.  draw_cell fills the background rect per cell, so
     * we don't need a full SDL_RenderClear for incremental updates.
     * Each call to draw_cell updates the shadow, so the next repaint
     * cycle will skip those cells again if unchanged. */
    for (int r = 1; r <= rows; r++) {
        const vte_cell_t *row_src = screen
            ? &screen[(r-1) * vte_cols_n] : NULL;
        vte_cell_t *row_shd = have_shadow
            ? &ctx->shadow[(r-1) * ctx->shadow_cols] : NULL;

        for (int c = 0; c < cols; c++) {
            const vte_cell_t *cell = row_src ? &row_src[c] : NULL;
            if (!cell) continue;

            if (row_shd && memcmp(&row_shd[c], cell, sizeof(vte_cell_t)) == 0)
                continue;

            photon_sdl_draw_cell(ctx, c + 1, r, cell);
        }
    }

    /* Draw cursor on top of rendered content (respect DECTCEM hide).
     * Always read cursor position from the VTE so switching tabs
     * does not leave a stale cursor from the previous tab. */
    int cur_c = 0, cur_r = 0;
    vte_cursor_pos(vte, &cur_c, &cur_r);
    if (vte_cursor_visible(vte) &&
        cur_c >= 1 && cur_r >= 1 &&
        cur_c <= cols && cur_r <= rows) {
        vte_cell_t under_cell;
        vte_get_cell(vte, cur_c, cur_r, &under_cell);
        photon_sdl_draw_cursor(ctx, cur_c, cur_r, &under_cell);
    }
}

/* ── VTE callback adapter ───────────────────────────────────────────── */

static void vte_cb_cursor(vte_t *vte, int col, int row, void *user)
{
    (void)vte;
    photon_sdl_t *ctx = (photon_sdl_t *)user;
    /* Invalidate the OLD cursor position in the shadow buffer so it gets
     * redrawn without cursor on the next repaint. */
    if (ctx->cur_visible && ctx->cur_col >= 1 && ctx->cur_row >= 1 &&
        ctx->shadow && ctx->cur_col <= ctx->shadow_cols &&
        ctx->cur_row <= ctx->shadow_rows) {
        ctx->shadow[(ctx->cur_row - 1) * ctx->shadow_cols + (ctx->cur_col - 1)]
            = (vte_cell_t){0};
    }
    ctx->cur_col = col;
    ctx->cur_row = row;
}

/* Scroll callback: invalidate the shadow buffer for the scroll region so
 * photon_sdl_repaint() fully redraws those rows from VTE's screen buffer.
 * The render target texture retains pixel content across frames, so we
 * cannot just shift the shadow without also shifting the texture pixels;
 * blanking the shadow region is the simplest correct approach. */
static bool vte_cb_scroll(vte_t *vte, int top, int bot, int n, int dir, void *user)
{
    (void)vte; (void)n; (void)dir;
    photon_sdl_t *ctx = (photon_sdl_t *)user;
    if (!ctx->shadow) return false;
    int scols = ctx->shadow_cols;
    int srows = ctx->shadow_rows;
    if (top < 1 || bot > srows || top > bot) return false;

    /* Zero out the scroll region so every cell compares as dirty */
    for (int r = top; r <= bot; r++)
        memset(&ctx->shadow[(r - 1) * scols], 0,
               sizeof(vte_cell_t) * (size_t)scols);
    return true;
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
        .scroll   = vte_cb_scroll,
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
    photon_sdl_present(ctx);
    SDL_Delay(80);
    /* Invalidate shadow so next repaint redraws all cells (the white overlay
     * contaminated the render target and must be fully overwritten). */
    photon_sdl_invalidate(ctx);
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
            if (ctx->fixed_cols > 0 && ctx->fixed_rows > 0) {
                /* Fixed terminal size: scale the font to fit the configured
                 * grid in the new window dimensions. */
                int pt_w = log_w / ((ctx->fixed_cols + 1) / 2);  /* cell_w ~ pt/2 */
                int pt_h = log_h / ctx->fixed_rows;               /* cell_h ~ pt */
                int new_pt = pt_w < pt_h ? pt_w : pt_h;
                if (new_pt < 6) new_pt = 6;
                if (new_pt > 72) new_pt = 72;
                /* Round down to even for cleaner glyph metrics */
                new_pt &= ~1;
                if (new_pt >= 6 && new_pt != ctx->font_pt) {
                    int nc = 0, nr = 0;
                    photon_sdl_set_font_size(ctx, new_pt, &nc, &nr);
                }
            } else if (ctx->cell_w > 0 && ctx->cell_h > 0) {
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
            /* Invalidate shadow so next repaint redraws all cells */
            if (ctx->shadow && ctx->shadow_cols > 0 && ctx->shadow_rows > 0)
                memset(ctx->shadow, 0,
                       (size_t)ctx->shadow_cols * ctx->shadow_rows * sizeof(vte_cell_t));
        }
        if (ev->window.event == SDL_WINDOWEVENT_LEAVE) {
            /* Mouse left the window while dragging - if the button was released
             * outside, clear the selection so highlight doesn't get stuck. */
            if (ctx->sel_active &&
                !(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(SDL_BUTTON_LEFT)))
                photon_sdl_clear_selection(ctx);
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
        PHOTON_DBG("mouse: MOUSEDOWN x=%d y=%d cell_w=%d cell_h=%d cols=%d rows=%d",
            ev->button.x, ev->button.y, ctx->cell_w, ctx->cell_h, ctx->cols, ctx->rows);
        /* Clear any previous selection (invalidates shadow rows so the old
         * highlight overlay gets erased on the next repaint). */
        if (ctx->sel_have)
            photon_sdl_clear_selection(ctx);
        ctx->sel_active = true;
        int col = ev->button.x / ctx->cell_w;
        int row = ev->button.y / ctx->cell_h;
        if (col >= ctx->cols) col = ctx->cols - 1;
        if (row >= ctx->rows) row = ctx->rows - 1;
        PHOTON_DBG("mouse: start selection col=%d row=%d", col, row);
        ctx->sel_start_col = ctx->sel_end_col = col;
        ctx->sel_start_row = ctx->sel_end_row = row;
        return;
    }

    if (ev->type == SDL_MOUSEMOTION) {
        if (!ctx->sel_active) {
            PHOTON_DBG("mouse: MOTION but sel_active=FALSE (dropped)");
            return;
        }
        PHOTON_DBG("mouse: MOTION x=%d y=%d", ev->motion.x, ev->motion.y);
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

    if (ev->type == SDL_MOUSEBUTTONUP) {
        PHOTON_DBG("mouse: MOUSEUP button=%d x=%d y=%d sel_active=%d",
            ev->button.button, ev->button.x, ev->button.y, ctx->sel_active);
        if (ev->button.button == SDL_BUTTON_LEFT && ctx->sel_active) {
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

    /* Mouse wheel: scroll up enters/scrolls scrollback */
    if (ev->type == SDL_MOUSEWHEEL) {
        if (ev->wheel.y > 0) {
            photon_key_t k = { .code = PHOTON_KEY_SCROLL_UP };
            kq_push(&ctx->keys, k);
        }
        return;
    }

    if (ev->type != SDL_KEYDOWN)
        return;

    SDL_Keycode sym = ev->key.keysym.sym;
    int mod = 0;
    SDL_Keymod km = SDL_GetModState();
    if (km & KMOD_SHIFT)  mod |= PHOTON_MOD_SHIFT;
    if (km & KMOD_CTRL)   mod |= PHOTON_MOD_CTRL;
    if (km & KMOD_ALT)    mod |= PHOTON_MOD_ALT;
    if (km & KMOD_GUI)    mod |= PHOTON_MOD_META;

    /* Alt-Enter or F11: toggle fullscreen.
     * Use SDL_WINDOW_FULLSCREEN (not DESKTOP) so the window actually resizes
     * to the display resolution - this lets the grid expand to fill the screen.
     * SDL_WINDOW_FULLSCREEN_DESKTOP would scale the windowed content in place,
     * leaving cols/rows unchanged (visual scale ≠ grid resize). */
    if (((mod & PHOTON_MOD_ALT) && (sym == SDLK_RETURN || sym == SDLK_KP_ENTER))
        || sym == SDLK_F11) {
        Uint32 flags = SDL_GetWindowFlags(ctx->win);
        if (flags & SDL_WINDOW_FULLSCREEN) {
            /* Exiting fullscreen: restore the windowed size we saved on entry. */
            SDL_SetWindowFullscreen(ctx->win, 0);
            if (ctx->pre_fullscreen_win_w > 0 && ctx->pre_fullscreen_win_h > 0) {
                SDL_SetWindowSize(ctx->win,
                                  ctx->pre_fullscreen_win_w,
                                  ctx->pre_fullscreen_win_h);
            }
        } else {
            /* Entering fullscreen: save current windowed logical size first so we
             * can restore it exactly on exit (especially important on Wayland,
             * where SDL_GetWindowSize may briefly return the fullscreen resolution
             * before the compositor sends the windowed resize event). */
            SDL_GetWindowSize(ctx->win, &ctx->pre_fullscreen_win_w,
                             &ctx->pre_fullscreen_win_h);
            SDL_SetWindowFullscreen(ctx->win, SDL_WINDOW_FULLSCREEN);
        }
        /* Use SDL_GetRendererOutputSize for physical dimensions; these are
         * reliable immediately after a fullscreen transition on both X11 and
         * Wayland (they report the committed output size, not the logical
         * window size which can lag on Wayland). */
        int win_w, win_h, draw_w, draw_h;
        SDL_GetRendererOutputSize(ctx->ren, &draw_w, &draw_h);
        SDL_GetWindowSize(ctx->win, &win_w, &win_h);
        /* Use the larger of logical and physical so we fill the screen. */
        if (win_w < draw_w) win_w = draw_w;
        if (win_h < draw_h) win_h = draw_h;
        ctx->win_w = win_w;
        ctx->win_h = win_h;
        ctx->draw_w = draw_w;
        ctx->draw_h = draw_h;
        ctx->retina_scale = (win_w > 0) ? (float)draw_w / (float)win_w : 1.0f;
        if (ctx->fixed_cols > 0 && ctx->fixed_rows > 0) {
            /* Fixed grid: scale font to fit */
            int pt_w = win_w / ((ctx->fixed_cols + 1) / 2);
            int pt_h = win_h / ctx->fixed_rows;
            int new_pt = pt_w < pt_h ? pt_w : pt_h;
            if (new_pt < 6) new_pt = 6;
            if (new_pt > 72) new_pt = 72;
            new_pt &= ~1;
            if (new_pt >= 6 && new_pt != ctx->font_pt) {
                int nc = 0, nr = 0;
                photon_sdl_set_font_size(ctx, new_pt, &nc, &nr);
            }
        } else if (ctx->cell_w > 0 && ctx->cell_h > 0) {
            int nc = win_w / ctx->cell_w;
            int nr = win_h / ctx->cell_h;
            if (nc < 1) nc = 1;
            if (nr < 1) nr = 1;
            ctx->pending_cols = nc;
            ctx->pending_rows = nr;
        }
        ctx->resize_pending = true;
        return;
    }

    /* Alt+Plus/Minus or Ctrl+Plus/Minus: change font size (local, not passed
     * to the host).  Use the same hotkey on all platforms - the host never
     * needs raw +/- anyway. */
    if ((mod & (PHOTON_MOD_ALT | PHOTON_MOD_CTRL)) &&
        (sym == SDLK_PLUS || sym == SDLK_KP_PLUS ||
         sym == SDLK_MINUS || sym == SDLK_KP_MINUS ||
         sym == SDLK_EQUALS)) {
        int delta = (sym == SDLK_MINUS || sym == SDLK_KP_MINUS) ? -2 : 2;
        int new_pt = photon_sdl_get_font_size(ctx) + delta;
        if (new_pt < 6)  new_pt = 6;
        if (new_pt > 72) new_pt = 72;
        int nc = 0, nr = 0;
        photon_sdl_set_font_size(ctx, new_pt, &nc, &nr);
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
    /* Cmd+digit or Cmd+punctuation (macOS) - pass to host key hook */
    if (mod & PHOTON_MOD_META) {
        if ((sym >= SDLK_0 && sym <= SDLK_9) ||
             sym == SDLK_EQUALS || sym == SDLK_MINUS ||
             sym == SDLK_PLUS   || sym == SDLK_KP_PLUS ||
             sym == SDLK_KP_MINUS || sym == SDLK_KP_EQUALS) {
            photon_key_t k = { .code = (int)sym, .mod = mod };
            kq_push(&ctx->keys, k);
            return;
        }
    }
    /* Ctrl+digit or Ctrl+punctuation - pass to host key hook */
    if (mod & PHOTON_MOD_CTRL) {
        if ((sym >= SDLK_0 && sym <= SDLK_9) ||
             sym == SDLK_EQUALS || sym == SDLK_MINUS ||
             sym == SDLK_PLUS   || sym == SDLK_KP_PLUS ||
             sym == SDLK_KP_MINUS || sym == SDLK_KP_EQUALS) {
            photon_key_t k = { .code = (int)sym, .mod = mod };
            kq_push(&ctx->keys, k);
            return;
        }
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

    /* Catch mouse-up that happened outside the window.  SDL may not deliver
     * MOUSEBUTTONUP when the release occurs over another window.  Poll the
     * button state after draining all events to finalise a stale selection. */
    if (ctx->sel_active && !(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(SDL_BUTTON_LEFT)))
        photon_sdl_clear_selection(ctx);

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

/* Return pending grid dimensions when a resize is queued but not yet consumed.
 * This lets photon_ui layout code see the new grid immediately. */
int photon_sdl_cols(const photon_sdl_t *ctx)
{
    if (!ctx) return 0;
    return (ctx->resize_pending && ctx->pending_cols > 0)
           ? ctx->pending_cols : ctx->cols;
}
int photon_sdl_rows(const photon_sdl_t *ctx)
{
    if (!ctx) return 0;
    return (ctx->resize_pending && ctx->pending_rows > 0)
           ? ctx->pending_rows : ctx->rows;
}

void photon_sdl_set_fixed_size(photon_sdl_t *ctx, int cols, int rows)
{
    if (!ctx) return;
    ctx->fixed_cols = cols;
    ctx->fixed_rows = rows;
}

const vte_cell_t *photon_sdl_shadow_ptr(const photon_sdl_t *ctx)
{
    return ctx ? ctx->shadow : NULL;
}
int photon_sdl_shadow_cols(const photon_sdl_t *ctx)
{
    return ctx ? ctx->shadow_cols : 0;
}
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

    /* Query the actual renderer output size (correct on Wayland even before
     * the compositor has animated the window resize after fullscreen toggle). */
    int draw_w, draw_h;
    SDL_GetRendererOutputSize(ctx->ren, &draw_w, &draw_h);
    int log_w = draw_w, log_h = draw_h;
    SDL_GetWindowSize(ctx->win, &log_w, &log_h);
    /* Use the larger of logical and physical so we fill the screen. */
    if (log_w < draw_w) log_w = draw_w;
    if (log_h < draw_h) log_h = draw_h;

    ctx->win_w = log_w;
    ctx->win_h = log_h;
    ctx->draw_w = draw_w;
    ctx->draw_h = draw_h;
    ctx->retina_scale = (log_w > 0) ? (float)draw_w / (float)log_w : 1.0f;

    /* Reallocate shadow buffer */
    free(ctx->shadow);
    ctx->shadow = calloc((size_t)(new_cols * new_rows), sizeof(vte_cell_t));
    ctx->shadow_cols = new_cols;
    ctx->shadow_rows = new_rows;

    /* Recreate render target texture for new window size */
    if (ctx->texture) {
        SDL_DestroyTexture(ctx->texture);
        ctx->texture = make_texture(ctx->ren, log_w, log_h);
        SDL_SetRenderTarget(ctx->ren, ctx->texture);
    }

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

    /* Recreate render target texture for new window size */
    if (ctx->texture) {
        SDL_DestroyTexture(ctx->texture);
        ctx->texture = make_texture(ctx->ren, ctx->win_w, ctx->win_h);
        SDL_SetRenderTarget(ctx->ren, ctx->texture);
    }

    SDL_SetWindowSize(ctx->win, ctx->win_w, ctx->win_h);

    /* Recreate render target texture to match new window size */
    if (ctx->texture) {
        SDL_DestroyTexture(ctx->texture);
        ctx->texture = make_texture(ctx->ren, ctx->win_w, ctx->win_h);
        SDL_SetRenderTarget(ctx->ren, ctx->texture);
    }

    if (vte) {
        vte_resize(vte, new_cols, new_rows);
        photon_sdl_repaint(ctx, vte);
        photon_sdl_present(ctx);
    }
}
