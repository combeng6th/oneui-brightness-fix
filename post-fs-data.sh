#!/system/bin/sh
MODDIR=${0%/*}

# Ensure the daemon binary is executable (safety net for
# ZIP extraction that may not preserve exec bits reliably).
chmod 755 "$MODDIR/brightd"

# Lock all real backlight sysfs nodes to root-only write.
# Scans /sys/class/backlight/ and locks any device with max_brightness > 0.
# Dummy/virtual devices (max=0) are skipped.
for d in /sys/class/backlight/*/; do
    max=$(cat "$d/max_brightness" 2>/dev/null)
    [ "$max" -gt 0 ] 2>/dev/null || continue
    chown root:root "$d/brightness"
    chmod 644 "$d/brightness"
done
