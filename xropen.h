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

extern const char *program_name;

extern xcb_connection_t *display;

struct ropen_atoms {
    xcb_atom_t xropen;
    xcb_atom_t timestamp;
    xcb_atom_t data;
    xcb_atom_t file_name;
    xcb_atom_t content_type;
    xcb_atom_t size;
    xcb_atom_t error;
};

extern struct ropen_atoms atom;

void start_display(void);
void *calloc_safe(size_t n, size_t s);
void set_property_string(xcb_window_t win, xcb_atom_t atom, const char *str);
char *copy_string_prop(xcb_get_property_reply_t *prop);
