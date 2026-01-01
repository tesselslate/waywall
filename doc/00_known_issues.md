# Known Issues

This page contains a list of known issues which you may encounter while using
waywall.

## Ninjabrain Bot calibration does not work

Ninjabrain Bot's calibration functionality has issues on Linux and does not work
correctly within waywall. Using boat eye largely eliminates the need to
calibrate, but if that is not possible; otherwise, you may need to calibrate
your standard deviation outside of waywall.

## Ninjabrain Bot in wrong position on Smithay-based compositors

On Smithay-based compositors, such as Niri, Ninjabrain Bot may appear in the
wrong position after the visibility of floating windows is toggled. This is due
to [a bug](https://github.com/Smithay/smithay/issues/1894) in Smithay. A
workaround for this issue may be implemented in the future.

## waywall freezing in some Nvidia environments

Older versions of the Nvidia driver may cause waywall to freeze until waywall
receives more events from your compositor (mouse movement, keyboard inputs,
window resizing, etc).

If you have V-Sync enabled ingame, disabling it should fix the problem. If your
framerate is uncapped and V-Sync is off, then try capping your framerate to less
than 5x your monitor refresh rate.

If neither of these fix the problem, please let us know in [Discord] or on the
[issue tracker].

[Discord]: https://discord.gg/3tm4UpUQ8t
[issue tracker]: https://github.com/tesselslate/waywall/issues
