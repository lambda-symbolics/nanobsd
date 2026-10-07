#!/bin/sh
# Build /usr/local/bin/lfetch: an SBCL executable with cl-colorist.
# Run as root on the laptop from this directory.
set -e
here=$(cd "$(dirname "$0")" && pwd)
SBCL=${SBCL:-/usr/local/bin/sbcl}
COLORIST=${COLORIST:-/usr/local/lib/cclsh/src/cl-colorist}
OUT=${OUT:-/usr/local/bin/lfetch}
"$SBCL" --non-interactive --no-userinit \
  --eval '(require :asdf)' \
  --eval "(asdf:load-asd #p\"$COLORIST/cl-colorist.asd\")" \
  --eval "(asdf:load-asd #p\"$here/lfetch.asd\")" \
  --eval '(asdf:load-system :lfetch)' \
  --eval "(sb-ext:save-lisp-and-die \"$OUT.new\" :toplevel (lambda () (lfetch:main) (sb-ext:exit)) :executable t :save-runtime-options t)"
mv "$OUT.new" "$OUT"
ls -l "$OUT"
