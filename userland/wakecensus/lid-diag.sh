#!/bin/sh
# lid-diag.sh: why did the last lid close not suspend?  Run on the laptop.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/local/sbin:/usr/local/bin
echo "== kernel: $(sysctl -n machdep.booted_kernel)   $(uptime)"
echo "== sleep sysctls"; sysctl hw.acpi.sleep 2>/dev/null | grep -v 'states\|state ='
echo "== last s2idle messages"; dmesg | grep -E 's2idle|filesystem barrier|FAILED|idle_d3|acpi power' | tail -40
echo "== LWPs not suspended-capable (sleeping uninterruptibly or stopped)"
ps -axlo pid,stat,wchan,command | awk '$2 ~ /D|T/' | head
echo "== mounts"; mount | sed 's/, / /g' | cut -c1-100
echo "== usb/bt"; usb status 2>&1; drvctl -l pci0 2>/dev/null | grep xhci; sysctl -n hw.acpi.sleep.idle_d3
