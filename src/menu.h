/* Context menu contents, computed from a selection snapshot. Pure: no GTK, so it's tested
 * directly. The window turns entries into a GtkMenu and performs them.
 *
 * On items:      Open, Open With (same-type files), Execute / Execute in Terminal (one
 *                runnable file), Open Terminal Here (one folder), clipboard and file
 *                operations, Copy Full Path, then configured actions that match every item.
 * On empty space (the current directory as a folder): Paste, Copy Full Path, Open Terminal
 *                Here, actions that accept directories, then Sort by Name / Modified and
 *                Show Hidden Files. */
#pragma once

#include "open.h"

typedef enum
{
  GBK_MENU_SEPARATOR,
  GBK_MENU_OPEN,
  GBK_MENU_OPEN_WITH,          /* submenu: the window lists applications for `mime` */
  GBK_MENU_EXECUTE,
  GBK_MENU_EXECUTE_TERMINAL,
  GBK_MENU_TERMINAL_HERE,
  GBK_MENU_ACTION,             /* `action` */
  GBK_MENU_CUT,
  GBK_MENU_COPY,
  GBK_MENU_COPY_PATH,          /* the items' full paths as text, one per line */
  GBK_MENU_PASTE,              /* into the current directory (background menu) */
  GBK_MENU_PASTE_INTO,         /* into the one selected folder */
  GBK_MENU_TRASH,
  GBK_MENU_DELETE,
  GBK_MENU_RENAME,
  GBK_MENU_SORT_NAME,
  GBK_MENU_SORT_MTIME,
  GBK_MENU_SHOW_HIDDEN,
} GbkMenuKind;

typedef struct
{
  GbkMenuKind kind;
  GbkAction *action;           /* GBK_MENU_ACTION; borrowed from the config */
} GbkMenuEntry;

/* `items` is the selection, or the current directory alone when `background`.
 * Returns GbkMenuEntry elements; never starts/ends with or doubles a separator. */
[[nodiscard]] GArray *gbk_menu_entries (const GbkConfig *config, GPtrArray *items,
                                        bool background);

/* The common MIME type of all items, or nullptr if they differ or include folders. */
const char *gbk_menu_common_mime (GPtrArray *items);
