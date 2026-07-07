# Adaptive Brightness Fix

A Magisk module that permanently fixes broken adaptive brightness on ported Samsung OneUI 8 ROMs.

## The problem

When a Samsung ROM built for one phone (like the Galaxy S23 Ultra) is ported to different hardware (like the Note 20 Ultra), the brightness system breaks. The part of the software responsible for controlling the screen backlight — the "lights HAL" — doesn't know how to talk to the new phone's display hardware. The result: the screen is permanently stuck at maximum brightness, and the brightness slider does nothing. Adaptive (automatic) brightness is completely non-functional.

## What this module does

This module works around the problem in two steps:

1. **Blocks the broken brightness controller.** At boot, before the broken lights HAL starts up, the module changes the permissions on the backlight control file so that only root can write to it. The broken HAL silently fails to write, and the screen is no longer forced to max brightness.

2. **Replaces it with a working one.** A small native daemon (~9KB) runs in the background and takes over brightness control. It reads the ambient light sensor directly using Android's built-in sensor API — no extra apps or frameworks needed. It adjusts the screen brightness smoothly based on the ambient light, just like the stock adaptive brightness you're used to.

### Features

- Adaptive brightness that actually works — smoothly adjusts based on ambient light
- Slider adjustments in auto mode are respected and remembered (just like stock)
- Manual brightness mode works normally when adaptive is turned off
- Smooth transitions with no flickering
- Tiny footprint: single 9KB binary, no Java, no frameworks, no dependencies
- Survives reboots (installed as a Magisk module)

## Compatibility

This module was developed and tested on:

- **Galaxy Note 20 Ultra (SM-N9860, Snapdragon 865+)** running [Astro-OS v3.x](https://xdaforums.com/t/closed-astro-os-oneui-8-0-galaxy-note20-series-snapdragon-s23-ultra-port-version-3-1-0-ai-port-camera-enhancements-optimize.4786282/) (S23 Ultra port)

It should work on **any Qualcomm-based Samsung device** running a ported OneUI 8 ROM where the brightness is broken due to a mismatched lights HAL. The module targets the standard Qualcomm MDSS backlight path (`/sys/class/backlight/panel0-backlight/brightness`).

**Requirements:**
- Magisk v20.4 or newer
- ARM64 device
- Root access

## Installation

1. Download the latest release ZIP from the [Releases](https://github.com/combeng6th/brightness-fix/releases) page
2. Open Magisk Manager
3. Go to Modules → Install from storage
4. Select the ZIP file
5. Reboot

After rebooting, adaptive brightness should work immediately. You can toggle it on/off from the notification shade as usual.

## Tuning

If the brightness feels too bright or too dim overall, you can adjust the curve by editing the `AUTO_K` value in `brightd.c` and recompiling:

```
#define AUTO_K 50   /* lower = brighter at same light level */
```

Rebuild with:
```
clang -O2 -Wall -o brightd brightd.c -ldl && llvm-strip brightd
```

Then replace the `brightd` binary in the module directory and reboot.

## How it works (technical)

The daemon (`brightd`) uses `dlopen` to load Android's `libandroid.so` at runtime and registers a light sensor listener via the `ASensorManager` NDK API. Sensor events arrive directly into the process — no polling, no process forks, no shell commands.

The brightness pipeline:

```
sensor lux → median filter (5 samples) → asymmetric EMA → saturation curve → user offset → hysteresis → proportional ramp → sysfs write
```

- **Median filter** rejects sensor noise spikes
- **Asymmetric EMA** responds quickly to brightening (0.3s), slowly to darkening (1.2s)
- **Saturation curve** (`lux × max / (lux + K)`) maps lux to backlight perceptually
- **User offset** captures slider adjustments and applies them persistently
- **Hysteresis** prevents micro-jitter from residual noise
- **Proportional ramp** animates transitions smoothly

If the NDK sensor API is unavailable, the daemon falls back to parsing `dumpsys sensorservice` output (one fork per second).

## Credits

This module exists because of [Astro-OS](https://xdaforums.com/t/closed-astro-os-oneui-8-0-galaxy-note20-series-snapdragon-s23-ultra-port-version-3-1-0-ai-port-camera-enhancements-optimize.4786282/), an excellent S23 Ultra port for the Note 20 series by the Astro-OS team on XDA. The ROM itself is fantastic — this module just fixes one hardware-specific issue that comes with running a ported ROM on different display hardware.

I am not affiliated with the Astro-OS project. Just a grateful user.

## License

MIT
