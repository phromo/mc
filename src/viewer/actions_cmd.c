/*
   Internal file viewer for the Midnight Commander
   Callback function for some actions (hotkeys, menu)

   Copyright (C) 1994, 1995, 1996, 1998, 1999, 2000, 2001, 2002, 2003,
   2004, 2005, 2006, 2007, 2009 Free Software Foundation, Inc.

   Written by: 1994, 1995, 1998 Miguel de Icaza
	       1994, 1995 Janne Kukonlehto
	       1995 Jakub Jelinek
	       1996 Joseph M. Hinkle
	       1997 Norbert Warmuth
	       1998 Pavel Machek
	       2004 Roland Illig <roland.illig@gmx.de>
	       2005 Roland Illig <roland.illig@gmx.de>
	       2009 Slava Zanko <slavazanko@google.com>
	       2009 Andrew Borodin <aborodin@vmail.ru>
	       2009 Ilia Maslakov <il.smind@gmail.com>

   This file is part of the Midnight Commander.

   The Midnight Commander is free software; you can redistribute it
   and/or modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Midnight Commander is distributed in the hope that it will be
   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA.
 */

/*
    The functions in this section can be bound to hotkeys. They are all
    of the same type (taking a pointer to mcview_t as parameter and
    returning void). TODO: In the not-too-distant future, these commands
    will become fully configurable, like they already are in the
    internal editor. By convention, all the function names end in
    "_cmd".
 */

#include <config.h>

#include <errno.h>
#include <stdlib.h>

#include "../src/global.h"

#include "../src/tty/tty.h"
#include "../src/tty/key.h"

#include "../src/dialog.h"	/* cb_ret_t */
#include "../src/panel.h"
#include "../src/layout.h"
#include "../src/wtools.h"
#include "../src/history.h"
#include "../src/charsets.h"
#include "../src/cmd.h"
#include "../src/execute.h"
#include "../src/help.h"
#include "../src/keybind.h"	/* global_key_map_t */
#include "../src/cmddef.h"	/* CK_ cmd name const */


#include "internal.h"
#include "mcviewer.h"

/*** global variables ****************************************************************************/

/*** file scope macro definitions ****************************************************************/

/*** file scope type declarations ****************************************************************/

/*** file scope variables ************************************************************************/

/*** file scope functions ************************************************************************/

/* --------------------------------------------------------------------------------------------- */

static void
mcview_continue_search_cmd (mcview_t * view)
{

    if (view->last_search_string != NULL) {
        mcview_do_search (view);
    } else {
        /* find last search string in history */
        GList *history;
        history = history_get (MC_HISTORY_SHARED_SEARCH);
        if (history != NULL && history->data != NULL) {
            view->last_search_string = (gchar *) g_strdup(history->data);
            history = g_list_first (history);
            g_list_foreach (history, (GFunc) g_free, NULL);
            g_list_free (history);

            view->search = mc_search_new (view->last_search_string, -1);
            view->search_nroff_seq = mcview_nroff_seq_new (view);

            if (!view->search) {
                /* if not... then ask for an expression */
                g_free(view->last_search_string);
                mcview_search_cmd (view);
            } else {
                view->search->search_type = view->search_type;
                view->search->is_all_charsets = view->search_all_codepages;
                view->search->is_case_sentitive = view->search_case;
                view->search->search_fn = mcview_search_cmd_callback;
                view->search->update_fn = mcview_search_update_cmd_callback;
                view->search->whole_words = view->whole_words;

                mcview_do_search (view);
            }
        } else {
            /* if not... then ask for an expression */
            g_free(view->last_search_string);
            mcview_search_cmd (view);
        }
    }
}

/* --------------------------------------------------------------------------------------------- */
static cb_ret_t
mcview_handle_editkey (mcview_t * view, int key)
{
    struct hexedit_change_node *node;
    int byte_val;

    /* Has there been a change at this position? */
    node = view->change_list;
    while (node && (node->offset != view->hex_cursor))
        node = node->next;

    if (!view->hexview_in_text) {
        /* Hex editing */
        unsigned int hexvalue = 0;

        if (key >= '0' && key <= '9')
            hexvalue = 0 + (key - '0');
        else if (key >= 'A' && key <= 'F')
            hexvalue = 10 + (key - 'A');
        else if (key >= 'a' && key <= 'f')
            hexvalue = 10 + (key - 'a');
        else
            return MSG_NOT_HANDLED;

        if (node)
            byte_val = node->value;
        else
            mcview_get_byte (view, view->hex_cursor, &byte_val);

        if (view->hexedit_lownibble) {
            byte_val = (byte_val & 0xf0) | (hexvalue);
        } else {
            byte_val = (byte_val & 0x0f) | (hexvalue << 4);
        }
    } else {
        /* Text editing */
        if (key < 256 && ((key == '\n') || is_printable (key)))
            byte_val = key;
        else
            return MSG_NOT_HANDLED;
    }
    if (!node) {
        node = g_new (struct hexedit_change_node, 1);
        node->offset = view->hex_cursor;
        node->value = byte_val;
        mcview_enqueue_change (&view->change_list, node);
    } else {
        node->value = byte_val;
    }
    view->dirty++;
    mcview_update (view);
    mcview_move_right (view, 1);
    return MSG_HANDLED;
}

/* --------------------------------------------------------------------------------------------- */

/* Check for left and right arrows, possibly with modifiers */
static cb_ret_t
mcview_check_left_right_keys (mcview_t * view, int c)
{
    if (c == KEY_LEFT) {
        mcview_move_left (view, 1);
        return MSG_HANDLED;
    }

    if (c == KEY_RIGHT) {
        mcview_move_right (view, 1);
        return MSG_HANDLED;
    }

    /* Ctrl with arrows moves by 10 postions in the unwrap mode */
    if (view->hex_mode || view->text_wrap_mode)
        return MSG_NOT_HANDLED;

    if (c == (KEY_M_CTRL | KEY_LEFT)) {
        if (view->dpy_text_column >= 10)
            view->dpy_text_column -= 10;
        else
            view->dpy_text_column = 0;
        view->dirty++;
        return MSG_HANDLED;
    }

    if (c == (KEY_M_CTRL | KEY_RIGHT)) {
        if (view->dpy_text_column <= OFFSETTYPE_MAX - 10)
            view->dpy_text_column += 10;
        else
            view->dpy_text_column = OFFSETTYPE_MAX;
        view->dirty++;
        return MSG_HANDLED;
    }

    return MSG_NOT_HANDLED;
}

/* --------------------------------------------------------------------------------------------- */

static void
mcview_cmk_move_up (void *w, int n)
{
    mcview_move_up ((mcview_t *) w, n);
}

/* --------------------------------------------------------------------------------------------- */

static void
mcview_cmk_move_down (void *w, int n)
{
    mcview_move_down ((mcview_t *) w, n);
}

/* --------------------------------------------------------------------------------------------- */

static void
mcview_cmk_moveto_top (void *w, int n)
{
    (void) &n;
    mcview_moveto_top ((mcview_t *) w);
}

/* --------------------------------------------------------------------------------------------- */

static void
mcview_cmk_moveto_bottom (void *w, int n)
{
    (void) &n;
    mcview_moveto_bottom ((mcview_t *) w);
}

/* --------------------------------------------------------------------------------------------- */

static void
mcview_hook (void *v)
{
    mcview_t *view = (mcview_t *) v;
    WPanel *panel;

    /* If the user is busy typing, wait until he finishes to update the
       screen */
    if (!is_idle ()) {
        if (!hook_present (idle_hook, mcview_hook))
            add_hook (&idle_hook, mcview_hook, v);
        return;
    }

    delete_hook (&idle_hook, mcview_hook);

    if (get_current_type () == view_listing)
        panel = current_panel;
    else if (get_other_type () == view_listing)
        panel = other_panel;
    else
        return;

    mcview_load (view, 0, panel->dir.list[panel->selected].fname, 0);
    mcview_display (view);
}

/* --------------------------------------------------------------------------------------------- */

static cb_ret_t
mcview_execute_cmd (mcview_t * view, int command, int key)
{
    int res = MSG_HANDLED;

    if (view->hex_mode) {
        switch (command) {
        case CK_HexViewToggleNavigationMode:
            view->hexview_in_text = !view->hexview_in_text;
            view->dirty++;
            break;
        case CK_ViewMoveToBol:
            mcview_moveto_bol (view);
            view->dirty++;
            break;
        case CK_ViewMoveToEol:
            mcview_moveto_eol (view);
            break;
        case CK_ViewMoveLeft:
            mcview_move_left (view, 1);
            break;
        case CK_ViewMoveRight:
            mcview_move_right (view, 1);
            break;
        default :
            res = MSG_NOT_HANDLED;
            break;
        }
    } else {
        switch (command) {
        case CK_ViewSearch:
            view->search_type = MC_SEARCH_T_REGEX;
            mcview_search_cmd (view);
            break;
            /* Continue search */
        case CK_ViewContinueSearch:
            mcview_continue_search_cmd (view);
            break;
            /* toggle ruler */
        case CK_ViewToggleRuler:
            mcview_toggle_ruler_cmd (view);
            break;
        case CK_ViewMoveLeft:
            mcview_move_left (view, 1);
            break;
        case CK_ViewMoveRight:
            mcview_move_right (view, 1);
            break;
        case CK_ViewMoveUp:
            mcview_move_up (view, 1);
            break;
        case CK_ViewMoveDown:
            mcview_move_down (view, 1);
            break;
        case CK_ViewMoveHalfPgUp:
            mcview_move_up (view, (view->data_area.height + 1) / 2);
            break;
        case CK_ViewMoveHalfPgDn:
            mcview_move_down (view, (view->data_area.height + 1) / 2);
            break;
        case CK_ViewMovePgUp:
            mcview_move_up (view, view->data_area.height);
            break;
        case CK_ViewMovePgDn:
            mcview_move_down (view, view->data_area.height);
            break;
        case CK_ShowCommandLine:
            view_other_cmd ();
            break;
        /*
            // Unlike Ctrl-O, run a new shell if the subshell is not running
        case '!':
            exec_shell ();
            return MSG_HANDLED;
        */
        case CK_ViewGotoBookmark:
            view->marks[view->marker] = view->dpy_start;
            break;
        case CK_ViewNewBookmark:
            view->dpy_start = view->marks[view->marker];
            view->dirty++;
            break;
        case CK_ViewNextFile:
            /*  Use to indicate parent that we want to see the next/previous file */
            /* Does not work in panel mode */
            if (!mcview_is_in_panel (view))
                view->move_dir = 1;
            break;
        case CK_ViewPrevFile:
            /*  Use to indicate parent that we want to see the next/previous file */
            /* Does not work in panel mode */
            if (!mcview_is_in_panel (view))
                view->move_dir = -1;
            break;
        case CK_ViewQuit:
            if (mcview_ok_to_quit (view))
                view->want_to_quit = TRUE;
            break;
        case CK_SelectCodepage:
            mcview_select_encoding (view);
            view->dirty++;
            mcview_update (view);
            break;
        default :
            res = MSG_NOT_HANDLED;
        }
    }
    return res;
}


/* Both views */
static cb_ret_t
mcview_handle_key (mcview_t * view, int key)
{
    key = convert_from_input_c (key);

    int i;

    if (view->hex_mode) {
        for (i = 0; view->hex_map[i].key != 0; i++)
            if ((key == view->hex_map[i].key)
                && (mcview_execute_cmd (view, view->hex_map[i].command,
                                        key) == MSG_HANDLED))
                    return MSG_HANDLED;
    } else {
        for (i = 0; view->plain_map[i].key != 0; i++)
            if ((key == view->plain_map[i].key)
                && (mcview_execute_cmd (view, view->plain_map[i].command,
                                        key) == MSG_HANDLED))
                    return MSG_HANDLED;
    }
    if (mcview_check_left_right_keys (view, key))
        return MSG_HANDLED;

    if (check_movement_keys (key, view->data_area.height + 1, view,
                             mcview_cmk_move_up, mcview_cmk_move_down,
                             mcview_cmk_moveto_top, mcview_cmk_moveto_bottom))
        return MSG_HANDLED;

#ifdef MC_ENABLE_DEBUGGING_CODE
    if (c == 't') {                  /* mnemonic: "test" */
        mcview_ccache_dump (view);
        return MSG_HANDLED;
    }
#endif
    if (key >= '0' && key <= '9')
        view->marker = key - '0';

    /* Key not used */
    return MSG_NOT_HANDLED;
}

/* --------------------------------------------------------------------------------------------- */

/*** public functions ****************************************************************************/

/* --------------------------------------------------------------------------------------------- */

cb_ret_t
mcview_callback (Widget * w, widget_msg_t msg, int parm)
{
    mcview_t *view = (mcview_t *) w;
    cb_ret_t i;
    Dlg_head *h = view->widget.parent;

    mcview_compute_areas (view);
    mcview_update_bytes_per_line (view);

    switch (msg) {
    case WIDGET_INIT:
        if (mcview_is_in_panel (view))
            add_hook (&select_file_hook, mcview_hook, view);
        else
            view->dpy_bbar_dirty = TRUE;
        return MSG_HANDLED;

    case WIDGET_DRAW:
        mcview_display (view);
        return MSG_HANDLED;

    case WIDGET_CURSOR:
        if (view->hex_mode)
            mcview_place_cursor (view);
        return MSG_HANDLED;

    case WIDGET_KEY:
        i = mcview_handle_key ((mcview_t *) view, parm);
        if (view->want_to_quit && !mcview_is_in_panel (view))
            dlg_stop (h);
        else {
            mcview_update (view);
        }
        return i;

    case WIDGET_FOCUS:
        view->dpy_bbar_dirty = TRUE;
        mcview_update (view);
        return MSG_HANDLED;

    case WIDGET_DESTROY:
        mcview_done (view);
        if (mcview_is_in_panel (view))
            delete_hook (&select_file_hook, mcview_hook);
        return MSG_HANDLED;

    default:
        return default_proc (msg, parm);
    }
}

/* --------------------------------------------------------------------------------------------- */

cb_ret_t
mcview_dialog_callback (Dlg_head * h, dlg_msg_t msg, int parm)
{
    switch (msg) {
    case DLG_RESIZE:
        mcview_adjust_size (h);
        return MSG_HANDLED;

    default:
        return default_dlg_callback (h, msg, parm);
    }
}

/* --------------------------------------------------------------------------------------------- */

void
mcview_help_cmd (void)
{
    interactive_display (NULL, "[Internal File Viewer]");
}

/* --------------------------------------------------------------------------------------------- */

void
mcview_quit_cmd (mcview_t * view)
{
    if (mcview_ok_to_quit (view))
        dlg_stop (view->widget.parent);
}

/* --------------------------------------------------------------------------------------------- */

/* Toggle between hex view and text view */
void
mcview_toggle_hex_mode_cmd (mcview_t * view)
{
    mcview_toggle_hex_mode (view);
    mcview_update (view);
}

/* --------------------------------------------------------------------------------------------- */

void
mcview_moveto_line_cmd (mcview_t * view)
{
    char *answer, *answer_end, prompt[BUF_SMALL];
    off_t line, col;

    mcview_offset_to_coord (view, &line, &col, view->dpy_start);

    g_snprintf (prompt, sizeof (prompt),
                _(" The current line number is %lld.\n"
                  " Enter the new line number:"), (line + 1));
    answer = input_dialog (_(" Goto line "), prompt, MC_HISTORY_VIEW_GOTO_LINE, "");
    if (answer != NULL && answer[0] != '\0') {
        errno = 0;
        line = strtoul (answer, &answer_end, 10);
        if (*answer_end == '\0' && errno == 0 && line >= 1)
            mcview_moveto (view, line - 1, 0);
    }
    g_free (answer);
    view->dirty++;
    mcview_update (view);
}

/* --------------------------------------------------------------------------------------------- */

void
mcview_moveto_addr_cmd (mcview_t * view)
{
    char *line, *error, prompt[BUF_SMALL], prompt_format[BUF_SMALL];
    off_t addr;

    g_snprintf (prompt_format, sizeof (prompt_format),
                _(" The current address is %s.\n"
                  " Enter the new address:"), "0x%08" OFFSETTYPE_PRIX "");
    g_snprintf (prompt, sizeof (prompt), prompt_format, view->hex_cursor);
    line = input_dialog (_(" Goto Address "), prompt, MC_HISTORY_VIEW_GOTO_ADDR, "");
    if (line != NULL) {
        if (*line != '\0') {
            addr = strtoul (line, &error, 0);
            if ((*error == '\0') && mcview_get_byte (view, addr, NULL) == TRUE) {
                mcview_moveto_offset (view, addr);
            } else {
                message (D_ERROR, _("Warning"), _(" Invalid address "));
            }
        }
        g_free (line);
    }
    view->dirty++;
    mcview_update (view);
}

/* --------------------------------------------------------------------------------------------- */

/* Toggle between hexview and hexedit mode */
void
mcview_toggle_hexedit_mode_cmd (mcview_t * view)
{
    mcview_toggle_hexedit_mode (view);
    mcview_update (view);
}

/* --------------------------------------------------------------------------------------------- */

void
mcview_hexedit_save_changes_cmd (mcview_t * view)
{
    (void) mcview_hexedit_save_changes (view);
}

/* --------------------------------------------------------------------------------------------- */

/* Toggle between wrapped and unwrapped view */
void
mcview_toggle_wrap_mode_cmd (mcview_t * view)
{
    mcview_toggle_wrap_mode (view);
    mcview_update (view);
}

/* --------------------------------------------------------------------------------------------- */

/* Both views */
void
mcview_search_cmd (mcview_t * view)
{
    if (mcview_dialog_search (view))
        mcview_do_search (view);
}

/* --------------------------------------------------------------------------------------------- */

void
mcview_toggle_magic_mode_cmd (mcview_t * view)
{
    mcview_toggle_magic_mode (view);
    mcview_update (view);
}

/* --------------------------------------------------------------------------------------------- */

void
mcview_toggle_nroff_mode_cmd (mcview_t * view)
{
    mcview_toggle_nroff_mode (view);
    mcview_update (view);
}

/* --------------------------------------------------------------------------------------------- */

void
mcview_toggle_ruler_cmd (mcview_t * view)
{
    mcview_display_toggle_ruler (view);
}

/* --------------------------------------------------------------------------------------------- */