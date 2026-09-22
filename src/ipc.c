#include "nook.h"
#include <string.h>

#define MAX_REPLY (32u * 1024u * 1024u)

static GSocketConnection *connect_socket(const char *path, GError **error) {
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, 3);
    GSocketAddress *address = g_unix_socket_address_new(path);
    GSocketConnection *connection = g_socket_client_connect(client,
        G_SOCKET_CONNECTABLE(address), NULL, error);
    if (connection) g_socket_set_timeout(g_socket_connection_get_socket(connection), 3);
    g_object_unref(address);
    g_object_unref(client);
    return connection;
}

static gboolean write_frame(GSocketConnection *connection, guint32 type,
                            const char *payload, GError **error) {
    gsize length = payload ? strlen(payload) : 0;
    if (length > G_MAXUINT32) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "IPC command is too large");
        return FALSE;
    }
    guint8 header[14] = {'i', '3', '-', 'i', 'p', 'c'};
    guint32 little_length = GUINT32_TO_LE((guint32)length);
    guint32 little_type = GUINT32_TO_LE(type);
    memcpy(header + 6, &little_length, 4);
    memcpy(header + 10, &little_type, 4);
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    gsize written;
    return g_output_stream_write_all(out, header, sizeof(header), &written, NULL, error) &&
        (!length || g_output_stream_write_all(out, payload, length, &written, NULL, error));
}

static JsonNode *read_reply(GSocketConnection *connection, GError **error) {
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    guint8 header[14];
    gsize read;
    if (!g_input_stream_read_all(in, header, sizeof(header), &read, NULL, error)) return NULL;
    if (read != sizeof(header) || memcmp(header, "i3-ipc", 6)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid Sway IPC response");
        return NULL;
    }
    guint32 little_length;
    memcpy(&little_length, header + 6, 4);
    guint32 length = GUINT32_FROM_LE(little_length);
    if (length > MAX_REPLY) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Sway IPC response is too large");
        return NULL;
    }
    char *body = g_malloc(length + 1);
    if (!g_input_stream_read_all(in, body, length, &read, NULL, error)) {
        g_free(body);
        return NULL;
    }
    if (read != length) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Incomplete Sway IPC response");
        g_free(body);
        return NULL;
    }
    body[length] = 0;
    JsonParser *parser = json_parser_new();
    JsonNode *node = NULL;
    if (json_parser_load_from_data(parser, body, length, error))
        node = json_node_copy(json_parser_get_root(parser));
    g_object_unref(parser);
    g_free(body);
    return node;
}

JsonNode *nook_ipc_request(const char *socket_path, guint32 type,
                           const char *payload, GError **error) {
    GSocketConnection *connection = connect_socket(socket_path, error);
    if (!connection) return NULL;
    JsonNode *node = NULL;
    if (write_frame(connection, type, payload, error))
        node = read_reply(connection, error);
    g_object_unref(connection);
    return node;
}

GSocket *nook_ipc_subscribe(const char *socket_path, GError **error) {
    GSocketConnection *connection = connect_socket(socket_path, error);
    if (!connection) return NULL;
    GSocket *socket = NULL;
    if (write_frame(connection, 2, "[\"window\"]", error)) {
        JsonNode *reply = read_reply(connection, error);
        if (reply) {
            JsonObject *obj = JSON_NODE_HOLDS_OBJECT(reply) ? json_node_get_object(reply) : NULL;
            if (obj && json_object_get_boolean_member_with_default(obj, "success", FALSE)) {
                socket = g_object_ref(g_socket_connection_get_socket(connection));
                g_socket_set_blocking(socket, FALSE);
            } else {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Sway refused event subscription");
            }
            json_node_free(reply);
        }
    }
    g_object_unref(connection);
    return socket;
}
