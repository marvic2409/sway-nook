#ifndef NOOK_TEST_IPC_FIXTURE_H
#define NOOK_TEST_IPC_FIXTURE_H

#include "nook.h"
#include <glib/gstdio.h>
#include <string.h>

typedef struct {
    GSocketListener *listener;
    GThread *thread;
    char *directory, *path;
    GBytes *reply;
    guint32 request_type;
    const char *request_payload;
    gboolean fragmented;
} IpcFixture;

static GBytes *ipc_frame(const char *body, guint32 length, const char *magic) {
    GByteArray *bytes = g_byte_array_new();
    guint8 header[14] = {0};
    memcpy(header, magic, 6);
    guint32 raw = GUINT32_TO_LE(length);
    memcpy(header + 6, &raw, 4);
    g_byte_array_append(bytes, header, sizeof(header));
    if (body) g_byte_array_append(bytes, (const guint8 *)body, strlen(body));
    return g_byte_array_free_to_bytes(bytes);
}

static gpointer ipc_serve(gpointer data) {
    IpcFixture *fixture = data;
    GError *error = NULL;
    GSocketConnection *connection = g_socket_listener_accept(fixture->listener, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(connection);
    g_socket_set_timeout(g_socket_connection_get_socket(connection), 3);
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    guint8 header[14];
    gsize count;
    g_assert_true(g_input_stream_read_all(input, header, sizeof(header), &count, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(count, ==, sizeof(header));
    g_assert_cmpmem(header, 6, "i3-ipc", 6);
    guint32 raw_length, raw_type;
    memcpy(&raw_length, header + 6, 4);
    memcpy(&raw_type, header + 10, 4);
    g_assert_cmpuint(GUINT32_FROM_LE(raw_type), ==, fixture->request_type);
    guint32 length = GUINT32_FROM_LE(raw_length);
    g_assert_cmpuint(length, ==, strlen(fixture->request_payload));
    char *payload = g_malloc(length + 1);
    g_assert_true(g_input_stream_read_all(input, payload, length, &count, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(count, ==, length);
    payload[length] = 0;
    g_assert_cmpstr(payload, ==, fixture->request_payload);
    g_free(payload);
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    gsize size;
    const guint8 *reply = g_bytes_get_data(fixture->reply, &size);
    for (gsize offset = 0; offset < size;) {
        gsize chunk = fixture->fragmented ? 1 : size - offset;
        g_assert_true(g_output_stream_write_all(output, reply + offset, chunk, &count, NULL, &error));
        g_assert_no_error(error);
        offset += count;
        if (fixture->fragmented) g_usleep(1000);
    }
    g_assert_true(g_io_stream_close(G_IO_STREAM(connection), NULL, &error));
    g_assert_no_error(error);
    g_object_unref(connection);
    return NULL;
}

static void ipc_fixture_start(IpcFixture *fixture, GBytes *reply, guint32 type,
                              const char *payload, gboolean fragmented) {
    fixture->directory = g_dir_make_tmp("nook-ipc-XXXXXX", NULL);
    g_assert_nonnull(fixture->directory);
    fixture->path = g_build_filename(fixture->directory, "ipc.sock", NULL);
    fixture->reply = g_bytes_ref(reply);
    fixture->request_type = type;
    fixture->request_payload = payload ? payload : "";
    fixture->fragmented = fragmented;
    fixture->listener = g_socket_listener_new();
    GSocketAddress *address = g_unix_socket_address_new(fixture->path);
    GError *error = NULL;
    g_assert_true(g_socket_listener_add_address(fixture->listener, address,
        G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error));
    g_assert_no_error(error);
    g_object_unref(address);
    fixture->thread = g_thread_new("ipc-fixture", ipc_serve, fixture);
}

static void ipc_fixture_stop(IpcFixture *fixture) {
    g_thread_join(fixture->thread);
    g_object_unref(fixture->listener);
    g_bytes_unref(fixture->reply);
    g_assert_cmpint(g_unlink(fixture->path), ==, 0);
    g_assert_cmpint(g_rmdir(fixture->directory), ==, 0);
    g_free(fixture->path);
    g_free(fixture->directory);
}

#endif
