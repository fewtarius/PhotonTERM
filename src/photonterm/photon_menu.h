/* SPDX-License-Identifier: GPL-3.0-or-later
 * photon_menu.h - Full-screen menu primitives (list and form menus) */

#ifndef PHOTON_MENU_H
#define PHOTON_MENU_H

#include "photon_ui.h"
#include "photon_sdl.h"
#include "photon_settings.h"
#include <stdbool.h>

/* ── Drawing helpers ─────────────────────────────────────────────────── */

/* Compose fg+bg into attribute byte (high nibble = bg, low = fg) */
#define PHOTON_MENU_ATTR(fg, bg) ((uint8_t)(((bg) << 4) | ((fg) & 0x0f)))

/* Theme-derived attribute shortcuts (pass a photon_theme_t pointer) */
#define PHOTON_MENU_A_NORM(t)    PHOTON_MENU_ATTR((t)->lclr,  (t)->bclr)
#define PHOTON_MENU_A_DIM(t)     PHOTON_MENU_ATTR((t)->cclr,  (t)->bclr)
#define PHOTON_MENU_A_HINT(t)    PHOTON_MENU_ATTR((t)->lclr,  (t)->bclr)
#define PHOTON_MENU_A_HINT_K(t)  PHOTON_MENU_ATTR((t)->hclr,  (t)->bclr)
#define PHOTON_MENU_A_ACCENT(t)  PHOTON_MENU_ATTR(CGA_BLACK,  (t)->hclr)
#define PHOTON_MENU_A_TITLE(t)   PHOTON_MENU_ATTR(CGA_BLACK,  (t)->hclr)
#define PHOTON_MENU_A_HDR(t)     PHOTON_MENU_ATTR((t)->lclr,  (t)->cclr)
#define PHOTON_MENU_A_SEL(t)     PHOTON_MENU_ATTR((t)->lbclr, (t)->lbbclr)
#define PHOTON_MENU_A_SEL_HI(t)  PHOTON_MENU_ATTR((t)->hclr,  (t)->lbbclr)
#define PHOTON_MENU_A_STATUS(t)  PHOTON_MENU_ATTR((t)->lclr,  (t)->bclr)

/* Get the active photon theme */
const photon_theme_t *photon_menu_theme(void);

/* Terminal dimensions */
int photon_menu_cols(photon_ui_t *ui);
int photon_menu_rows(photon_ui_t *ui);

/* Cell-level drawing */
void photon_menu_put_cell(photon_ui_t *ui, int col, int row,
                          uint32_t cp, uint8_t attr);
int  photon_menu_put_str(photon_ui_t *ui, int col, int row,
                         const char *s, uint8_t attr);
void photon_menu_fill_rect(photon_ui_t *ui,
                           int c1, int r1, int c2, int r2, uint8_t attr);
void photon_menu_put_padded(photon_ui_t *ui, int col, int row,
                            const char *s, int width, uint8_t attr);

/* Draw status bar (bottom row) */
void photon_menu_draw_statusbar(photon_ui_t *ui);

/* Draw title bar (row 1) */
void photon_menu_draw_titlebar(photon_ui_t *ui, const char *title);

/* ── Key hints ───────────────────────────────────────────────────────── */

/* A single key hint pair: "Key" + " Description  " */
typedef struct {
    const char *key;
    const char *desc;
} photon_hint_t;

/* Draw key hints centered on a row */
void photon_menu_draw_hints(photon_ui_t *ui, int row,
                            const photon_hint_t *hints, int nhints);

/* ── List menu ───────────────────────────────────────────────────────── */

/* Column definition for multi-column list */
typedef struct {
    const char *header;     /* column header text */
    int         width;      /* width in cells (0 = fill remaining) */
    int         offset;     /* byte offset into item data (for text columns) */
    int         max_len;    /* max field length at that offset */
} photon_menu_col_t;

/* Callback to render a single item row.
 * Given the item index, column range, row, and selection state,
 * render the item into cells. */
typedef void (*photon_menu_draw_item_fn)(photon_ui_t *ui,
                                         void *items, int index,
                                         int row, int col_start, int col_end,
                                         bool selected,
                                         const photon_theme_t *theme);

/* List menu configuration */
typedef struct {
    const char                *title;       /* title bar text */
    void                      *items;       /* opaque item array */
    int                        count;       /* number of items */
    const char                *empty_msg;   /* shown when count == 0 */
    photon_menu_draw_item_fn   draw_item;   /* item renderer callback */
    const photon_hint_t       *hints;       /* key hints */
    int                        nhints;      /* number of hints */
    int                        header_rows; /* extra rows for column headers (0 or 1) */
    /* Optional: header draw callback (draw column headers on row 2) */
    void (*draw_header)(photon_ui_t *ui, int row, int width,
                        const photon_theme_t *theme);
} photon_menu_list_t;

/* List menu state (caller owns this, persists cursor between redraws) */
typedef struct {
    int cursor;
    int scroll;
} photon_menu_list_state_t;

/* Draw a full-screen list menu (no event handling).
 * Call this in your event loop, then handle keys yourself.
 * Returns the number of visible rows. */
int photon_menu_draw_list(photon_ui_t *ui,
                          const photon_menu_list_t *menu,
                          photon_menu_list_state_t *state);

/* Standard list navigation key handler.
 * Updates cursor/scroll for arrow keys, pgup/pgdn, home/end.
 * Returns true if key was handled (navigation), false otherwise. */
bool photon_menu_list_handle_nav(photon_key_t *key,
                                 photon_menu_list_state_t *state,
                                 int count, int visible);

/* ── Form menu ───────────────────────────────────────────────────────── */

/* Field types */
typedef enum {
    PHOTON_MENU_FIELD_TEXT,       /* editable text (opens input on Enter) */
    PHOTON_MENU_FIELD_READONLY,   /* display-only text */
    PHOTON_MENU_FIELD_TOGGLE,     /* boolean [X]/[ ] */
    PHOTON_MENU_FIELD_ACTION,     /* button that triggers callback */
    PHOTON_MENU_FIELD_SEPARATOR,  /* blank row spacer */
    PHOTON_MENU_FIELD_BUTTON,     /* centered button like [Launch] or [Save] */
} photon_menu_field_type_t;

/* Form field definition */
typedef struct {
    const char          *label;    /* left-column label (NULL for buttons) */
    photon_menu_field_type_t  type;
    char                *buf;      /* text buffer (TEXT/READONLY) */
    int                  buflen;   /* buffer capacity */
    bool                *toggle;   /* pointer to bool (TOGGLE) */
    const char          *btn_text; /* button label text (BUTTON) */
    const char          *subtitle; /* secondary display line */
} photon_menu_field_t;

/* Form menu configuration */
typedef struct {
    const char          *title;    /* title bar text */
    photon_menu_field_t *fields;   /* field array */
    int                  nfields;  /* number of fields */
    const photon_hint_t *hints;    /* key hints */
    int                  nhints;   /* number of hints */
} photon_menu_form_t;

/* Form state */
typedef struct {
    int field;  /* currently focused field index */
} photon_menu_form_state_t;

/* Draw a full-screen form.
 * Returns the total number of focusable fields. */
int photon_menu_draw_form(photon_ui_t *ui,
                          const photon_menu_form_t *form,
                          photon_menu_form_state_t *state);

/* Standard form navigation key handler.
 * Handles up/down/tab between focusable fields, space for toggles.
 * Returns true if key was consumed. */
bool photon_menu_form_handle_nav(photon_key_t *key,
                                 photon_menu_form_state_t *state,
                                 photon_menu_form_t *form);

/* Get the number of focusable fields (skips SEPARATORs and READONLYs) */
int photon_menu_form_focusable_count(const photon_menu_form_t *form);

/* Map a focusable index to the actual field array index */
int photon_menu_form_focus_to_field(const photon_menu_form_t *form, int focus_idx);

/* Map a field array index to the focusable index, or -1 */
int photon_menu_form_field_to_focus(const photon_menu_form_t *form, int field_idx);

#endif /* PHOTON_MENU_H */
