#include "nook.h"

static JsonObject *object(JsonNode *node) {
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

static const char *string(JsonObject *obj, const char *key) {
    JsonNode *node = obj ? json_object_get_member(obj, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
        ? json_node_get_string(node) : NULL;
}

static gint64 number(JsonObject *obj, const char *key) {
    JsonNode *node = obj ? json_object_get_member(obj, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64
        ? json_node_get_int(node) : 0;
}

static gboolean flag(JsonObject *obj, const char *key) {
    JsonNode *node = obj ? json_object_get_member(obj, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN
        ? json_node_get_boolean(node) : FALSE;
}

static void each_child(JsonNode *node, void (*visit)(JsonNode *, gpointer), gpointer data) {
    JsonObject *obj = object(node);
    if (!obj) return;
    const char *keys[] = {"nodes", "floating_nodes"};
    for (guint k = 0; k < G_N_ELEMENTS(keys); k++) {
        JsonNode *member = json_object_get_member(obj, keys[k]);
        if (!member || !JSON_NODE_HOLDS_ARRAY(member)) continue;
        JsonArray *array = json_node_get_array(member);
        for (guint i = 0; i < json_array_get_length(array); i++)
            visit(json_array_get_element(array, i), data);
    }
}

static gboolean is_window(JsonNode *node) {
    JsonObject *obj = object(node);
    const char *type = string(obj, "type");
    return type && (!g_strcmp0(type, "con") || !g_strcmp0(type, "floating_con")) &&
        (string(obj, "app_id") || number(obj, "window") || number(obj, "pid") ||
         string(obj, "foreign_toplevel_identifier") || json_object_has_member(obj, "window_properties"));
}

static JsonNode *representative(JsonNode *node) {
    if (is_window(node)) return node;
    JsonObject *obj = object(node);
    if (!obj) return NULL;
    JsonNode *focus_node = json_object_get_member(obj, "focus");
    if (focus_node && JSON_NODE_HOLDS_ARRAY(focus_node)) {
        JsonArray *focus = json_node_get_array(focus_node);
        for (guint i = 0; i < json_array_get_length(focus); i++) {
            gint64 id = json_array_get_int_element(focus, i);
            const char *keys[] = {"nodes", "floating_nodes"};
            for (guint k = 0; k < G_N_ELEMENTS(keys); k++) {
                JsonNode *member = json_object_get_member(obj, keys[k]);
                if (!member || !JSON_NODE_HOLDS_ARRAY(member)) continue;
                JsonArray *children = json_node_get_array(member);
                for (guint j = 0; j < json_array_get_length(children); j++) {
                    JsonNode *child = json_array_get_element(children, j);
                    if (number(object(child), "id") != id) continue;
                    JsonNode *found = representative(child);
                    if (found) return found;
                }
            }
        }
    }
    const char *keys[] = {"nodes", "floating_nodes"};
    for (guint k = 0; k < G_N_ELEMENTS(keys); k++) {
        JsonNode *member = json_object_get_member(obj, keys[k]);
        if (!member || !JSON_NODE_HOLDS_ARRAY(member)) continue;
        JsonArray *children = json_node_get_array(member);
        for (guint i = 0; i < json_array_get_length(children); i++) {
            JsonNode *found = representative(json_array_get_element(children, i));
            if (found) return found;
        }
    }
    return NULL;
}

static void count_visit(JsonNode *node, gpointer data) {
    if (is_window(node)) (*(gint *)data)++;
    each_child(node, count_visit, data);
}

static NookWindow *window_from_node(JsonNode *node) {
    JsonNode *leaf = representative(node);
    if (!leaf) return NULL;
    JsonObject *root = object(node), *obj = object(leaf);
    JsonNode *props_node = json_object_get_member(obj, "window_properties");
    JsonObject *props = object(props_node);
    const char *app = string(obj, "app_id");
    if (!app) app = string(props, "class");
    if (!app) app = string(props, "instance");
    if (!app) app = "Application";
    const char *title = string(obj, "name");
    if (!title) title = string(props, "title");
    if (!title) title = app;
    NookWindow *window = g_new0(NookWindow, 1);
    window->id = number(root, "id");
    window->title = g_strdup(title);
    window->app_id = g_strdup(app);
    window->capture_id = g_strdup(string(obj, "foreign_toplevel_identifier"));
    window->visible = flag(obj, "visible");
    JsonObject *rect = object(json_object_get_member(obj, "rect"));
    window->x = number(rect, "x");
    window->y = number(rect, "y");
    window->width = number(rect, "width");
    window->height = number(rect, "height");
    count_visit(node, &window->count);
    return window;
}

void nook_window_free(gpointer data) {
    NookWindow *window = data;
    if (!window) return;
    g_free(window->title);
    g_free(window->app_id);
    g_free(window->capture_id);
    g_free(window);
}

static void add_hidden(JsonNode *node, gpointer data) {
    NookWindow *window = window_from_node(node);
    if (window) g_ptr_array_add(data, window);
}

typedef struct { JsonNode *scratch; } ScratchSearch;
static void find_scratch(JsonNode *node, gpointer data) {
    ScratchSearch *search = data;
    if (search->scratch) return;
    JsonObject *obj = object(node);
    if (!g_strcmp0(string(obj, "type"), "workspace") &&
        !g_strcmp0(string(obj, "name"), "__i3_scratch")) {
        search->scratch = node;
        return;
    }
    each_child(node, find_scratch, data);
}

GPtrArray *nook_hidden_windows(JsonNode *tree) {
    GPtrArray *windows = g_ptr_array_new_with_free_func(nook_window_free);
    ScratchSearch search = {0};
    find_scratch(tree, &search);
    if (search.scratch) each_child(search.scratch, add_hidden, windows);
    return windows;
}

typedef struct { NookWindow *window; } FocusSearch;
static void find_focused(JsonNode *node, JsonNode *floating_root, FocusSearch *search) {
    if (search->window) return;
    JsonObject *obj = object(node);
    const char *type = string(obj, "type");
    if (flag(obj, "focused") && type &&
        (!g_strcmp0(type, "con") || !g_strcmp0(type, "floating_con"))) {
        search->window = window_from_node(floating_root ? floating_root : node);
        if (search->window) return;
    }
    const char *keys[] = {"nodes", "floating_nodes"};
    for (guint k = 0; k < G_N_ELEMENTS(keys); k++) {
        JsonNode *member = obj ? json_object_get_member(obj, keys[k]) : NULL;
        if (!member || !JSON_NODE_HOLDS_ARRAY(member)) continue;
        JsonArray *children = json_node_get_array(member);
        for (guint i = 0; i < json_array_get_length(children); i++) {
            JsonNode *child = json_array_get_element(children, i);
            JsonNode *root = k == 1 && !floating_root ? child : floating_root;
            find_focused(child, root, search);
            if (search->window) return;
        }
    }
}

NookWindow *nook_focused_window(JsonNode *tree) {
    FocusSearch search = {0};
    find_focused(tree, NULL, &search);
    return search.window;
}

typedef struct { gint64 id; gboolean found; } IdSearch;
static void find_id(JsonNode *node, gpointer data) {
    IdSearch *search = data;
    if (number(object(node), "id") == search->id) search->found = TRUE;
    if (!search->found) each_child(node, find_id, data);
}

gboolean nook_tree_has_id(JsonNode *tree, gint64 id) {
    if (id <= 0) return FALSE;
    IdSearch search = {id, FALSE};
    find_id(tree, &search);
    return search.found;
}

static void add_id(JsonNode *node, gpointer data) {
    gint64 id = number(object(node), "id");
    if (id > 0) g_hash_table_add(data, g_strdup_printf("%" G_GINT64_FORMAT, id));
    each_child(node, add_id, data);
}

GHashTable *nook_tree_ids(JsonNode *tree) {
    GHashTable *ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    add_id(tree, ids);
    return ids;
}

GHashTable *nook_hidden_ids(JsonNode *tree) {
    GHashTable *ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    ScratchSearch search = {0};
    find_scratch(tree, &search);
    if (search.scratch) each_child(search.scratch, add_id, ids);
    return ids;
}
