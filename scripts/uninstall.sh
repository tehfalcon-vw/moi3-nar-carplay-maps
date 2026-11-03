#!/bin/sh
set -u
CARPLAY_DIR=/lge/app_rw/smartlink/carplay
DATA_DIR=/data/lge/smartlink/carplay
TARGET="$CARPLAY_DIR/carplay"
STOCK="$CARPLAY_DIR/carplay.moi3nar.stock"
EXPECTED=6d32486f5ec302c55ccf73b9884e20b1cf6d2773174abfe56245b2d3c7f4ce16

[ "$(id -u 2>/dev/null)" = 0 ] || exit 1
if [ -f "$STOCK" ]; then
    actual=$(sha256sum "$STOCK" 2>/dev/null | awk '{print $1}')
    [ "$actual" = "$EXPECTED" ] || { echo "ABORTED: preserved stock hash mismatch"; exit 1; }
    for mp in /lge/app_rw /lge /; do mount -o remount,rw "$mp" 2>/dev/null || true; done
    mv -f "$STOCK" "$TARGET" || exit 1
    chmod 0755 "$TARGET"
    command -v chsmack >/dev/null 2>&1 && chsmack -a smartlink::app -e carplay::app "$TARGET" 2>/dev/null || true
fi
rm -f "$CARPLAY_DIR/moi3nar_altscreen.so" "$DATA_DIR/moi3nar.enable" "$DATA_DIR/moi3nar.bootid" "$DATA_DIR/MOI3NAR_DISABLE"
sync
systemctl restart carplay.service 2>/dev/null || true
echo "UNINSTALLED: stock CarPlay restored"
