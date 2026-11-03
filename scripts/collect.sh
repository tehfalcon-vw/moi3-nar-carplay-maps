#!/bin/sh
set -u
OUT=${1:-}
[ -n "$OUT" ] || { echo "usage: collect.sh /path/to/usb"; exit 1; }
DEST="$OUT/MOI3_NAR_LOGS/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$DEST" || exit 1
cp -f /data/lge/smartlink/carplay/carplay-handler.log "$DEST/" 2>/dev/null || true
cp -f /data/lge/smartlink/carplay/moi3nar-install.log "$DEST/" 2>/dev/null || true
cp -f /data/lge/smartlink/carplay/moi3nar-wrapper.log "$DEST/" 2>/dev/null || true
cp -f /data/lge/smartlink/carplay/server_info_v62.plist "$DEST/" 2>/dev/null || true
journalctl -u carplay.service --no-pager -n 500 > "$DEST/journal-carplay.txt" 2>&1 || true
ip -6 addr > "$DEST/ip6-address.txt" 2>&1 || true
ip -6 route > "$DEST/ip6-route.txt" 2>&1 || true
sha256sum /lge/app_rw/smartlink/carplay/carplay* /lge/app_rw/lib64/libAirPlay.so /lge/app_rw/lib64/libCoreUtils.so > "$DEST/fingerprints.txt" 2>&1 || true
dmesg | tail -n 500 > "$DEST/dmesg.txt" 2>&1 || true
sync
echo "COLLECTED: $DEST"
