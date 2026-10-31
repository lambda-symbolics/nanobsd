#!/bin/sh
# ffsym.sh PROF PMAP NAMES: aggregate dtrace PC samples by thread, library, and libxul symbol
PROF=$1 PMAP=$2 NAMES=$3
L=/usr/pkgsrc/www/firefox/work/build/dist/bin/libxul.so
awk -v pmap=$PMAP -v names=$NAMES '
function hex(s,   i,c,v) { v = 0; s = tolower(s); for (i = 1; i <= length(s); i++) { c = index("0123456789abcdef", substr(s, i, 1)) - 1; v = v * 16 + c }; return v }
BEGIN {
  n = 0
  while ((getline l < pmap) > 0) { split(l, f, " "); if (f[4] == "") continue; st = hex(f[1]); sz = f[2]; sub(/K/, "", sz); lo[n] = st; hi[n] = st + sz * 1024; lib[n] = f[4]; sub(/.*\//, "", lib[n]); if (!(lib[n] in base) || st < base[lib[n]]) base[lib[n]] = st; n++ }
  while ((getline l < names) > 0) { split(l, a, " "); t = a[1]; sub(/^ *[0-9]+ */, "", l); nm[t] = l }
}
NF == 4 {
  pc = hex($2); w = $4; tot += w; tn = nm[$1]; if (tn == "") tn = $1
  if ($3 == 1) { L2 = "[kernel]" } else { L2 = "?"; for (i = 0; i < n; i++) if (pc >= lo[i] && pc < hi[i]) { L2 = lib[i]; break } }
  bylib[L2] += w; bythr[tn] += w
  if (L2 == "libxul.so") printf "%x %d\n", pc - base["libxul.so"], w > "/var/tmp/ffsym.xul"
}
END {
  print "samples", tot
  for (k in bylib) printf "%6.1f%% %s\n", 100 * bylib[k] / tot, k | "sort -nr | head -12"; close("sort -nr | head -12")
  print "--"
  for (k in bythr) printf "%6.1f%% %s\n", 100 * bythr[k] / tot, k | "sort -nr | head -12"; close("sort -nr | head -12")
}' $PROF
echo "-- libxul symbols"
[ -s /var/tmp/ffsym.xul ] && awk '{print $1}' /var/tmp/ffsym.xul | sed 's/^/0x/' | addr2line -f -C -e $L | awk 'NR % 2 == 1' > /var/tmp/ffsym.names
paste /var/tmp/ffsym.names /var/tmp/ffsym.xul | awk -F'\t' '{split($2, a, " "); s[substr($1, 1, 90)] += a[2]; t += a[2]} END {for (k in s) printf "%5d %s\n", s[k], k}' | sort -nr | head -25
rm -f /var/tmp/ffsym.xul
