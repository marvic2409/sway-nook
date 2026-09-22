#include "nook.h"
#include <glib/gstdio.h>
#include <unistd.h>

static JsonNode *parse(const char *text) {
    JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));
    JsonNode *node = json_node_copy(json_parser_get_root(parser));
    g_object_unref(parser);
    return node;
}

static void discovery(void) {
    JsonNode *tree = parse("{\"id\":0,\"nodes\":[{\"type\":\"workspace\",\"name\":\"__i3_scratch\",\"nodes\":[],\"floating_nodes\":[{\"id\":7,\"type\":\"con\",\"focus\":[9,8],\"nodes\":[{\"id\":8,\"type\":\"con\",\"name\":\"First\",\"app_id\":\"app\"},{\"id\":9,\"type\":\"con\",\"name\":\"Second\",\"app_id\":\"app\",\"foreign_toplevel_identifier\":\"capture-9\"}]}]}]}");
    GPtrArray *windows = nook_hidden_windows(tree);
    g_assert_cmpuint(windows->len, ==, 1);
    NookWindow *window = g_ptr_array_index(windows, 0);
    g_assert_cmpint(window->id, ==, 7);
    g_assert_cmpint(window->count, ==, 2);
    g_assert_cmpstr(window->title, ==, "Second");
    g_assert_cmpstr(window->capture_id, ==, "capture-9");
    GHashTable *hidden_ids = nook_hidden_ids(tree);
    g_assert_true(g_hash_table_contains(hidden_ids, "7"));
    g_assert_true(g_hash_table_contains(hidden_ids, "9"));
    g_assert_false(g_hash_table_contains(hidden_ids, "0"));
    g_hash_table_unref(hidden_ids);
    g_ptr_array_unref(windows);
    json_node_free(tree);
}

static void focused_and_hidden(void) {
    JsonNode *tree = parse("{\"id\":0,\"nodes\":[{\"id\":3,\"type\":\"con\",\"focused\":true,\"name\":\"Text \\\" ]; exec evil\",\"app_id\":\"app\"},{\"type\":\"workspace\",\"name\":\"__i3_scratch\",\"nodes\":[],\"floating_nodes\":[]}]}");
    NookWindow *window = nook_focused_window(tree);
    g_assert_nonnull(window);
    g_assert_cmpint(window->id, ==, 3);
    g_assert_true(nook_tree_has_id(tree, 3));
    g_assert_false(nook_tree_has_id(tree, 4));
    nook_window_free(window);
    json_node_free(tree);
}

static void focused_floating_group_uses_outer_id(void) {
    JsonNode *tree = parse("{\"type\":\"workspace\",\"floating_nodes\":[{\"id\":7,\"type\":\"floating_con\",\"focus\":[9],\"nodes\":[{\"id\":9,\"type\":\"con\",\"focused\":true,\"name\":\"Notes\",\"app_id\":\"app\",\"foreign_toplevel_identifier\":\"capture-9\"}]}]}");
    NookWindow *window = nook_focused_window(tree);
    g_assert_nonnull(window);
    g_assert_cmpint(window->id, ==, 7);
    g_assert_cmpstr(window->capture_id, ==, "capture-9");
    nook_window_free(window);
    json_node_free(tree);
}

static void theme_defaults(void) {
    NookTheme *theme = nook_theme_load("/nonexistent/nook.ini", NULL);
    g_assert_nonnull(theme);
    g_assert_cmpfloat(theme->panel_opacity, <, 1.0);
    char *css = nook_theme_css(theme);
    g_assert_nonnull(strstr(css, "@define-color nook_panel rgba("));
    g_assert_nonnull(strstr(css, "@define-color nook_preview #090909;"));
    g_free(css);
    nook_theme_free(theme);
}

static void theme_override(void) {
    char *path = g_strdup_printf("%s/nook-theme-XXXXXX", g_get_tmp_dir());
    int fd = g_mkstemp(path);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_true(g_file_set_contents(path, "[theme]\naccent=#aabbcc\npanel_opacity=0.5\n", -1, NULL));
    NookTheme *theme = nook_theme_load(path, NULL);
    g_assert_nonnull(theme);
    g_assert_cmpstr(theme->accent, ==, "#aabbcc");
    g_assert_cmpfloat(theme->panel_opacity, ==, 0.5);
    char *css = nook_theme_css(theme);
    g_assert_nonnull(strstr(css, "nook_accent_soft rgba(170, 187, 204, 0.100)"));
    g_free(css);
    nook_theme_free(theme);
    g_unlink(path);
    g_free(path);
}

static void theme_rejects_invalid(void) {
    char *path = g_strdup_printf("%s/nook-theme-XXXXXX", g_get_tmp_dir());
    int fd = g_mkstemp(path);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_true(g_file_set_contents(path, "[theme]\naccent=red;bad\n", -1, NULL));
    GError *error = NULL;
    g_assert_null(nook_theme_load(path, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_unlink(path);
    g_free(path);
}

static void theme_rejects_typo(void) {
    char *path = g_strdup_printf("%s/nook-theme-XXXXXX", g_get_tmp_dir());
    int fd = g_mkstemp(path);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_true(g_file_set_contents(path, "[theme]\naccnt=#aabbcc\n", -1, NULL));
    GError *error = NULL;
    g_assert_null(nook_theme_load(path, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_unlink(path);
    g_free(path);
}

static void snapshot_rejects_symlink_without_writing_through_it(void) {
    char *root = g_dir_make_tmp("nook-test-XXXXXX", NULL);
    g_assert_nonnull(root);
    char *runtime = g_build_filename(root, "runtime", NULL);
    char *outside = g_build_filename(root, "outside", NULL);
    g_assert_cmpint(g_mkdir(runtime, 0700), ==, 0);
    g_assert_cmpint(g_mkdir(outside, 0700), ==, 0);
    char *base = g_strdup_printf("%s/sway-nook-%u", runtime, (guint)getuid());
    GFile *link = g_file_new_for_path(base);
    g_assert_true(g_file_make_symbolic_link(link, outside, NULL, NULL));
    g_object_unref(link);
    const char *old_runtime = g_getenv("XDG_RUNTIME_DIR");
    char *saved = g_strdup(old_runtime);
    g_setenv("XDG_RUNTIME_DIR", runtime, TRUE);
    GError *error = NULL;
    g_assert_null(nook_snapshot_dir("/test-sway.sock", &error));
    g_assert_nonnull(error);
    GDir *folder = g_dir_open(outside, 0, NULL);
    g_assert_null(g_dir_read_name(folder));
    g_dir_close(folder);
    g_clear_error(&error);
    if (saved) g_setenv("XDG_RUNTIME_DIR", saved, TRUE);
    else g_unsetenv("XDG_RUNTIME_DIR");
    g_unlink(base);
    g_rmdir(outside);
    g_rmdir(runtime);
    g_rmdir(root);
    g_free(saved);
    g_free(base);
    g_free(outside);
    g_free(runtime);
    g_free(root);
}

static void snapshot_capture_times_out(void) {
    char *dir = g_dir_make_tmp("nook-grim-XXXXXX", NULL);
    g_assert_nonnull(dir);
    char *grim = g_build_filename(dir, "grim", NULL);
    g_assert_true(g_file_set_contents(grim, "#!/bin/sh\nexec sleep 5\n", -1, NULL));
    g_assert_cmpint(g_chmod(grim, 0700), ==, 0);
    char *old_path = g_strdup(g_getenv("PATH"));
    char *path = g_strdup_printf("%s:%s", dir, old_path ? old_path : "");
    g_setenv("PATH", path, TRUE);
    NookWindow window = {.id = 5, .capture_id = "fake"};
    gint64 start = g_get_monotonic_time();
    g_assert_null(nook_capture(dir, &window, FALSE));
    g_assert_cmpint(g_get_monotonic_time() - start, <, 2500000);
    if (old_path) g_setenv("PATH", old_path, TRUE);
    else g_unsetenv("PATH");
    g_unlink(grim);
    g_rmdir(dir);
    g_free(old_path);
    g_free(path);
    g_free(grim);
    g_free(dir);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/model/discovery", discovery);
    g_test_add_func("/model/focused", focused_and_hidden);
    g_test_add_func("/model/floating-group", focused_floating_group_uses_outer_id);
    g_test_add_func("/theme/defaults", theme_defaults);
    g_test_add_func("/theme/override", theme_override);
    g_test_add_func("/theme/invalid", theme_rejects_invalid);
    g_test_add_func("/theme/typo", theme_rejects_typo);
    g_test_add_func("/snapshot/symlink", snapshot_rejects_symlink_without_writing_through_it);
    g_test_add_func("/snapshot/timeout", snapshot_capture_times_out);
    return g_test_run();
}
