/* Context menu contents, computed from a selection snapshot. Pure: no GTK, so it's tested
 * directly. The window turns entries into a GtkMenu and performs them.
 *
 * On items:      Open, Open in New Tab / Window (all folders), Open With (same-type files),
 *                Execute / Execute in Terminal (one runnable file), Open Terminal Here (one
 *                folder), clipboard and file operations, Copy Full Path, then configured
 *                actions that match every item.
 * On empty space (the current directory as a folder): Paste, Copy Full Path, Open Terminal
 *                Here, actions that accept directories, then the sort modes and Show Hidden
 *                Files.
 * On a path bar button (one folder, an ancestor of or the current one): opening, Copy,
 *                Paste Into Folder, Copy Full Path and actions; nothing that moves, renames
 *                or deletes it, since the tab is inside it. */
#pragma once

#include "open.h"

typedef enum
{
  GBK_MENU_SEPARATOR,
  GBK_MENU_OPEN,
  GBK_MENU_OPEN_TAB,           /* folders, each in a new tab */
  GBK_MENU_OPEN_WINDOW,        /* folders, each in a new window */
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
  GBK_MENU_SORT_SIZE,
  GBK_MENU_SHOW_HIDDEN,
} GbkMenuKind;

typedef struct
{
  GbkMenuKind kind;
  GbkAction *action;           /* GBK_MENU_ACTION; borrowed from the config */
} GbkMenuEntry;

typedef enum
{
  GBK_MENU_ON_ITEMS,           /* the selection */
  GBK_MENU_ON_BACKGROUND,      /* empty space: `items` is the current directory alone */
  GBK_MENU_ON_PATHBAR,         /* a path bar button: `items` is that directory alone */
} GbkMenuTarget;

/* Returns GbkMenuEntry elements; never starts/ends with or doubles a separator. */
[[nodiscard]] GArray *gbk_menu_entries (const GbkConfig *config, GPtrArray *items,
                                        GbkMenuTarget target);

/* The common MIME type of all items, or nullptr if they differ or include folders. */
const char *gbk_menu_common_mime (GPtrArray *items);
