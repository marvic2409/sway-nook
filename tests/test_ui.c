/* Include the implementation to exercise its private callbacks without a test API. */
#include "../src/ui.c"
#include "ipc_fixture.h"
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    NookApp app;
    char *cache;
} UiFixture;

static void settle_layout(void) {
    gint64 deadline = g_get_monotonic_time() + 250000;
    do {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
}

static void ui_setup(UiFixture *fixture, gconstpointer data) {
    (void)data;
    fixture->cache = g_dir_make_tmp("nook-ui-XXXXXX", NULL);
    g_assert_nonnull(fixture->cache);
    NookApp *app = &fixture->app;
    app->cache_dir = fixture->cache;
    app->available_width = 800;
    app->app = gtk_application_new("io.github.swaynook.Test", G_APPLICATION_NON_UNIQUE);
    app->attempted = g_hash_table_new(g_direct_hash, g_direct_equal);
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(app->window), 800, 300);
    g_signal_connect(app->window, "destroy", G_CALLBACK(on_destroy), app);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app->window), body);
    app->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(app->stack), GTK_STACK_TRANSITION_TYPE_NONE);
    app->row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    app->scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app->scroller),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_container_add(GTK_CONTAINER(app->scroller), app->row);
    gtk_stack_add_named(GTK_STACK(app->stack), app->scroller, "windows");
    gtk_stack_add_named(GTK_STACK(app->stack), gtk_label_new("empty"), "empty");
    app->count = gtk_label_new("");
    app->status = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(body), app->stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(body), app->count, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), app->status, FALSE, FALSE, 0);
    gtk_widget_show_all(app->window);
    clear_error(app);
}

static void ui_teardown(UiFixture *fixture, gconstpointer data) {
    (void)data;
    close_shelf(&fixture->app);
    g_object_unref(fixture->app.app);
    g_assert_cmpint(g_rmdir(fixture->cache), ==, 0);
    g_free(fixture->cache);
    settle_layout();
}

static void refresh_cards(UiFixture *fixture, guint count) {
    GString *tree = g_string_new("{\"id\":1,\"type\":\"root\",\"nodes\":[{\"type\":\"workspace\",\"name\":\"__i3_scratch\",\"floating_nodes\":[");
    for (guint i = 0; i < count; i++)
        g_string_append_printf(tree, "%s{\"id\":%u,\"type\":\"con\",\"name\":\"Window %u\",\"app_id\":\"test\"}",
            i ? "," : "", i + 10, i + 1);
    g_string_append(tree, "]}]}");
    GBytes *reply = ipc_frame(tree->str, tree->len, "i3-ipc");
    IpcFixture server = {0};
    ipc_fixture_start(&server, reply, 4, NULL, FALSE);
    fixture->app.socket_path = server.path;
    refresh_shelf(&fixture->app);
    fixture->app.socket_path = NULL;
    ipc_fixture_stop(&server);
    g_bytes_unref(reply);
    g_string_free(tree, TRUE);
}

static void assert_selected_visible(NookApp *app, gint64 id) {
    g_assert_cmpint(app->selected_id, ==, id);
    GtkWidget *button = button_for_id(app, id);
    g_assert_nonnull(button);
    g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(button), "selected"));
    GtkAllocation allocation;
    gtk_widget_get_allocation(button, &allocation);
    GtkAdjustment *adjustment = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(app->scroller));
    double left = gtk_adjustment_get_value(adjustment);
    double right = left + gtk_adjustment_get_page_size(adjustment);
    g_test_message("selected=%" G_GINT64_FORMAT " scroll=%.0f page=%.0f card=[%d,%d]",
        id, left, right - left, allocation.x, allocation.x + allocation.width);
    g_assert_cmpfloat(allocation.x, >=, left - 1);
    g_assert_cmpfloat(allocation.x + allocation.width, <=, right + 1);
}

static void refresh_keeps_selection_visible(UiFixture *fixture, gconstpointer data) {
    (void)data;
    refresh_cards(fixture, 8);
    settle_layout();
    select_button(&fixture->app, button_for_id(&fixture->app, 17), TRUE);
    settle_layout();
    assert_selected_visible(&fixture->app, 17);
    refresh_cards(fixture, 8);
    settle_layout();
    assert_selected_visible(&fixture->app, 17);
    move_selection(&fixture->app, -1);
    settle_layout();
    assert_selected_visible(&fixture->app, 16);
}

static void missing_selection_falls_back(UiFixture *fixture, gconstpointer data) {
    (void)data;
    refresh_cards(fixture, 8);
    settle_layout();
    select_button(&fixture->app, button_for_id(&fixture->app, 17), TRUE);
    refresh_cards(fixture, 2);
    settle_layout();
    assert_selected_visible(&fixture->app, 10);
    refresh_cards(fixture, 0);
    settle_layout();
    g_assert_cmpint(fixture->app.selected_id, ==, 0);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(fixture->app.stack)), ==, "empty");
}

static GSocket *event_socket(int *peer) {
    int sockets[2];
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    GError *error = NULL;
    GSocket *socket = g_socket_new_from_fd(sockets[0], &error);
    g_assert_no_error(error);
    g_socket_set_blocking(socket, FALSE);
    *peer = sockets[1];
    return socket;
}

static void assert_connection_error(NookApp *app) {
    g_assert_true(gtk_widget_get_visible(app->status));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(app->status)), "reopen"));
}

static void event_disconnect(UiFixture *fixture, gconstpointer data) {
    int peer;
    GSocket *socket = event_socket(&peer);
    close(peer);
    g_assert_false(event_callback(socket, GPOINTER_TO_UINT(data), &fixture->app));
    assert_connection_error(&fixture->app);
    g_object_unref(socket);
}

static void refresh_preserves_connection_error(UiFixture *fixture, gconstpointer data) {
    gboolean read_error = !strcmp(data, "read");
    int peer;
    GSocket *socket = event_socket(&peer);
    close(peer);
    g_assert_false(event_callback(socket, G_IO_IN, &fixture->app));
    assert_connection_error(&fixture->app);
    GBytes *reply = read_error ? g_bytes_new_static("i3-ipc", 6) : ipc_frame("{}", 2, "i3-ipc");
    IpcFixture server = {0};
    ipc_fixture_start(&server, reply, 4, NULL, FALSE);
    fixture->app.socket_path = server.path;
    if (read_error) refresh_shelf(&fixture->app);
    else restore_window(&fixture->app, 17);
    fixture->app.socket_path = NULL;
    ipc_fixture_stop(&server);
    g_bytes_unref(reply);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(fixture->app.status)),
        read_error ? "Could not read the scratchpad" : "Could not restore window"));
    refresh_cards(fixture, 1);
    assert_connection_error(&fixture->app);
    g_object_unref(socket);
}

static void rejected_event(UiFixture *fixture, gconstpointer data) {
    const char *kind = data;
    GBytes *reply;
    if (!strcmp(kind, "invalid-header")) reply = ipc_frame(NULL, 0, "broken");
    else if (!strcmp(kind, "oversized")) reply = ipc_frame(NULL, 32u * 1024u * 1024u + 1, "i3-ipc");
    else reply = ipc_frame("!", 1, "i3-ipc");
    int peer;
    GSocket *socket = event_socket(&peer);
    gsize size;
    const void *bytes = g_bytes_get_data(reply, &size);
    g_assert_cmpint(write(peer, bytes, size), ==, (gssize)size);
    fixture->app.event_bytes = g_byte_array_new();
    g_assert_false(event_callback(socket, G_IO_IN, &fixture->app));
    assert_connection_error(&fixture->app);
    close(peer);
    g_object_unref(socket);
    g_bytes_unref(reply);
}

static void close_before_layout(UiFixture *fixture, gconstpointer data) {
    (void)data;
    refresh_cards(fixture, 8);
    close_shelf(&fixture->app);
    g_assert_null(fixture->app.window);
    settle_layout();
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check(&argc, &argv)) {
        g_print("No GTK display available; skipping UI tests\n");
        return 77;
    }
#define UI_TEST(path, data, function) \
    g_test_add(path, UiFixture, data, ui_setup, function, ui_teardown)
    UI_TEST("/ui/refresh-selection-visible", NULL, refresh_keeps_selection_visible);
    UI_TEST("/ui/missing-selection", NULL, missing_selection_falls_back);
    UI_TEST("/ui/connection-error-after-refresh", "restore", refresh_preserves_connection_error);
    UI_TEST("/ui/connection-error-after-read-failure", "read", refresh_preserves_connection_error);
    UI_TEST("/ui/event-hangup", GUINT_TO_POINTER(G_IO_HUP), event_disconnect);
    UI_TEST("/ui/event-error", GUINT_TO_POINTER(G_IO_ERR), event_disconnect);
    UI_TEST("/ui/event-eof", GUINT_TO_POINTER(G_IO_IN), event_disconnect);
    UI_TEST("/ui/event-invalid-header", "invalid-header", rejected_event);
    UI_TEST("/ui/event-oversized", "oversized", rejected_event);
    UI_TEST("/ui/event-invalid-json", "invalid-json", rejected_event);
    UI_TEST("/ui/close-before-layout", NULL, close_before_layout);
    return g_test_run();
}
