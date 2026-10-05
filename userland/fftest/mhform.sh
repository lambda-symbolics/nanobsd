#!/bin/sh
# mhform.sh FILE: evaluate the Lisp form in FILE inside Mahogany
XDG_RUNTIME_DIR=/var/run/user/1000 /usr/local/bin/mahoganyctl "$(cat "$1")"
