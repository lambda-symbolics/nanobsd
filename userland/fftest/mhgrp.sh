#!/bin/sh
XDG_RUNTIME_DIR=/var/run/user/1000 /usr/local/bin/mahoganyctl "(mahogany::state-select-group mahogany::*compositor-state* $1)"
