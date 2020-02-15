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
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <xcb/xcb.h>

#include "xropen.h"

#define MAX_CLIENTS 16

#define MAX_DATA_SIZE (16 * 1024 * 1024)

#define MAX_TEMP_DIR      4096
#define MAX_FILE_BASENAME   80
#define MAX_FILE_EXT        16
#define MAX_FILE_INDEX     100

struct xropen_client {
    xcb_window_t window;
    char *file_name;
    char *file_type;
    off_t file_size;
    off_t file_pos;
    FILE *file;
    uint64_t last_activity;
};

const char *program_name = "xropen-server";

static char *open_command = "see \"${2:+$2:}$1\" && "
                            "rm \"$1\" || "
                            "xmessage \"Could not open $1\"";
static char *temp_dir     = "/tmp";

static xcb_window_t server;
static struct xropen_client all_clients[MAX_CLIENTS];
static unsigned all_clients_size = 0;

static uint64_t
get_time(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void
create_window(void)
{
    xcb_screen_t *screen;
    uint64_t timestamp = get_time();
    uint32_t timestamp_dec[2];
    uint32_t values[3];

    screen = xcb_setup_roots_iterator(xcb_get_setup(display)).data;
    server = xcb_generate_id(display);
    values[0] = screen->black_pixel;
    values[1] = 1; /* override redirect */
    values[2] = 0; /* events */
    xcb_create_window(display, screen->root_depth, server, screen->root,
        0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
        screen->root_visual,
        XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
        values);

    timestamp_dec[0] = timestamp & 0xFFFFFFFF;
    timestamp_dec[1] = timestamp >> 32;
    xcb_change_property(display, XCB_PROP_MODE_REPLACE, server,
        XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
        strlen(program_name), program_name);
    xcb_change_property(display, XCB_PROP_MODE_REPLACE, server,
        atom.xropen, atom.timestamp, 32, 2, timestamp_dec);

    xcb_flush(display);
}

static void
remove_if_same(char *name, FILE *f)
{
    struct stat st1, st2;

    if (stat(name, &st1) < 0 || fstat(fileno(f), &st2) < 0 ||
        st1.st_ino != st2.st_ino)
        return;
    unlink(name);
}

static void
close_client(struct xropen_client *client)
{
    if (client->file != NULL) {
        remove_if_same(client->file_name, client->file);
        fclose(client->file);
    }
    free(client->file_name);
    free(client->file_type);
    all_clients_size--;
    while (client < all_clients + all_clients_size)
        client[0] = client[1];
}

static void
kill_non_client(xcb_window_t window, const char *msg)
{
    (void)window;
    (void)msg;
    if (msg == NULL) {
        msg = strerror(errno);
        if (msg == NULL)
            msg = "unknown error";
    }
    set_property_string(window, atom.error, msg);
}

static void
kill_client(struct xropen_client *client, const char *msg)
{
    kill_non_client(client->window, msg);
    close_client(client);
}

static char *
get_next_word(char **cur, char *end, size_t *rlen)
{
    char *r;

    for (; *cur < end && isspace(**cur); (*cur)++);
    if (*cur == end)
        return NULL;
    r = *cur;
    for (; *cur < end && !isspace(**cur); (*cur)++);
    *rlen = *cur - r;
    return r;
}

static int
memcasecmp(char *a, char *b, size_t len)
{
    for (; len > 0; len--, a++, b++)
        if (tolower(*a) != tolower(*b))
            return 1;
    return 0;
}

static void
check_file_extension(char *name, char *type, char *rext, char **name_end)
{
    size_t name_len = strlen(name);
    size_t type_len;
    FILE *mt;
    char line[4096], *line_end, *line_cur;
    char *mtype, *mext;
    size_t mtype_len, mext_len;
    int match;

    *rext = 0;
    *name_end = name + name_len;
    if (type == NULL)
        return;

    type_len = strlen(type);
    if ((mt = fopen("/etc/mime.types", "r")) == NULL) {
        perror("/etc/mime.types");
        return;
    }
    while (fgets(line, sizeof(line), mt) != NULL) {
        line_end = line + strlen(line);
        line_cur = line;
        mtype = get_next_word(&line_cur, line_end, &mtype_len);
        if (mtype == NULL || *mtype == '#' ||
            type_len != mtype_len || memcasecmp(type, mtype, type_len))
            continue;
        while ((mext = get_next_word(&line_cur, line_end, &mext_len)) != NULL) {
            if (mext_len > MAX_FILE_EXT)
                continue;
            match = name_len >= mext_len + 2 &&
                name[name_len - mext_len - 1] == '.' &&
                !memcasecmp(name + name_len - mext_len, mext, mext_len);
            if (*rext == 0 || match) {
                rext[0] = '.';
                memcpy(rext + 1, mext, mext_len);
                rext[mext_len + 1] = 0;
            }
            if (match) {
                *name_end = name + name_len - mext_len - 1;
                goto finished;
            }
        }
    }
finished:
    fclose(mt);
}

static int
is_safe_char(char c)
{
    return ((c | 32) >= 'a' && (c | 32) <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

static void
open_temp_file(struct xropen_client *client, char *name, char *type)
{
    char filename[MAX_TEMP_DIR + MAX_FILE_BASENAME + MAX_FILE_EXT + 128];
    char *filename_end = filename + sizeof(filename);
    char ext[MAX_FILE_EXT + 2];
    char *name_end;
    char *temp_end, *p, *base_end;
    unsigned i, len;
    time_t now;
    struct tm *tm;
    int fd;

    check_file_extension(name, type, ext, &name_end);
    if (name_end - name > MAX_FILE_BASENAME)
        name_end = name + MAX_FILE_BASENAME;

    temp_end = filename + strlen(temp_dir);
    memcpy(filename, temp_dir, temp_end - filename);
    *(temp_end++) = '/';

    time(&now);
    tm = localtime(&now);
    temp_end += snprintf(temp_end, filename_end - temp_end,
        "xropen-%04d%02d%02d-%02d%02d%02d-XX-",
        tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
        tm->tm_hour, tm->tm_min, tm->tm_sec);

    for (p = name, base_end = temp_end; p < name_end; p++, base_end++)
        *base_end = is_safe_char(*p) ? *p : '_';
    memcpy(base_end, ext, sizeof(ext));

    for (i = 0; i < MAX_FILE_INDEX; i++) {
        temp_end[-3] = '0' + i / 10;
        temp_end[-2] = '0' + i % 10;
        if ((fd = open(filename, O_WRONLY | O_CREAT | O_EXCL, 0666)) >= 0)
            break;
    }
    if (fd < 0) {
        kill_client(client, NULL);
        return;
    }
    len = strlen(filename);
    client->file_name = calloc_safe(1, len + 1);
    memcpy(client->file_name, filename, len + 1);
    if ((client->file = fdopen(fd, "w")) == NULL) {
        kill_client(client, NULL);
        return;
    }
}

static void
start_client(xcb_window_t window)
{
    struct xropen_client *client;
    xcb_get_property_cookie_t cookie_name, cookie_type, cookie_size;
    xcb_get_property_reply_t *prop_name, *prop_type, *prop_size;
    uint32_t *size_val;
    off_t size;
    char *name = NULL, *type = NULL;
    uint32_t events[1] = { XCB_EVENT_MASK_PROPERTY_CHANGE |
        XCB_EVENT_MASK_STRUCTURE_NOTIFY};

    if (all_clients_size == MAX_CLIENTS) {
        kill_non_client(window, "too many clients");
        return;
    }
    client = &all_clients[all_clients_size++];

    client->window = window;
    cookie_name = xcb_get_property(display, 0, client->window,
        atom.file_name, XCB_ATOM_STRING, 0, FILENAME_MAX);
    cookie_type = xcb_get_property(display, 0, client->window,
        atom.content_type, XCB_ATOM_STRING, 0, FILENAME_MAX);
    cookie_size = xcb_get_property(display, 0,
        client->window, atom.size, XCB_ATOM_INTEGER, 0, 2);

    prop_name = xcb_get_property_reply(display, cookie_name, NULL);
    prop_type = xcb_get_property_reply(display, cookie_type, NULL);
    prop_size = xcb_get_property_reply(display, cookie_size, NULL);

    if (prop_size == NULL ||
        prop_size->type != XCB_ATOM_INTEGER  ||
        prop_size->format != 32 ||
        prop_size->value_len < 1 || prop_size->value_len > 2)
        goto fail;
    if (prop_name != NULL && prop_type->format != 0 &&
        (prop_name->type != XCB_ATOM_STRING || prop_name->format != 8))
        goto fail;
    if (prop_type != NULL && prop_type->format != 0 &&
        (prop_type->type != XCB_ATOM_STRING || prop_type->format != 8))
        goto fail;

    size_val = xcb_get_property_value(prop_size);
    size = size_val[0];
    if (prop_size->value_len > 1)
        size += (off_t)size_val[1] << 32;
    name = copy_string_prop(prop_name);
    type = copy_string_prop(prop_type);

    open_temp_file(client, name, type);
    free(name);
    xcb_change_window_attributes(display, client->window,
        XCB_CW_EVENT_MASK, events);
    xcb_delete_property(display, client->window, atom.data);
    xcb_flush(display);

    client->file_type     = type;
    client->file_size     = size;
    client->file_pos      = 0;
    client->last_activity = get_time();

fail:
    free(prop_name);
    free(prop_type);
    free(prop_size);
}

static void
open_file(struct xropen_client *client)
{
    posix_spawnattr_t attr;
    pid_t child;
    char *cmd[] = { "sh", "-c", open_command, "sh",
        client->file_name,
        client->file_type == NULL ? NULL : client->file_type,
        NULL };
    extern char **environ; /* ??? */

    fclose(client->file);
    client->file = NULL;

    if (posix_spawnattr_init(&attr) < 0) {
        kill_client(client, "posix_spawnattr_init");
        return;
    }
    if (posix_spawnattr_setpgroup(&attr, 0) < 0) {
        posix_spawnattr_destroy(&attr);
        kill_client(client, "posix_spawnattr_setpgroup");
        return;
    }
    if (posix_spawn(&child, "/bin/sh", NULL, &attr, cmd, environ) < 0) {
        posix_spawnattr_destroy(&attr);
        kill_client(client, "posix_spawn");
        return;
    }
    posix_spawnattr_destroy(&attr);
    close_client(client);
}

struct xropen_client *
find_client(xcb_window_t window)
{
    unsigned i;

    for (i = 0; i < all_clients_size; i++)
        if (all_clients[i].window == window)
            return &all_clients[i];
    return NULL;
}

static void
handle_property_change(xcb_property_notify_event_t *ev)
{
    struct xropen_client *client = NULL;
    xcb_get_property_cookie_t cookie;
    xcb_get_property_reply_t *prop;
    uint8_t *data;
    unsigned size;
    off_t miss;

    if (ev->atom != atom.data || ev->state != XCB_PROPERTY_NEW_VALUE ||
        (client = find_client(ev->window)) == NULL)
        return;

    miss = client->file_size - client->file_pos;
    if (miss <= 0) {
        kill_client(client, "invalid data packet");
        return;
    }
    if (miss > MAX_DATA_SIZE)
        miss = MAX_DATA_SIZE;
    miss = (miss + 3) / 4;
    cookie = xcb_get_property(display, 0, client->window, atom.data, atom.data,
        0, miss);
    prop = xcb_get_property_reply(display, cookie, NULL);
    if (prop == NULL || prop->type != atom.data || prop->format != 8 ||
        prop->bytes_after > 0) {
        free(prop);
        kill_client(client, "invalid data property");
        return;
    }

    size = prop->value_len;
    data = xcb_get_property_value(prop);
    if (fwrite(data, 1, size, client->file) != size) {
        kill_client(client, NULL);
        return;
    }
    free(prop);

    xcb_delete_property(display, client->window, atom.data);
    xcb_flush(display);

    client->file_pos += size;
    if (client->file_pos == client->file_size)
        open_file(client);
}

static void
handle_destroy(xcb_destroy_notify_event_t *ev)
{
    struct xropen_client *client;

    if ((client = find_client(ev->window)) == NULL)
        return;
    close_client(client);
}

static void
handle_client_message(xcb_client_message_event_t *ev)
{
    if (ev->format != 32 || ev->window != server)
        return;
    start_client(ev->data.data32[0]);
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    xcb_generic_event_t *ev;

    signal(SIGCHLD, SIG_IGN);
    start_display();
    create_window();
    while ((ev = xcb_wait_for_event(display)) != NULL) {
        switch (ev->response_type & ~0x80) {
            case XCB_CLIENT_MESSAGE:
                handle_client_message((xcb_client_message_event_t *)ev);
                break;

            case XCB_PROPERTY_NOTIFY:
                handle_property_change((xcb_property_notify_event_t *)ev);
                break;

            case XCB_DESTROY_NOTIFY:
                handle_destroy((xcb_destroy_notify_event_t *)ev);
                break;

            default:
                fprintf(stderr, "%s: unknown event type %d\n", program_name,
                    ev->response_type);
                break;
        }
        free(ev);
    }
    return 0;
}
