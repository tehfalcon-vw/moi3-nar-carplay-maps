#!/bin/sh
# Entry point used by compatible MOI3 LG engineering post-script containers.
set -u
USB_DIR=""
for path in /tmp/USB/PORT1/PART1 /tmp/USB/PORT2/PART1 /media/usb0 /media/usb1 /mnt/usb; do
    [ -f "$path/fix_hw_partnumber.sh" ] && USB_DIR="$path" && break
done
[ -n "$USB_DIR" ] || exit 0

if [ -f "$USB_DIR/REVERT.txt" ]; then
    /bin/sh "$USB_DIR/moi3-nar/scripts/uninstall.sh" > "$USB_DIR/moi3nar-revert.log" 2>&1
elif [ -f "$USB_DIR/COLLECT_LOGS.txt" ]; then
    /bin/sh "$USB_DIR/moi3-nar/scripts/collect.sh" "$USB_DIR" > "$USB_DIR/moi3nar-collect.log" 2>&1
elif [ -f "$USB_DIR/ENABLE_ALTSCREEN.txt" ]; then
    PAYLOAD_DIR="$USB_DIR/moi3-nar/payload" /bin/sh "$USB_DIR/moi3-nar/scripts/install.sh" > "$USB_DIR/moi3nar-install.log" 2>&1
else
    echo "No action marker found" > "$USB_DIR/moi3nar-no-action.log"
fi
sync
exit 0
