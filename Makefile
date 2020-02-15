# Copyright (c) 2012-2020 Nicolas George
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# version 2.0 as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

CFLAGS  = -Wall -Wextra -Wno-pointer-sign -std=c99 -D_XOPEN_SOURCE=600 -g -O2
LDFLAGS =
LIBS    = -lxcb

all: xropen xropen-server

XROPEN        = xropen.o        common.o
XROPEN_SERVER = xropen-server.o common.o

xropen: $(XROPEN)
	$(CC) $(LDFLAGS) -o $@ $(XROPEN) $(LIBS)

xropen-server: $(XROPEN_SERVER)
	$(CC) $(LDFLAGS) -o $@ $(XROPEN_SERVER) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

$(XROPEN) $(XROPEN_SERVER): xropen.h

clean:
	rm -f xropen xropen-server $(XROPEN) $(XROPEN_SERVER)
