/* SPDX-License-Identifier: GPL-3.0-or-later
 * photon_menu.c - Full-screen menu primitives (list and form menus) */

#include "photon_menu.h"
#include "photon_vte.h"
#include "photon_debug.h"

#include <stdio.h>
#include <string.h>

/* ── Theme access ────────────────────────────────────────────────────── */

extern int photon_active_theme;
extern const photon_theme_t photon_themes[];

const photon_theme_t *photon_menu_theme(void)
{
	int idx = photon_active_theme;
	if (idx < 0) idx = 0;
	int n = 0;
	while (photon_themes[n].name) n++;
	if (idx >= n) idx = 0;
	return &photon_themes[idx];
}

/* ── Terminal dimensions ─────────────────────────────────────────────── */

int photon_menu_cols(photon_ui_t *ui)
{
	return vte_cols(photon_ui_vte(ui));
}

int photon_menu_rows(photon_ui_t *ui)
{
	return vte_rows(photon_ui_vte(ui));
}

/* ── Cell-level drawing ──────────────────────────────────────────────── */

void photon_menu_put_cell(photon_ui_t *ui, int col, int row,
                          uint32_t cp, uint8_t attr)
{
	vte_cell_t cell = {
		.codepoint = cp,
		.fg = attr & 0x0f,
		.bg = (attr >> 4) & 0x0f,
		.attr = 0
	};
	photon_sdl_draw_cell(photon_ui_sdl(ui), col, row, &cell);
}

int photon_menu_put_str(photon_ui_t *ui, int col, int row,
                        const char *s, uint8_t attr)
{
	int c = col;
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		uint32_t cp;
		if      (*p < 0x80) { cp = *p++; }
		else if ((*p & 0xe0) == 0xc0) {
			cp = (uint32_t)(*p++ & 0x1f) << 6;
			if (*p) cp |= (*p++ & 0x3f);
		} else if ((*p & 0xf0) == 0xe0) {
			cp = (uint32_t)(*p++ & 0x0f) << 12;
			if (*p) cp |= (uint32_t)(*p++ & 0x3f) << 6;
			if (*p) cp |= (*p++ & 0x3f);
		} else { cp = '?'; p++; }
		photon_menu_put_cell(ui, c++, row, cp, attr);
	}
	return c - col;
}

void photon_menu_fill_rect(photon_ui_t *ui,
                           int c1, int r1, int c2, int r2, uint8_t attr)
{
	uint8_t fg = attr & 0x0f;
	uint8_t bg = (attr >> 4) & 0x0f;
	photon_sdl_fill_rect(photon_ui_sdl(ui), c1, r1, c2, r2, fg, bg);
}

void photon_menu_put_padded(photon_ui_t *ui, int col, int row,
                            const char *s, int width, uint8_t attr)
{
	char tmp[512];
	if (width > (int)sizeof(tmp) - 1) width = (int)sizeof(tmp) - 1;
	snprintf(tmp, sizeof(tmp), "%-*.*s", width, width, s ? s : "");
	photon_menu_put_str(ui, col, row, tmp, attr);
}

/* ── Status bar ──────────────────────────────────────────────────────── */

void photon_menu_draw_statusbar(photon_ui_t *ui)
{
	const photon_theme_t *t = photon_menu_theme();
	int W = photon_menu_cols(ui);
	int H = photon_menu_rows(ui);
	photon_menu_fill_rect(ui, 1, H, W, H, PHOTON_MENU_A_STATUS(t));
}

/* ── Title bar ───────────────────────────────────────────────────────── */

void photon_menu_draw_titlebar(photon_ui_t *ui, const char *title)
{
	const photon_theme_t *t = photon_menu_theme();
	int W = photon_menu_cols(ui);
	photon_menu_fill_rect(ui, 1, 1, W, 1, PHOTON_MENU_A_TITLE(t));
	if (title) {
		int len = (int)strlen(title);
		photon_menu_put_str(ui, (W - len) / 2, 1, title, PHOTON_MENU_A_TITLE(t));
	}
}

/* ── Key hints ───────────────────────────────────────────────────────── */

void photon_menu_draw_hints(photon_ui_t *ui, int row,
                            const photon_hint_t *hints, int nhints)
{
	const photon_theme_t *t = photon_menu_theme();
	int W = photon_menu_cols(ui);

	photon_menu_fill_rect(ui, 1, row, W, row, PHOTON_MENU_A_NORM(t));

	int total = 0;
	for (int i = 0; i < nhints; i++)
		total += (int)strlen(hints[i].key) + (int)strlen(hints[i].desc);

	int col = (W - total) / 2;
	if (col < 2) col = 2;

	for (int i = 0; i < nhints; i++) {
		col += photon_menu_put_str(ui, col, row, hints[i].key, PHOTON_MENU_A_HINT_K(t));
		col += photon_menu_put_str(ui, col, row, hints[i].desc, PHOTON_MENU_A_HINT(t));
	}
}

/* ── List menu ───────────────────────────────────────────────────────── */

int photon_menu_draw_list(photon_ui_t *ui,
                          const photon_menu_list_t *menu,
                          photon_menu_list_state_t *state)
{
	const photon_theme_t *t = photon_menu_theme();
	int W = photon_menu_cols(ui);
	int H = photon_menu_rows(ui);

	uint8_t bg = PHOTON_MENU_A_NORM(t);

	/* Clear area (leave status bar row) */
	photon_menu_fill_rect(ui, 1, 1, W, H - 1, bg);

	/* Title bar */
	photon_menu_draw_titlebar(ui, menu->title);

	/* Optional column header row */
	int list_top = 2;
	if (menu->header_rows > 0 && menu->draw_header) {
		menu->draw_header(ui, 2, W, t);
		list_top = 2 + menu->header_rows;
	}

	/* Hint row */
	int hint_row = H - 2;
	int list_bot = hint_row - 1;
	int visible = list_bot - list_top + 1;
	if (visible < 1) visible = 1;

	/* Clamp cursor/scroll */
	if (state->cursor < 0) state->cursor = 0;
	if (state->cursor >= menu->count) state->cursor = menu->count - 1;
	if (state->cursor < 0) state->cursor = 0;
	if (state->scroll > state->cursor) state->scroll = state->cursor;
	if (state->scroll + visible <= state->cursor)
		state->scroll = state->cursor - visible + 1;
	if (state->scroll < 0) state->scroll = 0;

	/* Empty state */
	if (menu->count == 0 && menu->empty_msg) {
		photon_menu_put_str(ui, 3, list_top, menu->empty_msg, PHOTON_MENU_A_DIM(t));
	}

	/* Draw items */
	for (int i = 0; i < visible && (state->scroll + i) < menu->count; i++) {
		int si = state->scroll + i;
		int row = list_top + i;
		bool selected = (si == state->cursor);
		menu->draw_item(ui, menu->items, si, row, 1, W, selected, t);
	}

	/* Hints */
	if (menu->hints && menu->nhints > 0)
		photon_menu_draw_hints(ui, hint_row, menu->hints, menu->nhints);

	/* Scroll indicator */
	if (menu->count > visible) {
		char info[32];
		snprintf(info, sizeof(info), " %d/%d ", state->cursor + 1, menu->count);
		photon_menu_put_str(ui, W - (int)strlen(info), hint_row,
		                    info, PHOTON_MENU_A_DIM(t));
	}

	/* Status bar */
	photon_menu_draw_statusbar(ui);

	return visible;
}

bool photon_menu_list_handle_nav(photon_key_t *key,
                                 photon_menu_list_state_t *state,
                                 int count, int visible)
{
	if (count <= 0) return false;

	switch (key->code) {
	case PHOTON_KEY_UP:
		if (state->cursor > 0) state->cursor--;
		return true;
	case PHOTON_KEY_DOWN:
		if (state->cursor < count - 1) state->cursor++;
		return true;
	case PHOTON_KEY_PGUP:
		state->cursor -= visible;
		if (state->cursor < 0) state->cursor = 0;
		return true;
	case PHOTON_KEY_PGDN:
		state->cursor += visible;
		if (state->cursor >= count) state->cursor = count - 1;
		return true;
	case PHOTON_KEY_HOME:
		state->cursor = 0;
		return true;
	case PHOTON_KEY_END:
		state->cursor = count - 1;
		return true;
	default:
		return false;
	}
}

/* ── Form menu ───────────────────────────────────────────────────────── */

int photon_menu_form_focusable_count(const photon_menu_form_t *form)
{
	int n = 0;
	for (int i = 0; i < form->nfields; i++) {
		if (form->fields[i].type != PHOTON_MENU_FIELD_SEPARATOR &&
		    form->fields[i].type != PHOTON_MENU_FIELD_READONLY)
			n++;
	}
	return n;
}

int photon_menu_form_focus_to_field(const photon_menu_form_t *form, int focus_idx)
{
	int n = 0;
	for (int i = 0; i < form->nfields; i++) {
		if (form->fields[i].type != PHOTON_MENU_FIELD_SEPARATOR &&
		    form->fields[i].type != PHOTON_MENU_FIELD_READONLY) {
			if (n == focus_idx) return i;
			n++;
		}
	}
	return -1;
}

int photon_menu_form_field_to_focus(const photon_menu_form_t *form, int field_idx)
{
	int n = 0;
	for (int i = 0; i < form->nfields; i++) {
		if (form->fields[i].type != PHOTON_MENU_FIELD_SEPARATOR &&
		    form->fields[i].type != PHOTON_MENU_FIELD_READONLY) {
			if (i == field_idx) return n;
			n++;
		}
	}
	return -1;
}

int photon_menu_draw_form(photon_ui_t *ui,
                          const photon_menu_form_t *form,
                          photon_menu_form_state_t *state)
{
	const photon_theme_t *t = photon_menu_theme();
	int W = photon_menu_cols(ui);
	int H = photon_menu_rows(ui);
	int mid = W / 2;

	uint8_t bg = PHOTON_MENU_A_NORM(t);

	/* Clear area */
	photon_menu_fill_rect(ui, 1, 1, W, H - 1, bg);

	/* Title bar */
	photon_menu_draw_titlebar(ui, form->title);

	/* Layout columns */
	int label_col = mid - 20;
	int value_col = mid - 4;
	if (label_col < 2) label_col = 2;
	if (value_col < 16) value_col = 16;

	/* Draw fields */
	int row = 4;
	int focus_idx = 0;
	int focusable_count = 0;

	for (int i = 0; i < form->nfields; i++) {
		photon_menu_field_t *f = &form->fields[i];

		if (f->type == PHOTON_MENU_FIELD_SEPARATOR) {
			row += 1;
			continue;
		}

		bool is_focusable = (f->type != PHOTON_MENU_FIELD_READONLY);
		bool focused = is_focusable && (focus_idx == state->field);

		if (f->type == PHOTON_MENU_FIELD_BUTTON) {
			uint8_t va;
			if (focused)
				va = PHOTON_MENU_A_SEL(t);
			else
				va = PHOTON_MENU_ATTR(t->hclr, t->bclr);

			const char *text = f->btn_text ? f->btn_text : "[ OK ]";
			int blen = (int)strlen(text);
			int bcol = mid - blen / 2;
			photon_menu_put_str(ui, bcol, row, text, va);
		} else {
			uint8_t la = PHOTON_MENU_A_DIM(t);
			uint8_t va;

			if (f->type == PHOTON_MENU_FIELD_READONLY)
				va = PHOTON_MENU_A_DIM(t);
			else if (focused)
				va = PHOTON_MENU_A_SEL(t);
			else
				va = PHOTON_MENU_A_NORM(t);

			if (f->label)
				photon_menu_put_str(ui, label_col, row, f->label, la);

			switch (f->type) {
			case PHOTON_MENU_FIELD_TEXT:
			case PHOTON_MENU_FIELD_READONLY: {
				const char *text = (f->buf && f->buf[0]) ? f->buf : "(none)";
				photon_menu_put_padded(ui, value_col, row, text,
				                       W - value_col - 1, va);
				break;
			}
			case PHOTON_MENU_FIELD_TOGGLE: {
				const char *text = (f->toggle && *f->toggle) ? "[X]" : "[ ]";
				photon_menu_put_str(ui, value_col, row, text, va);
				break;
			}
			case PHOTON_MENU_FIELD_ACTION: {
				const char *text = (f->buf && f->buf[0]) ? f->buf : "(none)";
				photon_menu_put_padded(ui, value_col, row, text,
				                       W - value_col - 1, va);
				break;
			}
			default:
				break;
			}

			/* Subtitle line */
			if (f->subtitle && f->subtitle[0] && row + 1 < H - 3) {
				photon_menu_put_padded(ui, value_col, row + 1,
				                       f->subtitle, W - value_col - 1,
				                       PHOTON_MENU_A_DIM(t));
				row++;
			}
		}

		row += 2;
		if (is_focusable) {
			focus_idx++;
			focusable_count++;
		}
	}

	/* Hints */
	int hint_row = H - 2;
	if (form->hints && form->nhints > 0)
		photon_menu_draw_hints(ui, hint_row, form->hints, form->nhints);

	/* Status bar */
	photon_menu_draw_statusbar(ui);

	return focusable_count;
}

bool photon_menu_form_handle_nav(photon_key_t *key,
                                 photon_menu_form_state_t *state,
                                 photon_menu_form_t *form)
{
	int count = photon_menu_form_focusable_count(form);
	if (count <= 0) return false;

	switch (key->code) {
	case PHOTON_KEY_UP:
		if (state->field > 0) state->field--;
		return true;
	case PHOTON_KEY_DOWN:
	case '\t':
		if (state->field < count - 1) state->field++;
		return true;
	case ' ': {
		int fi = photon_menu_form_focus_to_field(form, state->field);
		if (fi >= 0 && form->fields[fi].type == PHOTON_MENU_FIELD_TOGGLE &&
		    form->fields[fi].toggle) {
			*form->fields[fi].toggle = !*form->fields[fi].toggle;
			return true;
		}
		return false;
	}
	default:
		return false;
	}
}
