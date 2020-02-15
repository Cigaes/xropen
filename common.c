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
#include <time.h>
#include <assert.h>
#include <xcb/xcb.h>

#include "xropen.h"

#define N_ATOMS 7

xcb_connection_t *display;
struct ropen_atoms atom;

void
start_display(void)
{
    static const char *const name[] = {
        "XROPEN", "TIMESTAMP", "DATA", "FILE-NAME", "CONTENT-TYPE", "SIZE",
        "ERROR",
    };
    xcb_intern_atom_cookie_t atom_cookie[N_ATOMS];
    xcb_intern_atom_reply_t *r;
    int i;

    assert(sizeof(atom) == N_ATOMS * sizeof(xcb_atom_t));
    assert(sizeof(name) == N_ATOMS * sizeof(char *));
    if ((display = xcb_connect(NULL, NULL)) == NULL) {
        fprintf(stderr, "%s: unable to open display.\n", program_name);
        exit(1);
    }

    for (i = 0; i < N_ATOMS; i++)
        atom_cookie[i] = xcb_intern_atom(display, 0, strlen(name[i]), name[i]);
    for (i = 0; i < N_ATOMS; i++) {
        r = xcb_intern_atom_reply(display, atom_cookie[i], NULL);
        if (r == NULL) {
            fprintf(stderr, "%s: unable to create atom.\n", program_name);
            exit(1);
        }
        ((xcb_atom_t *)&atom)[i] = r->atom;
        free(r);
    }
}

void *
calloc_safe(size_t n, size_t s)
{
    void *r;

    if ((r = calloc(n, s)) == NULL) {
        fprintf(stderr, "%s: out of memory.\n", program_name);
        exit(1);
    }
    return r;
}

void
set_property_string(xcb_window_t win, xcb_atom_t atom, const char *str)
{
    xcb_change_property(display, XCB_PROP_MODE_REPLACE, win,
        atom, XCB_ATOM_STRING, 8, strlen(str), str);
}

char *
copy_string_prop(xcb_get_property_reply_t *prop)
{
    char *r;

    if (prop == NULL || prop->format == 0)
        return NULL;
    r = calloc_safe(1, prop->value_len + 1);
    memcpy(r, xcb_get_property_value(prop), prop->value_len);
    return r;
}
