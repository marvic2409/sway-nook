#define _POSIX_C_SOURCE 200809L
#include "nook.h"
#include <errno.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

static gboolean ensure_private_dir(const char *path, GError **error) {
    if (g_mkdir(path, 0700) != 0 && errno != EEXIST) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot create snapshot directory: %s", g_strerror(errno));
        return FALSE;
    }
    struct stat st;
    if (g_lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_ACCES,
                            "Snapshot directory must belong to your user");
        return FALSE;
    }
    if (g_chmod(path, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot protect snapshot directory: %s", g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

typedef struct {
    GMainLoop *loop;
    GBytes *bytes;
    GError *error;
    gboolean ok;
} CaptureReply;

static void capture_finished(GObject *source, GAsyncResult *result, gpointer data) {
    CaptureReply *reply = data;
    reply->ok = g_subprocess_communicate_finish(G_SUBPROCESS(source), result,
                                                 &reply->bytes, NULL, &reply->error);
    g_main_loop_quit(reply->loop);
}

static gboolean capture_expired(gpointer data) {
    g_subprocess_force_exit(G_SUBPROCESS(data));
    return G_SOURCE_REMOVE;
}

char *nook_snapshot_dir(const char *socket_path, GError **error) {
    const char *runtime = g_getenv("XDG_RUNTIME_DIR");
    if (!runtime) runtime = g_get_tmp_dir();
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, socket_path, -1);
    char *base = g_strdup_printf("%s/sway-nook-%u", runtime, (guint)getuid());
    char *short_hash = g_strndup(hash, 12);
    char *dir = g_build_filename(base, short_hash, NULL);
    g_free(hash);
    g_free(short_hash);
    if (!ensure_private_dir(base, error) || !ensure_private_dir(dir, error)) {
        g_free(dir);
        dir = NULL;
    }
    g_free(base);
    return dir;
}

char *nook_cached_preview(const char *dir, gint64 id) {
    if (id <= 0) return NULL;
    char *name = g_strdup_printf("%" G_GINT64_FORMAT ".png", id);
    char *path = g_build_filename(dir, name, NULL);
    g_free(name);
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) return path;
    g_free(path);
    return NULL;
}

static char *capture_once(const char *dir, const NookWindow *window,
                          const char *target, gboolean region) {
    char *geometry = NULL;
    const char *args[8] = {"grim", NULL};
    guint i = 1;
    if (region) {
        geometry = g_strdup_printf("%d,%d %dx%d", window->x, window->y,
                                   window->width, window->height);
        args[i++] = "-g";
        args[i++] = geometry;
    } else {
        args[i++] = "-T";
        args[i++] = target;
    }
    args[i++] = "-s";
    args[i++] = "0.35";
    args[i++] = "-";
    GError *error = NULL;
    GSubprocess *process = g_subprocess_newv(args,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    g_free(geometry);
    if (!process) { g_clear_error(&error); return NULL; }
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    CaptureReply reply = {.loop = g_main_loop_new(context, FALSE)};
    GSource *deadline = g_timeout_source_new(1200);
    g_source_set_callback(deadline, capture_expired, process, NULL);
    g_source_attach(deadline, context);
    g_subprocess_communicate_async(process, NULL, NULL, capture_finished, &reply);
    g_main_loop_run(reply.loop);
    g_source_destroy(deadline);
    g_source_unref(deadline);
    g_main_loop_unref(reply.loop);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    if (!reply.ok || !g_subprocess_get_successful(process) || !reply.bytes) {
        g_clear_error(&reply.error);
        g_clear_pointer(&reply.bytes, g_bytes_unref);
        g_object_unref(process);
        return NULL;
    }
    g_object_unref(process);
    gsize size;
    const guint8 *data = g_bytes_get_data(reply.bytes, &size);
    if (!size || !data) {
        g_bytes_unref(reply.bytes);
        return NULL;
    }
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    gboolean ok = gdk_pixbuf_loader_write(loader, data, size, NULL) &&
         gdk_pixbuf_loader_close(loader, NULL);
    GdkPixbuf *source = ok ? gdk_pixbuf_loader_get_pixbuf(loader) : NULL;
    if (source) g_object_ref(source);
    g_object_unref(loader);
    g_bytes_unref(reply.bytes);
    if (!source) return NULL;

    gint width = gdk_pixbuf_get_width(source), height = gdk_pixbuf_get_height(source);
    double scale = MIN(1.0, MIN(640.0 / width, 400.0 / height));
    gint out_width = MAX(1, (gint)(width * scale));
    gint out_height = MAX(1, (gint)(height * scale));
    GdkPixbuf *opaque = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, out_width, out_height);
    gdk_pixbuf_fill(opaque, 0x090909ff);
    gdk_pixbuf_composite(source, opaque, 0, 0, out_width, out_height,
                         0, 0, scale, scale, GDK_INTERP_BILINEAR, 255);
    g_object_unref(source);
    char *template = g_build_filename(dir, ".capture-XXXXXX", NULL);
    int fd = g_mkstemp(template);
    if (fd >= 0) close(fd);
    ok = fd >= 0 && gdk_pixbuf_save(opaque, template, "png", NULL,
                                    "compression", "3", NULL);
    g_object_unref(opaque);
    if (ok) {
        char *name = g_strdup_printf("%" G_GINT64_FORMAT ".png", window->id);
        char *path = g_build_filename(dir, name, NULL);
        g_free(name);
        if (g_rename(template, path) == 0) {
            g_free(template);
            return path;
        }
        g_free(path);
    }
    g_unlink(template);
    g_free(template);
    return NULL;
}

char *nook_capture(const char *dir, const NookWindow *window, gboolean allow_region) {
    if (!window || window->id <= 0) return NULL;
    char *path = NULL;
    if (window->capture_id)
        path = capture_once(dir, window, window->capture_id, FALSE);
    if (!path && allow_region && window->visible && window->width > 0 && window->height > 0)
        path = capture_once(dir, window, NULL, TRUE);
    return path ? path : nook_cached_preview(dir, window->id);
}

void nook_snapshot_cleanup(const char *dir, GHashTable *live_ids) {
    GDir *folder = g_dir_open(dir, 0, NULL);
    if (!folder) return;
    const char *name;
    while ((name = g_dir_read_name(folder))) {
        if (!g_str_has_suffix(name, ".png")) continue;
        char *id = g_strndup(name, strlen(name) - 4);
        if (!g_hash_table_contains(live_ids, id)) {
            char *path = g_build_filename(dir, name, NULL);
            g_unlink(path);
            g_free(path);
        }
        g_free(id);
    }
    g_dir_close(folder);
}
