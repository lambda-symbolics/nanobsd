#!/bin/sh
# codecdump [codecid] : dump codec state through raw verbs. Read-only except SET_COEF_INDEX (0x500),
# which only selects which coefficient GET_PROC_COEF reads.
PATH=/sbin:/usr/sbin/:/bin:/usr/bin; export PATH
C=${1:-0}; V=/var/tmp/hdaverb
sub=$($V $C 1 0xF00 4); start=$(( (0x$sub >> 16) & 0xff )); n=$(( 0x$sub & 0xff ))
echo "afg nid 1: subnodes start=$start n=$n power=$($V $C 1 0xF05 0) gpio data=$($V $C 1 0xF15 0) mask=$($V $C 1 0xF16 0) dir=$($V $C 1 0xF17 0) gpiocnt=$($V $C 1 0xF00 0x11)"
i=$start; while [ $i -lt $((start+n)) ]; do
  cap=$($V $C $i 0xF00 9); type=$(( (0x$cap >> 20) & 0xf ))
  line="nid $(printf 0x%02x $i) type=$type cap=$cap pwr=$($V $C $i 0xF05 0) connsel=$($V $C $i 0xF01 0) proc=$($V $C $i 0xF03 0)"
  case $type in
    0|1) line="$line fmt=$($V $C $i 0xA00 0) strm=$($V $C $i 0xF06 0) ampoutL=$($V $C $i 0xB00 0xA000) ampoutR=$($V $C $i 0xB00 0x8000) ampinL0=$($V $C $i 0xB00 0x2000) ampinR0=$($V $C $i 0xB00 0x0000)" ;;
    2|3) line="$line ampoutL=$($V $C $i 0xB00 0xA000) ampoutR=$($V $C $i 0xB00 0x8000)"; k=0; while [ $k -lt 8 ]; do line="$line in${k}=$($V $C $i 0xB00 $((0x2000+k)))/$($V $C $i 0xB00 $k)"; k=$((k+1)); done ;;
    4) line="$line pinctl=$($V $C $i 0xF07 0) eapd=$($V $C $i 0xF0C 0) cfg=$($V $C $i 0xF1C 0) sense=$($V $C $i 0xF09 0) ampoutL=$($V $C $i 0xB00 0xA000) ampoutR=$($V $C $i 0xB00 0x8000) ampinL0=$($V $C $i 0xB00 0x2000)" ;;
    7) line="$line beep=$($V $C $i 0xF0A 0)" ;;
  esac
  echo "$line"; i=$((i+1))
done
echo "--- vendor coefficients nid 0x20 (index: value)"
k=0; while [ $k -lt 128 ]; do $V $C 0x20 0x500 $k >/dev/null; printf '%02x:%s ' $k "$($V $C 0x20 0xC00 0)"; k=$((k+1)); done; echo
