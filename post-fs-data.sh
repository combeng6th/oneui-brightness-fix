#!/system/bin/sh
# Lock the backlight sysfs to root-only write BEFORE the lights HAL starts.
# The HAL always writes 510 regardless of input. Blocking it at the
# permission level eliminates flicker — the daemon is the sole writer.
chown root:root /sys/class/backlight/panel0-backlight/brightness
chmod 644 /sys/class/backlight/panel0-backlight/brightness
