/*
 * Copyright (c) 2012-2020 Nicolas George
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2.0 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <xcb/xcb.h>

#include "xropen.h"

const char *program_name = "xropen";

static int option_quiet = 0;

struct xropen_connection {
    xcb_window_t server;
    xcb_window_t client;
    char *file_name;
    char *file_base;
    char *file_type;
    off_t file_size;
    off_t file_pos;
    FILE *file;
};

static void
usage(int code)
{
    fprintf(code ? stderr : stdout,
        "Usage: %s [-q] [-t mime/type] file\n", program_name);
    exit(code);
}

static void
die_system_error(const char *comment)
{
    fprintf(stderr, "%s: %s: %s\n", program_name, comment, strerror(errno));
    exit(1);
}

static void
find_server(struct xropen_connection *conn)
{
    xcb_screen_t *screen;
    xcb_query_tree_reply_t *tree;
    unsigned i, n_children;
    xcb_window_t *children;
    xcb_get_property_cookie_t *cookie;
    xcb_get_property_reply_t *prop;
    xcb_window_t found = XCB_NONE;

    screen = xcb_setup_roots_iterator(xcb_get_setup(display)).data;
    tree = xcb_query_tree_reply(display,
        xcb_query_tree(display, screen->root), NULL);
    if (tree == NULL) {
        fprintf(stderr, "%s: unable to get toplevel windows.\n", program_name);
        exit(1);
    }
    n_children = xcb_query_tree_children_length(tree);
    children = xcb_query_tree_children(tree);
    cookie = calloc_safe(n_children, sizeof(*cookie));
    for (i = 0; i < n_children; i++)
        cookie[i] = xcb_get_property(display, 0, children[i],
            atom.xropen, atom.timestamp, 0, 2);
    for (i = 0; i < n_children; i++) {
        prop = xcb_get_property_reply(display, cookie[i], NULL);
        if (prop == NULL)
            continue;
        if (prop->type == atom.timestamp && prop->format == 32 &&
            prop->value_len == 2) {
            /* TODO compare timestamps */
            found = children[i];
        }
        free(prop);
    }
    free(cookie);
    free(tree);
    if (found == XCB_NONE) {
        fprintf(stderr, "%s: no server found.\n", program_name);
        exit(1);
    }
    conn->server = found;
}

static void
set_data_property(struct xropen_connection *conn, uint8_t *data, unsigned size)
{
    xcb_change_property(display, XCB_PROP_MODE_REPLACE, conn->client,
        atom.data, atom.data, 8, size, data);
}

static void
create_window(struct xropen_connection *conn)
{
    xcb_screen_t *screen;
    uint32_t size[2];
    uint32_t events[1] = { XCB_EVENT_MASK_PROPERTY_CHANGE };

    screen = xcb_setup_roots_iterator(xcb_get_setup(display)).data;
    conn->client = xcb_generate_id(display);
    xcb_create_window(display, screen->root_depth, conn->client, conn->server,
        0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
        screen->root_visual, XCB_CW_EVENT_MASK, events);

    set_property_string(conn->client, XCB_ATOM_WM_NAME, program_name);
    set_property_string(conn->client, atom.file_name, conn->file_base);
    if (conn->file_type != NULL)
        set_property_string(conn->client, atom.content_type, conn->file_type);
    size[0] = (uint64_t)conn->file_size & 0xFFFFFFFF;
    size[1] = (uint64_t)conn->file_size >> 32;
    xcb_change_property(display, XCB_PROP_MODE_REPLACE, conn->client,
        atom.size, XCB_ATOM_INTEGER, 32, size[1] != 0 ? 2 : 1, size);
    set_data_property(conn, NULL, 0);

    xcb_flush(display);
}

static void
ping_server(struct xropen_connection *conn)
{
    xcb_client_message_event_t event = {
        .response_type  = XCB_CLIENT_MESSAGE | 0x80,
        .format         = 32,
        .window         = conn->server,
        .type           = atom.xropen,
        .data.data32[0] = conn->client,
    };

    xcb_send_event(display, 0, conn->server,
        XCB_EVENT_MASK_NO_EVENT, (char *)&event);
    xcb_flush(display);
}

static void
print_progress(struct xropen_connection *conn)
{
    int shift = conn->file_size > ((off_t)1 << (sizeof(off_t) * 8 - 8)) ? 8 : 0;
    int progress = 100 * (conn->file_pos >> shift) / (conn->file_size >> shift);

    if (option_quiet)
        return;
    printf("\r%.64s: %3d%% ", conn->file_base, progress);
    fflush(stdout);
}

static void
handle_data_delete(struct xropen_connection *conn)
{
    uint8_t buf[16384];
    unsigned r;

    if ((r = fread(buf, 1, sizeof(buf), conn->file)) == 0) {
        if (!option_quiet) {
            printf("\r%72s\r", "");
            fflush(stdout);
        }
        fclose(conn->file);
        conn->file = NULL;
        return;
    }
    print_progress(conn);
    set_data_property(conn, buf, r);
    xcb_flush(display);
    conn->file_pos += r;
}

static void
handle_error(struct xropen_connection *conn)
{
    xcb_get_property_cookie_t cookie;
    xcb_get_property_reply_t *prop;
    char *msg = "unknown";

    cookie = xcb_get_property(display, 0, conn->client, atom.error,
        XCB_ATOM_STRING, 0, sizeof(msg) / 4);
    prop = xcb_get_property_reply(display, cookie, NULL);
    if (prop != NULL && prop->type != atom.error && prop->format == 8)
        msg = copy_string_prop(prop);
    fprintf(stderr, "%s: remote error: %s\n", program_name, msg);
    free(prop);
    exit(1);
}

static void
handle_property_change(struct xropen_connection *conn,
    xcb_property_notify_event_t *ev)
{
    if (ev->window == conn->client && ev->atom == atom.data &&
        ev->state == XCB_PROPERTY_DELETE)
        handle_data_delete(conn);
    if (ev->window == conn->client && ev->atom == atom.error &&
        ev->state == XCB_PROPERTY_NEW_VALUE)
        handle_error(conn);
}

int
main(int argc, char **argv)
{
    int opt;
    struct xropen_connection conn = { 0 };
    char *p;
    xcb_generic_event_t *ev;

    while ((opt = getopt(argc, argv, "ht:q")) != -1) {
        switch (opt) {
            case 't':
                conn.file_type = optarg;
                break;
            case 'q':
                option_quiet++;
                break;
            case 'h':
                usage(0);
            default:
                usage(1);
        }
    }
    argc -= optind;
    argv += optind;
    if (argc == 0)
        usage(1);
    conn.file_name = argv[0];
    p = strrchr(conn.file_name, '/');
    conn.file_base = p == NULL ? conn.file_name : p + 1;

    if ((conn.file = fopen(conn.file_name, "r")) == NULL)
        die_system_error(conn.file_name);
    if ((fseeko(conn.file, 0, SEEK_END)) < 0)
        die_system_error(conn.file_name);
    conn.file_size = ftello(conn.file);
    if ((fseeko(conn.file, 0, SEEK_SET)) < 0)
        die_system_error(conn.file_name);

    start_display();
    find_server(&conn);
    create_window(&conn);
    ping_server(&conn);

    while ((ev = xcb_wait_for_event(display)) != NULL) {
        switch (ev->response_type & ~0x80) {
            case XCB_PROPERTY_NOTIFY:
                handle_property_change(&conn,
                    (xcb_property_notify_event_t *)ev);
                break;

            case 0:
                fprintf(stderr, "%s: unknown error\n", program_name);
                exit(1);

            default:
                fprintf(stderr, "%s: unknown event type %d\n", program_name,
                    ev->response_type);
                break;
        }
        free(ev);
        if (conn.file == NULL)
            break;
    }

    xcb_disconnect(display);
    return 0;
}
