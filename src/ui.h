#ifndef UI_H
#define UI_H

#include "user.h"

/* The resource driven half of USER: string tables, bitmaps, icons and cursors, accelerators, menus, the
 * non-client area, dialogs and the built-in window classes, the clipboard, scroll bars and carets. It lives inside
 * user_t: user_create builds it, DefWindowProc and the window manager call into it. Drawing goes through the user_gfx
 * that GDI installs; without one the state is kept and nothing is painted. */

typedef struct ui ui_t;

ui_t *ui_create(user_t *u);
void ui_destroy(ui_t *ui);

/* Glue called by the window manager and DefWindowProc. ui_defproc handles what the UI owns (non-client area, system
 * commands, menus, dialogs, built-in classes) and returns false for everything else. */
bool ui_defproc(user_t *u, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp, uint32_t *ret);
uint16_t ui_class_menu(user_t *u, uint16_t hinst, uint32_t menu_name);
void ui_window_destroyed(user_t *u, uint16_t hwnd);
void ui_text_changed(user_t *u, uint16_t hwnd);

/* Size and hot spot of a cursor handle (LoadCursor), false when the handle is not a live cursor. */
bool ui_cursor_info(ui_t *ui, uint16_t hcursor, int *w, int *h, int *hot_x, int *hot_y);

#endif
