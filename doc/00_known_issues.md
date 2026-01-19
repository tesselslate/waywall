# Known Issues

This page contains a list of known issues which you may encounter while using
waywall.

## Ninjabrain Bot calibration does not work

Ninjabrain Bot's calibration functionality has issues on Linux and does not work
correctly within waywall. Using boat eye largely eliminates the need to
calibrate. If boat eye is not an option, you may need to calibrate your standard
deviation outside of waywall.

## waywall freezing in some Nvidia environments

Older versions of the Nvidia driver may cause waywall to freeze until waywall
receives more events from your compositor (mouse movement, keyboard inputs,
window resizing, etc).

If you have V-Sync enabled ingame, disabling it should fix the problem. If your
framerate is uncapped and V-Sync is off, then try capping your framerate to less
than 5x your monitor refresh rate.

If neither of these fix the problem, please let us know in [Discord] or on the
[issue tracker].

## Display scaling not automatically accounted for

waywall does not currently support either of the Wayland protocols used for
display scaling ([1](https://wayland.app/protocols/wayland#wl_surface:event:preferred_buffer_scale),
[2](https://wayland.app/protocols/fractional-scale-v1)). However, you can
manually configure waywall to use a specific resolution while fullscreened
with the [`fullscreen_width` and `fullscreen_height` options](01_options_window.md#fullscreen-resolution).

[Discord]: https://discord.gg/3tm4UpUQ8t
[issue tracker]: https://github.com/tesselslate/waywall/issues
