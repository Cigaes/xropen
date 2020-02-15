xropen
======

Introduction
------------

xropen is a project for people who work a lot remotely (SSH) with a
forwarded X11 display that works reasonably fast but is not comfortable
enough. It allows to display locally a document on the remote host.

I have been using xropen myself for several years. I am publishing it now
because other people have told me they are interested. I am not making a lot
of effort making it into a clean package. Use it as it comes.

Installation
------------

The command used to open files locally is hardcoded in the source code. Edit
`xropen-server.c` and change `open_command` if necessary.

Build using a simple make command; it requires the X11 development
libraries. The result is two binary files, `xropen` and `xropen-server`.
There is no make install.

Usage
-----

Run `xropen-server` locally. It is a background process without a window, it
will exit when the X11 server is terminated. It can be launched from the X11
start scripts.

On a remote host, to view a file, run `xropen` with the file name as
argument. It will print a progress percentage while transferring the file
`xropen-server`, and then `xropen-server` will open it and `xropen` exits.

The MIME type of the file can be specified with the `-t` option. The
progress percentage can be disabled with `-q`.

`xropen-server` will save the file in `/tmp` with a name
`xropen-date-hour-num-orig.ext` and will use a hardcoded command to open it. The default hardcoded command is:

```
see "${2:+$2:}$1" && rm "$1" || xmessage "Could not open $1"
```

It uses the `see` command from Debian's `mime-support` package. `$1` is the
name of the temp file, `$2` is the MIME type if given.

`xropen` can be used from mail user agents with lines in the `~/.mailcap`
file (using the `$NO_REMOTE_SEE` variable to inhibit it):

```
application/*; xropen -t %t %s; test=test -n "$DISPLAY" -a -z "$NO_REMOTE_SEE"
image/*; xropen -t %t %s; test=test -n "$DISPLAY" -a -z "$NO_REMOTE_SEE"
```

Bugs
----

`xropen-server` makes no effort to ensure the privacy of the temp files. Use
its umask for that.

Sometimes, the transfer freezes before starting, it is quite obvious.
Killing `xropen-server` and re-starting is enough to make it work again. The
bug is too sporadic, I have not yet had time to investigate.

Copyright
---------

Copyright © 2012–2020 Nicolas George

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2.0 as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
