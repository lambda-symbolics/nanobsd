#!/bin/sh
# fftree.sh PARENT SECS: exact CPU of a Firefox instance (parent + all descendants) and Mahogany
P=$1 S=${2:-10}
kids=$(ps -axo pid= -o ppid= | awk -v p=$P '{pp[$1] = $2}
  END { for (x in pp) { y = x; n = 0; while (y > 1 && n++ < 50) { y = pp[y]; if (y == p) { print x; break } } } }')
M=$(ps -axo pid,comm | awk '$2 ~ /mahogany$/ {print $1; exit}')
for x in $P $kids $M; do /var/tmp/lwptime $x $S > /tmp/lt.$x 2>/dev/null & done; wait
for x in $P $kids $M; do t=$(awk '/total/ {print $1}' /tmp/lt.$x); case $t in 0.00%|"") continue ;; esac
  echo "== $x $(ps -o args= -p $x | cut -c1-50) $t"; sort -nr /tmp/lt.$x | grep -v total | head -5; done
