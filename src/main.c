#include "nook.h"

static void usage(const char *program) {
    g_print("Usage: %s [toggle|show|stash|list|restore ID]\n", program);
}

int main(int argc, char **argv) {
    const char *action = argc > 1 ? argv[1] : "toggle";
    if (!g_strcmp0(action, "--help") || !g_strcmp0(action, "-h")) {
        usage(argv[0]);
        return 0;
    }
    if (!g_strcmp0(action, "--version")) {
        g_print("sway-nook 0.1.0\n");
        return 0;
    }
    gboolean ui = !g_strcmp0(action, "toggle") || !g_strcmp0(action, "show");
    gboolean cli = !g_strcmp0(action, "stash") || !g_strcmp0(action, "list") ||
                   !g_strcmp0(action, "restore");
    if ((!ui && !cli) || (argc > 2 && g_strcmp0(action, "restore")) ||
        (!g_strcmp0(action, "restore") && argc != 3)) {
        usage(argv[0]);
        return 2;
    }
    const char *socket_path = g_getenv("SWAYSOCK");
    if (!socket_path || !*socket_path) {
        g_printerr("sway-nook: run inside a Sway session (SWAYSOCK is missing)\n");
        return 1;
    }
    GError *error = NULL;
    char *cache_dir = nook_snapshot_dir(socket_path, &error);
    if (!cache_dir) {
        g_printerr("sway-nook: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }
    int result = ui ? nook_ui_main(socket_path, cache_dir, argc, argv)
                    : nook_cli_main(socket_path, cache_dir, argc, argv);
    g_free(cache_dir);
    return result;
}
