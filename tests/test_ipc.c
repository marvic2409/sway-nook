#include "ipc_fixture.h"

static void fragmented_reply(void) {
    GBytes *reply = ipc_frame("{\"answer\":42}", 13, "i3-ipc");
    IpcFixture fixture = {0};
    ipc_fixture_start(&fixture, reply, 4, NULL, TRUE);
    GError *error = NULL;
    JsonNode *node = nook_ipc_request(fixture.path, 4, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(node);
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(node), "answer"), ==, 42);
    json_node_free(node);
    ipc_fixture_stop(&fixture);
    g_bytes_unref(reply);
}

static void rejected_reply(gconstpointer data) {
    const char *kind = data;
    GBytes *reply;
    if (!strcmp(kind, "truncated-header")) reply = g_bytes_new_static("i3-ipc", 6);
    else if (!strcmp(kind, "truncated-body")) reply = ipc_frame("{", 12, "i3-ipc");
    else if (!strcmp(kind, "invalid-header")) reply = ipc_frame(NULL, 0, "broken");
    else reply = ipc_frame(NULL, 32u * 1024u * 1024u + 1, "i3-ipc");
    IpcFixture fixture = {0};
    ipc_fixture_start(&fixture, reply, 4, NULL, FALSE);
    GError *error = NULL;
    g_assert_null(nook_ipc_request(fixture.path, 4, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    ipc_fixture_stop(&fixture);
    g_bytes_unref(reply);
}

static void subscription(gconstpointer data) {
    gboolean accepted = GPOINTER_TO_INT(data);
    const char *body = accepted ? "{\"success\":true}" : "{\"success\":false}";
    GBytes *reply = ipc_frame(body, strlen(body), "i3-ipc");
    IpcFixture fixture = {0};
    ipc_fixture_start(&fixture, reply, 2, "[\"window\"]", TRUE);
    GError *error = NULL;
    GSocket *socket = nook_ipc_subscribe(fixture.path, &error);
    if (accepted) {
        g_assert_no_error(error);
        g_assert_nonnull(socket);
        g_assert_false(g_socket_get_blocking(socket));
        g_assert_false(g_socket_is_closed(socket));
        g_object_unref(socket);
    } else {
        g_assert_null(socket);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
        g_assert_nonnull(strstr(error->message, "refused"));
        g_clear_error(&error);
    }
    ipc_fixture_stop(&fixture);
    g_bytes_unref(reply);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ipc/fragmented-reply", fragmented_reply);
    const char *invalid[] = {"truncated-header", "truncated-body", "invalid-header", "oversized"};
    for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
        char *path = g_strdup_printf("/ipc/%s", invalid[i]);
        g_test_add_data_func(path, invalid[i], rejected_reply);
        g_free(path);
    }
    g_test_add_data_func("/ipc/subscribe-success", GINT_TO_POINTER(TRUE), subscription);
    g_test_add_data_func("/ipc/subscribe-refused", GINT_TO_POINTER(FALSE), subscription);
    return g_test_run();
}
