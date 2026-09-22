#include "nook.h"
#include <gtk-layer-shell.h>
#include <string.h>

#ifndef NOOK_DATA_DIR
#define NOOK_DATA_DIR "/usr/share/sway-nook"
#endif

typedef struct {
    GtkApplication *app;
    const char *socket_path;
    const char *cache_dir;
    GtkWidget *window, *row, *stack, *count, *status, *scroller;
    GSocket *events;
    GSource *event_source;
    GByteArray *event_bytes;
    GHashTable *attempted;
    GHashTable *hidden_ids;
    gint64 selected_id;
    guint refresh_source;
    gint available_width;
} NookApp;

typedef struct {
    NookWindow *window;
    char *cache_dir;
} CaptureTask;

static gint64 button_id(GtkWidget *button) {
    return (gint64)(gintptr)g_object_get_data(G_OBJECT(button), "nook-id");
}

static void show_error(NookApp *app, const char *message) {
    if (!app->status) return;
    gtk_label_set_text(GTK_LABEL(app->status), message);
    gtk_widget_show(app->status);
}

static void clear_error(NookApp *app) {
    gtk_label_set_text(GTK_LABEL(app->status), "");
    gtk_widget_hide(app->status);
}

static GtkWidget *button_for_id(NookApp *app, gint64 id) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->row));
    GtkWidget *found = NULL;
    for (GList *item = children; item; item = item->next) {
        if (button_id(item->data) == id) { found = item->data; break; }
    }
    g_list_free(children);
    return found;
}

static void select_button(NookApp *app, GtkWidget *button, gboolean focus) {
    if (!button) return;
    app->selected_id = button_id(button);
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->row));
    for (GList *item = children; item; item = item->next) {
        GtkStyleContext *context = gtk_widget_get_style_context(item->data);
        if (item->data == button) gtk_style_context_add_class(context, "selected");
        else gtk_style_context_remove_class(context, "selected");
    }
    g_list_free(children);
    if (focus) gtk_widget_grab_focus(button);
    GtkAdjustment *adjustment = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(app->scroller));
    GtkAllocation allocation;
    gtk_widget_get_allocation(button, &allocation);
    double value = gtk_adjustment_get_value(adjustment);
    double page = gtk_adjustment_get_page_size(adjustment);
    if (allocation.x < value) gtk_adjustment_set_value(adjustment, allocation.x);
    else if (allocation.x + allocation.width > value + page)
        gtk_adjustment_set_value(adjustment, allocation.x + allocation.width - page);
}

static gboolean focus_card(GtkWidget *button, GdkEventFocus *event, gpointer data) {
    (void)event;
    select_button(data, button, FALSE);
    return FALSE;
}

static void close_shelf(NookApp *app) {
    if (app->window) gtk_widget_destroy(app->window);
}

static void restore_window(NookApp *app, gint64 id) {
    if (id <= 0) return;
    GError *error = NULL;
    JsonNode *tree = nook_ipc_request(app->socket_path, 4, NULL, &error);
    gboolean hidden = FALSE;
    if (tree) {
        GPtrArray *windows = nook_hidden_windows(tree);
        for (guint i = 0; i < windows->len; i++)
            if (((NookWindow *)g_ptr_array_index(windows, i))->id == id) hidden = TRUE;
        g_ptr_array_unref(windows);
        json_node_free(tree);
    }
    if (!error && !hidden)
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "That window has already been shown or closed");
    if (!error) {
        char *command = g_strdup_printf("[con_id=%" G_GINT64_FORMAT "] scratchpad show", id);
        nook_sway_command(app->socket_path, command, &error);
        g_free(command);
    }
    if (error) {
        char *message = g_strdup_printf("Could not restore window: %s", error->message);
        show_error(app, message);
        g_free(message);
        g_clear_error(&error);
    } else close_shelf(app);
}

static void card_clicked(GtkButton *button, gpointer data) {
    restore_window(data, button_id(GTK_WIDGET(button)));
}

static void close_clicked(GtkButton *button, gpointer data) {
    (void)button;
    close_shelf(data);
}

static void apply_preview(GtkWidget *button, const char *path) {
    if (!path) return;
    GError *error = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(path, 256, 138, TRUE, &error);
    if (!pixbuf) { g_clear_error(&error); return; }
    GtkWidget *image = g_object_get_data(G_OBJECT(button), "preview-image");
    GtkWidget *stack = g_object_get_data(G_OBJECT(button), "preview-stack");
    gtk_image_set_from_pixbuf(GTK_IMAGE(image), pixbuf);
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "preview");
    g_object_unref(pixbuf);
}

static void capture_task_free(gpointer data) {
    CaptureTask *task = data;
    nook_window_free(task->window);
    g_free(task->cache_dir);
    g_free(task);
}

static void capture_worker(GTask *job, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source;
    (void)cancel;
    CaptureTask *task = data;
    char *path = nook_capture(task->cache_dir, task->window, FALSE);
    g_task_return_pointer(job, path, g_free);
}

static void capture_done(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source;
    NookApp *app = data;
    CaptureTask *task = g_task_get_task_data(G_TASK(result));
    char *path = g_task_propagate_pointer(G_TASK(result), NULL);
    if (app->window) {
        GtkWidget *button = button_for_id(app, task->window->id);
        if (button) apply_preview(button, path);
    }
    g_free(path);
}

static NookWindow *copy_window(const NookWindow *source) {
    NookWindow *copy = g_memdup2(source, sizeof(*source));
    copy->title = g_strdup(source->title);
    copy->app_id = g_strdup(source->app_id);
    copy->capture_id = g_strdup(source->capture_id);
    return copy;
}

static GtkWidget *make_card(NookApp *app, NookWindow *window) {
    GtkWidget *button = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_size_request(button, 280, -1);
    gtk_widget_set_can_focus(button, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(button), "window-card");
    g_object_set_data(G_OBJECT(button), "nook-id", (gpointer)(gintptr)window->id);
    g_signal_connect(button, "clicked", G_CALLBACK(card_clicked), app);
    g_signal_connect(button, "focus-in-event", G_CALLBACK(focus_card), app);
    char *tooltip = g_strdup_printf("Bring %s forward", window->title);
    gtk_widget_set_tooltip_text(button, tooltip);
    g_free(tooltip);
    atk_object_set_name(gtk_widget_get_accessible(button), window->title);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(button), body);
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(frame, 256, 138);
    gtk_widget_set_halign(frame, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(frame), "preview-frame");
    gtk_box_pack_start(GTK_BOX(body), frame, TRUE, TRUE, 0);
    GtkWidget *preview_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(preview_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(preview_stack), 120);
    gtk_box_pack_start(GTK_BOX(frame), preview_stack, TRUE, TRUE, 0);
    GtkWidget *image = gtk_image_new();
    gtk_widget_set_size_request(image, 256, 138);
    gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
    gtk_stack_add_named(GTK_STACK(preview_stack), image, "preview");
    GtkWidget *placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_halign(placeholder, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(placeholder, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(placeholder), "preview-placeholder");
    GtkWidget *placeholder_icon = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DIALOG);
    gtk_box_pack_start(GTK_BOX(placeholder), placeholder_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(placeholder), gtk_label_new("Preview unavailable"), FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(preview_stack), placeholder, "placeholder");
    g_object_set_data(G_OBJECT(button), "preview-image", image);
    g_object_set_data(G_OBJECT(button), "preview-stack", preview_stack);

    GtkWidget *details = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(details), "card-details");
    gtk_box_pack_start(GTK_BOX(body), details, FALSE, FALSE, 0);
    GtkIconTheme *icon_theme = gtk_icon_theme_get_default();
    const char *icon_name = gtk_icon_theme_has_icon(icon_theme, window->app_id)
        ? window->app_id : "application-x-executable";
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 22);
    gtk_style_context_add_class(gtk_widget_get_style_context(icon), "app-icon");
    gtk_box_pack_start(GTK_BOX(details), icon, FALSE, FALSE, 0);
    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_box_pack_start(GTK_BOX(details), labels, TRUE, TRUE, 0);
    GtkWidget *title = gtk_label_new(window->title);
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title), 30);
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "card-title");
    gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);
    GtkWidget *subtitle = gtk_label_new(window->app_id);
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "card-subtitle");
    gtk_box_pack_start(GTK_BOX(labels), subtitle, FALSE, FALSE, 0);
    if (window->count > 1) {
        char *count = g_strdup_printf("%d", window->count);
        GtkWidget *badge = gtk_label_new(count);
        gtk_style_context_add_class(gtk_widget_get_style_context(badge), "card-count");
        gtk_box_pack_end(GTK_BOX(details), badge, FALSE, FALSE, 0);
        g_free(count);
    }
    gtk_widget_show_all(button);
    gtk_stack_set_visible_child_name(GTK_STACK(preview_stack), "placeholder");
    char *path = nook_cached_preview(app->cache_dir, window->id);
    if (path) apply_preview(button, path);
    else if (window->capture_id &&
             !g_hash_table_contains(app->attempted, (gpointer)(gintptr)window->id)) {
        g_hash_table_add(app->attempted, (gpointer)(gintptr)window->id);
        CaptureTask *task = g_new0(CaptureTask, 1);
        task->window = copy_window(window);
        task->cache_dir = g_strdup(app->cache_dir);
        GTask *job = g_task_new(NULL, NULL, capture_done, app);
        g_task_set_task_data(job, task, capture_task_free);
        g_task_run_in_thread(job, capture_worker);
        g_object_unref(job);
    }
    g_free(path);
    return button;
}

static void refresh_shelf(NookApp *app) {
    GError *error = NULL;
    JsonNode *tree = nook_ipc_request(app->socket_path, 4, NULL, &error);
    if (!tree) {
        char *message = g_strdup_printf("Could not read the scratchpad: %s", error->message);
        show_error(app, message);
        g_free(message);
        g_clear_error(&error);
        return;
    }
    GPtrArray *windows = nook_hidden_windows(tree);
    GHashTable *live = nook_tree_ids(tree);
    g_clear_pointer(&app->hidden_ids, g_hash_table_unref);
    app->hidden_ids = nook_hidden_ids(tree);
    nook_snapshot_cleanup(app->cache_dir, live);
    g_hash_table_unref(live);
    json_node_free(tree);

    GList *children = gtk_container_get_children(GTK_CONTAINER(app->row));
    for (GList *item = children; item; item = item->next)
        gtk_container_remove(GTK_CONTAINER(app->row), item->data);
    g_list_free(children);
    gint total = 0;
    for (guint i = 0; i < windows->len; i++) {
        NookWindow *window = g_ptr_array_index(windows, i);
        GtkWidget *card = make_card(app, window);
        gtk_box_pack_start(GTK_BOX(app->row), card, FALSE, FALSE, 0);
        total += MAX(1, window->count);
    }
    char *count = g_strdup_printf("%d", total);
    gtk_label_set_text(GTK_LABEL(app->count), count);
    g_free(count);
    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), windows->len ? "windows" : "empty");
    gint width = MAX(MIN(520, app->available_width),
                     (gint)windows->len * 280 + MAX(0, (gint)windows->len - 1) * 12 + 36);
    gtk_window_set_default_size(GTK_WINDOW(app->window), MIN(app->available_width, width), -1);
    GtkWidget *selected = button_for_id(app, app->selected_id);
    if (!selected && windows->len)
        selected = button_for_id(app, ((NookWindow *)g_ptr_array_index(windows, 0))->id);
    if (selected) select_button(app, selected, FALSE);
    else app->selected_id = 0;
    g_ptr_array_unref(windows);
    clear_error(app);
}

static gboolean refresh_callback(gpointer data) {
    NookApp *app = data;
    app->refresh_source = 0;
    if (app->window) refresh_shelf(app);
    return G_SOURCE_REMOVE;
}

static gboolean event_callback(GSocket *socket, GIOCondition condition, gpointer data) {
    NookApp *app = data;
    if (condition & (G_IO_HUP | G_IO_ERR)) return G_SOURCE_REMOVE;
    guint8 chunk[8192];
    while (TRUE) {
        GError *error = NULL;
        gssize count = g_socket_receive(socket, (char *)chunk, sizeof(chunk), NULL, &error);
        if (count <= 0) {
            gboolean wait = error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
            g_clear_error(&error);
            if (!wait) show_error(app, "Sway event connection closed; reopen the shelf");
            return wait ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
        }
        g_byte_array_append(app->event_bytes, chunk, count);
        while (app->event_bytes->len >= 14) {
            const guint8 *frame = app->event_bytes->data;
            if (memcmp(frame, "i3-ipc", 6)) return G_SOURCE_REMOVE;
            guint32 raw_length;
            memcpy(&raw_length, frame + 6, 4);
            guint32 length = GUINT32_FROM_LE(raw_length);
            if (length > 32u * 1024u * 1024u) return G_SOURCE_REMOVE;
            if (app->event_bytes->len < (gsize)length + 14) break;
            JsonParser *parser = json_parser_new();
            gboolean relevant = FALSE;
            if (json_parser_load_from_data(parser, (const char *)frame + 14, length, NULL)) {
                JsonNode *root = json_parser_get_root(parser);
                JsonObject *event = JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
                const char *change = event
                    ? json_object_get_string_member_with_default(event, "change", "") : "";
                if (!g_strcmp0(change, "new") || !g_strcmp0(change, "close") ||
                    !g_strcmp0(change, "move")) relevant = TRUE;
                else if (!g_strcmp0(change, "title") && app->hidden_ids) {
                    JsonObject *container = json_object_get_object_member(event, "container");
                    gint64 id = container
                        ? json_object_get_int_member_with_default(container, "id", 0) : 0;
                    char *key = g_strdup_printf("%" G_GINT64_FORMAT, id);
                    relevant = g_hash_table_contains(app->hidden_ids, key);
                    g_free(key);
                }
            }
            g_object_unref(parser);
            g_byte_array_remove_range(app->event_bytes, 0, length + 14);
            if (relevant && !app->refresh_source)
                app->refresh_source = g_timeout_add(80, refresh_callback, app);
        }
    }
}

static void on_destroy(GtkWidget *widget, gpointer data) {
    (void)widget;
    NookApp *app = data;
    app->window = NULL;
    if (app->refresh_source) {
        g_source_remove(app->refresh_source);
        app->refresh_source = 0;
    }
    if (app->event_source) {
        g_source_destroy(app->event_source);
        g_source_unref(app->event_source);
        app->event_source = NULL;
    }
    g_clear_object(&app->events);
    g_clear_pointer(&app->event_bytes, g_byte_array_unref);
    g_clear_pointer(&app->attempted, g_hash_table_unref);
    g_clear_pointer(&app->hidden_ids, g_hash_table_unref);
    g_application_quit(G_APPLICATION(app->app));
}

static void move_selection(NookApp *app, gint delta) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->row));
    gint count = g_list_length(children);
    if (count) {
        gint index = 0;
        for (GList *item = children; item; item = item->next, index++)
            if (button_id(item->data) == app->selected_id) break;
        GtkWidget *next = g_list_nth_data(children, ((index + delta) % count + count) % count);
        select_button(app, next, TRUE);
    }
    g_list_free(children);
}

static gboolean key_pressed(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    (void)widget;
    NookApp *app = data;
    guint key = gdk_keyval_to_lower(event->keyval);
    if (key == GDK_KEY_Escape) { close_shelf(app); return TRUE; }
    if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) {
        restore_window(app, app->selected_id);
        return TRUE;
    }
    if (key == GDK_KEY_Left || key == GDK_KEY_h || key == GDK_KEY_k ||
        key == GDK_KEY_ISO_Left_Tab) { move_selection(app, -1); return TRUE; }
    if (key == GDK_KEY_Right || key == GDK_KEY_l || key == GDK_KEY_j || key == GDK_KEY_Tab) {
        move_selection(app, event->state & GDK_SHIFT_MASK ? -1 : 1);
        return TRUE;
    }
    return FALSE;
}

static gboolean load_css(NookApp *app) {
    char *config_path = g_build_filename(g_get_user_config_dir(), "sway-nook", "theme.ini", NULL);
    GError *error = NULL;
    NookTheme *theme = nook_theme_load(config_path, &error);
    g_free(config_path);
    if (!theme) {
        g_printerr("sway-nook: theme: %s\n", error->message);
        g_clear_error(&error);
        return FALSE;
    }
    char *theme_css = nook_theme_css(theme);
    nook_theme_free(theme);
    char *path = g_build_filename(NOOK_DATA_DIR, "style.css", NULL);
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        g_free(path);
        char *executable = g_file_read_link("/proc/self/exe", NULL);
        char *binary_dir = executable ? g_path_get_dirname(executable) : g_strdup(".");
        path = g_build_filename(binary_dir, "..", "data", "style.css", NULL);
        g_free(binary_dir);
        g_free(executable);
    }
    char *style_css = NULL;
    gboolean ok = g_file_get_contents(path, &style_css, NULL, &error);
    g_free(path);
    if (!ok) {
        g_printerr("sway-nook: CSS: %s\n", error->message);
        g_clear_error(&error);
        g_free(theme_css);
        return FALSE;
    }
    char *css = g_strconcat(theme_css, style_css, NULL);
    GtkCssProvider *provider = gtk_css_provider_new();
    ok = gtk_css_provider_load_from_data(provider, css, -1, &error);
    if (ok) gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    else {
        g_printerr("sway-nook: CSS: %s\n", error->message);
        g_clear_error(&error);
    }
    g_object_unref(provider);
    g_free(css);
    g_free(style_css);
    g_free(theme_css);
    (void)app;
    return ok;
}

static void position_on_output(NookApp *app) {
    app->available_width = 960;
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return;
    GdkMonitor *monitor = NULL;
    JsonNode *reply = nook_ipc_request(app->socket_path, 3, NULL, NULL);
    if (reply && JSON_NODE_HOLDS_ARRAY(reply)) {
        JsonArray *outputs = json_node_get_array(reply);
        for (guint i = 0; i < json_array_get_length(outputs); i++) {
            JsonObject *output = json_array_get_object_element(outputs, i);
            if (!output || !json_object_get_boolean_member_with_default(output, "focused", FALSE)) continue;
            JsonObject *rect = json_object_get_object_member(output, "rect");
            if (!rect) break;
            int x = json_object_get_int_member_with_default(rect, "x", 0);
            int y = json_object_get_int_member_with_default(rect, "y", 0);
            int width = json_object_get_int_member_with_default(rect, "width", 0);
            int height = json_object_get_int_member_with_default(rect, "height", 0);
            monitor = gdk_display_get_monitor_at_point(display, x + width / 2, y + height / 2);
            break;
        }
    }
    if (reply) json_node_free(reply);
    if (!monitor) monitor = gdk_display_get_primary_monitor(display);
    if (!monitor && gdk_display_get_n_monitors(display) > 0)
        monitor = gdk_display_get_monitor(display, 0);
    if (monitor) {
        GdkRectangle geometry;
        gdk_monitor_get_geometry(monitor, &geometry);
        app->available_width = MAX(360, geometry.width - 32);
        gtk_layer_set_monitor(GTK_WINDOW(app->window), monitor);
    }
}

static GtkWidget *label_with_class(const char *text, const char *name) {
    GtkWidget *label = gtk_label_new(text);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), name);
    return label;
}

static void build_window(NookApp *app) {
    app->window = gtk_application_window_new(app->app);
    gtk_widget_set_name(app->window, "sway-nook");
    gtk_style_context_add_class(gtk_widget_get_style_context(app->window), "shelf-window");
    gtk_window_set_title(GTK_WINDOW(app->window), "Sway Nook");
    gtk_window_set_decorated(GTK_WINDOW(app->window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(app->window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(app->window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(app->window), TRUE);
    g_signal_connect(app->window, "destroy", G_CALLBACK(on_destroy), app);
    g_signal_connect(app->window, "key-press-event", G_CALLBACK(key_pressed), app);
    gtk_layer_init_for_window(GTK_WINDOW(app->window));
    gtk_layer_set_layer(GTK_WINDOW(app->window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(GTK_WINDOW(app->window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(app->window), 0);
    gtk_layer_set_margin(GTK_WINDOW(app->window), GTK_LAYER_SHELL_EDGE_BOTTOM, 42);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(app->window), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    position_on_output(app);
    gtk_window_set_default_size(GTK_WINDOW(app->window), MIN(520, app->available_width), -1);

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(panel), "shelf-panel");
    gtk_container_add(GTK_CONTAINER(app->window), panel);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(header), "shelf-header");
    gtk_box_pack_start(GTK_BOX(panel), header, FALSE, FALSE, 0);
    GtkWidget *mark = gtk_image_new_from_icon_name("view-grid-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_style_context_add_class(gtk_widget_get_style_context(mark), "shelf-mark");
    gtk_box_pack_start(GTK_BOX(header), mark, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), label_with_class("Scratchpad", "shelf-title"), FALSE, FALSE, 0);
    app->count = label_with_class("0", "total-count");
    gtk_box_pack_start(GTK_BOX(header), app->count, FALSE, FALSE, 0);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(close, "Hide Sway Nook (Esc)");
    gtk_style_context_add_class(gtk_widget_get_style_context(close), "close-button");
    g_signal_connect(close, "clicked", G_CALLBACK(close_clicked), app);
    gtk_box_pack_end(GTK_BOX(header), close, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(header), label_with_class("Within reach", "header-hint"), FALSE, FALSE, 0);

    app->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(app->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(app->stack), 120);
    gtk_box_pack_start(GTK_BOX(panel), app->stack, TRUE, TRUE, 0);
    GtkWidget *empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(empty), "empty-state");
    GtkWidget *empty_icon = gtk_image_new_from_icon_name("view-grid-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_style_context_add_class(gtk_widget_get_style_context(empty_icon), "empty-icon");
    gtk_box_pack_start(GTK_BOX(empty), empty_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(empty), label_with_class("Your scratchpad is empty", "empty-title"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(empty), label_with_class("Press Super + Shift + − to tuck away a window.", "empty-help"), FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(app->stack), empty, "empty");
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_stack_add_named(GTK_STACK(app->stack), content, "windows");
    app->scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app->scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(app->scroller), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(app->scroller), "card-scroller");
    gtk_box_pack_start(GTK_BOX(content), app->scroller, TRUE, TRUE, 0);
    app->row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(app->row), "card-row");
    gtk_container_add(GTK_CONTAINER(app->scroller), app->row);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(footer), "shelf-footer");
    gtk_box_pack_end(GTK_BOX(panel), footer, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(footer), label_with_class("← → / h j k l   navigate    ↵ restore", "key-hints"), FALSE, FALSE, 0);
    app->status = label_with_class("", "status");
    gtk_label_set_xalign(GTK_LABEL(app->status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(app->status), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(footer), app->status, TRUE, TRUE, 8);
    gtk_box_pack_end(GTK_BOX(footer), label_with_class("esc   hide", "key-hints"), FALSE, FALSE, 0);
    gtk_widget_show_all(app->window);
    gtk_widget_hide(app->status);
}

static int on_command_line(GApplication *application, GApplicationCommandLine *line, gpointer data) {
    (void)application;
    NookApp *app = data;
    int argc = 0;
    char **argv = g_application_command_line_get_arguments(line, &argc);
    const char *action = argc > 1 ? argv[1] : "toggle";
    if (app->window) {
        if (!g_strcmp0(action, "toggle")) close_shelf(app);
        else gtk_window_present(GTK_WINDOW(app->window));
        g_strfreev(argv);
        return 0;
    }
    if (!load_css(app)) { g_strfreev(argv); return 1; }
    app->attempted = g_hash_table_new(g_direct_hash, g_direct_equal);
    build_window(app);
    GError *error = NULL;
    app->events = nook_ipc_subscribe(app->socket_path, &error);
    if (app->events) {
        app->event_bytes = g_byte_array_new();
        app->event_source = g_socket_create_source(app->events, G_IO_IN | G_IO_HUP | G_IO_ERR, NULL);
        g_source_set_callback(app->event_source, G_SOURCE_FUNC(event_callback), app, NULL);
        g_source_attach(app->event_source, NULL);
    } else {
        char *message = g_strdup_printf("Could not watch Sway events: %s", error->message);
        show_error(app, message);
        g_free(message);
        g_clear_error(&error);
    }
    refresh_shelf(app);
    gtk_window_present(GTK_WINDOW(app->window));
    GtkWidget *selected = button_for_id(app, app->selected_id);
    if (selected) select_button(app, selected, TRUE);
    g_strfreev(argv);
    return 0;
}

int nook_ui_main(const char *socket_path, const char *cache_dir, int argc, char **argv) {
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, socket_path, -1);
    char *short_hash = g_strndup(hash, 16);
    char *application_id = g_strdup_printf("io.github.marvic2409.SwayNook.Session%s", short_hash);
    g_free(short_hash);
    g_free(hash);
    NookApp app = {.socket_path = socket_path, .cache_dir = cache_dir};
    app.app = gtk_application_new(application_id, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_free(application_id);
    g_signal_connect(app.app, "command-line", G_CALLBACK(on_command_line), &app);
    int result = g_application_run(G_APPLICATION(app.app), argc, argv);
    g_object_unref(app.app);
    return result;
}
