#!/bin/sh
# One-boot CarPlay AltScreen trial for MOI3_EU_SE_R8410L / MU0841.
set -u

CARPLAY_DIR=/lge/app_rw/smartlink/carplay
LIB_DIR=/lge/app_rw/lib64
DATA_DIR=/data/lge/smartlink/carplay
STOCK="$CARPLAY_DIR/carplay.moi3nar.stock"
TARGET="$CARPLAY_DIR/carplay"
HANDLER="$CARPLAY_DIR/moi3nar_altscreen.so"
LOG="$DATA_DIR/moi3nar-install.log"
EXPECTED_CARPLAY=6d32486f5ec302c55ccf73b9884e20b1cf6d2773174abfe56245b2d3c7f4ce16
EXPECTED_AIRPLAY=1b4f62895abde292074748e621ee8e83c9deeda3e602d59543fa5edd1dce13cb
EXPECTED_SUPPORT=633c456ecc5c3cfb4ffe58693fb38f6c0db0ec763d407684406a5ea92ea97816
EXPECTED_CORE=f4ee85b42fe19caf3c9e6eabd027287798ad80afddc70d9e041fadffb273340c

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd)
PAYLOAD_DIR=${PAYLOAD_DIR:-"$SCRIPT_DIR/../payload"}

mkdir -p "$DATA_DIR" 2>/dev/null || exit 1
: > "$LOG"
log() { echo "$*" | tee -a "$LOG"; }
abort() { log "ABORTED: $*"; sync; exit 1; }
hash_file() { sha256sum "$1" 2>/dev/null | awk '{print $1}'; }
check_hash() {
    actual=$(hash_file "$1")
    [ "$actual" = "$2" ] || abort "unexpected $1 sha256=$actual"
}

[ "$(id -u 2>/dev/null)" = 0 ] || abort "installer must run as root"
[ -f "$PAYLOAD_DIR/altscreen_handler.so" ] || abort "payload missing"
[ -f "$PAYLOAD_DIR/altscreen_handler.so.sha256" ] || abort "payload checksum missing"
expected_handler=$(awk 'NR==1 {print $1}' "$PAYLOAD_DIR/altscreen_handler.so.sha256")
[ -n "$expected_handler" ] || abort "payload checksum unreadable"
check_hash "$PAYLOAD_DIR/altscreen_handler.so" "$expected_handler"

# If this is an update/re-arm, validate the preserved stock binary.
if [ -f "$STOCK" ]; then
    check_hash "$STOCK" "$EXPECTED_CARPLAY"
else
    check_hash "$TARGET" "$EXPECTED_CARPLAY"
fi
check_hash "$LIB_DIR/libAirPlay.so" "$EXPECTED_AIRPLAY"
check_hash "$LIB_DIR/libAirPlaySupport.so" "$EXPECTED_SUPPORT"
check_hash "$LIB_DIR/libCoreUtils.so" "$EXPECTED_CORE"

for mp in /lge/app_rw /lge /; do mount -o remount,rw "$mp" 2>/dev/null || true; done
[ -x /lge/app_ro/bin/remount_fs.sh ] && /lge/app_ro/bin/remount_fs.sh rw 2>/dev/null || true

mkdir -p "$DATA_DIR/backup" || abort "cannot create backup directory"
if [ ! -f "$STOCK" ]; then
    cp -p "$TARGET" "$DATA_DIR/backup/carplay.stock" || abort "stock backup failed"
    cp -p "$TARGET" "$STOCK" || abort "working backup failed"
fi
[ -f "$DATA_DIR/backup/carplay.sh.stock" ] || cp -p "$CARPLAY_DIR/carplay.sh" "$DATA_DIR/backup/carplay.sh.stock" 2>/dev/null || true

cp -f "$PAYLOAD_DIR/altscreen_handler.so" "$HANDLER" || abort "handler copy failed"
chmod 0755 "$HANDLER" "$STOCK" || abort "chmod failed"

cat > "$TARGET" <<'WRAPPER'
#!/bin/sh
DIR=/lge/app_rw/smartlink/carplay
DATA=/data/lge/smartlink/carplay
REAL="$DIR/carplay.moi3nar.stock"
SO="$DIR/moi3nar_altscreen.so"
FLAG="$DATA/moi3nar.enable"
BOOT_FILE="$DATA/moi3nar.bootid"
CURRENT=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
PREVIOUS=$(cat "$BOOT_FILE" 2>/dev/null)

if [ -n "$PREVIOUS" ] && [ -n "$CURRENT" ] && [ "$PREVIOUS" != "$CURRENT" ]; then
    rm -f "$FLAG" "$BOOT_FILE"
fi

if [ -f "$FLAG" ] && [ ! -f "$DATA/MOI3NAR_DISABLE" ] && [ -f "$SO" ]; then
    [ -n "$PREVIOUS" ] || echo "$CURRENT" > "$BOOT_FILE"
    echo "$(date) mode=trial boot=$CURRENT" >> "$DATA/moi3nar-wrapper.log" 2>/dev/null
    export LD_PRELOAD="$SO"
else
    echo "$(date) mode=stock boot=$CURRENT" >> "$DATA/moi3nar-wrapper.log" 2>/dev/null
fi
exec "$REAL" "$@"
WRAPPER
chmod 0755 "$TARGET" || abort "wrapper chmod failed"

if command -v chsmack >/dev/null 2>&1; then
    chsmack -a smartlink::app -e carplay::app "$TARGET" 2>/dev/null || true
    chsmack -a smartlink::app -e carplay::app "$STOCK" 2>/dev/null || true
    chsmack -a carplay::app "$HANDLER" 2>/dev/null || true
    chsmack -ra carplay::data "$DATA_DIR" 2>/dev/null || true
fi

rm -f "$DATA_DIR/MOI3NAR_DISABLE" "$DATA_DIR/moi3nar.bootid"
: > "$DATA_DIR/moi3nar.enable"
sync
log "INSTALLED: R8410L one-boot trial armed; stock SHA-256 verified and backed up"
exit 0
