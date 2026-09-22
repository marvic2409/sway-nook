#include "nook.h"
#include <errno.h>

gboolean nook_sway_command(const char *socket_path, const char *command, GError **error) {
    JsonNode *reply = nook_ipc_request(socket_path, 0, command, error);
    if (!reply) return FALSE;
    gboolean ok = JSON_NODE_HOLDS_ARRAY(reply);
    if (ok) {
        JsonArray *results = json_node_get_array(reply);
        ok = json_array_get_length(results) > 0;
        for (guint i = 0; ok && i < json_array_get_length(results); i++) {
            JsonObject *result = json_array_get_object_element(results, i);
            ok = result && json_object_get_boolean_member_with_default(result, "success", FALSE);
            if (!ok && result) {
                const char *message = json_object_get_string_member_with_default(result,
                    "error", "Sway rejected the command");
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
            }
        }
    }
    if (!ok && (!error || !*error))
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Sway rejected the command");
    json_node_free(reply);
    return ok;
}

static void print_window_list(GPtrArray *windows) {
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_array(builder);
    for (guint i = 0; i < windows->len; i++) {
        NookWindow *window = g_ptr_array_index(windows, i);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_int_value(builder, window->id);
        json_builder_set_member_name(builder, "title");
        json_builder_add_string_value(builder, window->title);
        json_builder_set_member_name(builder, "app_id");
        json_builder_add_string_value(builder, window->app_id);
        json_builder_set_member_name(builder, "window_count");
        json_builder_add_int_value(builder, window->count);
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);
    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, root);
    char *text = json_generator_to_data(generator, NULL);
    g_print("%s\n", text);
    g_free(text);
    g_object_unref(generator);
    json_node_free(root);
    g_object_unref(builder);
}

int nook_cli_main(const char *socket_path, const char *cache_dir,
                  int argc, char **argv) {
    const char *action = argc > 1 ? argv[1] : "toggle";
    GError *error = NULL;
    JsonNode *tree = nook_ipc_request(socket_path, 4, NULL, &error);
    if (!tree) goto failed;
    int result = 0;
    if (!g_strcmp0(action, "list")) {
        GPtrArray *windows = nook_hidden_windows(tree);
        print_window_list(windows);
        g_ptr_array_unref(windows);
    } else if (!g_strcmp0(action, "stash")) {
        NookWindow *window = nook_focused_window(tree);
        if (!window) {
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Focus an application window before stashing it");
        } else {
            char *preview = nook_capture(cache_dir, window, TRUE);
            g_free(preview);
            char *command = g_strdup_printf("[con_id=%" G_GINT64_FORMAT "] move scratchpad", window->id);
            if (!nook_sway_command(socket_path, command, &error)) result = 1;
            g_free(command);
            nook_window_free(window);
        }
    } else if (!g_strcmp0(action, "restore")) {
        char *end = NULL;
        errno = 0;
        gint64 id = argc > 2 ? g_ascii_strtoll(argv[2], &end, 10) : 0;
        if (argc != 3 || errno || !end || *end || id <= 0) {
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "restore requires a positive numeric container ID");
        } else {
            GPtrArray *windows = nook_hidden_windows(tree);
            gboolean found = FALSE;
            for (guint i = 0; i < windows->len; i++)
                if (((NookWindow *)g_ptr_array_index(windows, i))->id == id) found = TRUE;
            g_ptr_array_unref(windows);
            if (!found) {
                g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "That window has already been shown or closed");
            } else {
                char *command = g_strdup_printf("[con_id=%" G_GINT64_FORMAT "] scratchpad show", id);
                if (!nook_sway_command(socket_path, command, &error)) result = 1;
                g_free(command);
            }
        }
    }
    json_node_free(tree);
    if (!error) return result;
failed:
    g_printerr("sway-nook: %s\n", error ? error->message : "Sway is unavailable");
    g_clear_error(&error);
    return 1;
}
