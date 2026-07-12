# Changelog

## v3.0

- **Fixed bootloop caused by stopping the lights HAL service.** The `stop` command killed a binder service that system_server holds a live reference to, causing a fatal crash on boot. Reverted to permission-lockout only — the HAL process runs but cannot write to the backlight sysfs node. (Thanks to Ngo An Binh for help debugging this on the SM-N981N.)
- `post-fs-data.sh` now sets `chmod 755` on the daemon binary as a safety net for ZIP extraction that may not preserve exec bits.
- Dynamic sysfs discovery: `post-fs-data.sh` scans and locks all real backlight devices, not just a hardcoded path.

## v2.0

- `max_brightness` is now read from sysfs at startup instead of being hardcoded to 510. The daemon automatically detects the device's backlight range.
- Backlight path discovered dynamically by scanning `/sys/class/backlight/` for the device with the highest `max_brightness > 0`.
- No recompilation needed for different devices.

## v1.0

- Initial release.
- Native daemon (`brightd`, 9KB ARM64) with ASensorManager NDK API via dlopen for zero-fork sensor access.
- Median-of-5 filter, asymmetric EMA, hysteresis, proportional ramping.
- Adaptive brightness with persistent slider offset (matching stock behavior).
- HAL lockout via sysfs permission change in `post-fs-data.sh`.
- Falls back to `dumpsys sensorservice` parsing if NDK sensor API unavailable.
