#pragma once

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

typedef struct {
    gint64 id;
    char *title;
    char *app_id;
    char *capture_id;
    gint count;
    gboolean visible;
    gint x, y, width, height;
} NookWindow;

typedef struct {
    char *panel;
    char *card;
    char *card_hover;
    char *border;
    char *accent;
    char *text;
    char *muted;
    double panel_opacity;
    double card_opacity;
    double card_hover_opacity;
} NookTheme;

void nook_window_free(gpointer data);
GPtrArray *nook_hidden_windows(JsonNode *tree);
NookWindow *nook_focused_window(JsonNode *tree);
gboolean nook_tree_has_id(JsonNode *tree, gint64 id);
GHashTable *nook_tree_ids(JsonNode *tree);
GHashTable *nook_hidden_ids(JsonNode *tree);

NookTheme *nook_theme_load(const char *path, GError **error);
void nook_theme_free(NookTheme *theme);
char *nook_theme_css(const NookTheme *theme);

JsonNode *nook_ipc_request(const char *socket_path, guint32 type,
                           const char *payload, GError **error);
GSocket *nook_ipc_subscribe(const char *socket_path, GError **error);

char *nook_snapshot_dir(const char *socket_path, GError **error);
char *nook_cached_preview(const char *dir, gint64 id);
char *nook_capture(const char *dir, const NookWindow *window, gboolean allow_region);
void nook_snapshot_cleanup(const char *dir, GHashTable *live_ids);

gboolean nook_sway_command(const char *socket_path, const char *command, GError **error);
int nook_cli_main(const char *socket_path, const char *cache_dir,
                  int argc, char **argv);
int nook_ui_main(const char *socket_path, const char *cache_dir,
                 int argc, char **argv);
