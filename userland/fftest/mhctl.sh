#!/bin/sh
# mhctl.sh FORM: evaluate FORM in Mahogany (for su mag -c, whose shell is cclsh)
XDG_RUNTIME_DIR=/var/run/user/1000 exec /usr/local/bin/mahoganyctl "$1"
