# Experimental

The `experimental` section of the configuration table contains miscellaneous
and developer-focused settings.

## Default values

```lua
local config = {
    experimental = {
        debug = false,
        jit = false,
        tearing = false,
    },
}

return config
```

## Debug

<img class="right" src="assets/waywall-debug.png" alt="Debug text">

When enabled, the `debug` option will draw text about the state of waywall in
the upper left corner of the window.

This information is usually only needed for development purposes.

## JIT

waywall uses [LuaJIT] as its Lua implementation. By default, the JIT is
disabled due to limitations with the Lua `debug` package. If your configuration
contains a lot of compute-heavy Lua code, you may experience better performance
by setting the `jit` option to `true`.

> [!WARNING]
> Enabling the JIT may cause the [instruction limit] to behave inconsistently.
> If your configuration has infinite loops, waywall may freeze permanently.

## Tearing

The `tearing` option allows you to enable screen tearing (it is disabled by
default.) This option requires your compositor to support the
[`tearing_control_v1`] protocol, or else it will have no effect.

[LuaJIT]: https://luajit.org
[instruction limit]: 03_lua_changes.md#instruction-count-limit
[`tearing_control_v1`]: https://wayland.app/protocols/tearing-control-v1
