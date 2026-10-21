#!/bin/sh
# extract.sh HDAUDIO_C: print the kernel functions the host test runs, verbatim.
# A function is its return-type line, its "name(" line and everything to the
# next line that is just "}".
src=${1:?usage: extract.sh path/to/hdaudio.c}
for fn in hdaudio_corb_enqueue hdaudio_rirb_next hdaudio_unsol_queue \
    hdaudio_rirb_drain hdaudio_command_locked hdaudio_stream_elapsed; do
	awk -v fn="$fn" '
		{ line[NR] = $0 }
		$0 ~ "^" fn "\\(" { start = NR - 1; inside = 1 }
		inside && /^}$/ { for (i = start; i <= NR; i++) print line[i]; print ""; exit }
	' "$src"
done
