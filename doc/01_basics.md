# Lua

waywall is configured using the [Lua] programming language. If you have not used
Lua before, you may find the documentation to be helpful:

  - [Programming in Lua]
    - Do note that this book is slightly out of date, since it was written for
      Lua 5.0. waywall uses Lua 5.1, which has some additional features and
      changes.
  - [Lua 5.1 Reference Manual]

If you *are* familiar with Lua, it is worth noting that waywall uses LuaJIT and
makes some small changes to the behavior and standard library of Lua. See
[Lua changes] for more information.

<div class="warning">

Lua code executed by waywall is allowed to interact with the host operating
system in various ways, such as spawning subprocesses and modifying files on
disk. Read other people's code and do not blindly copy paste it into your own
configuration.

</div>

# Profiles

By default, waywall attempts to read and execute a configuration file from
`$XDG_CONFIG_HOME/waywall/init.lua` (typically, this is equivalent to
`~/.config/waywall/init.lua`).

If launched with a `--profile foo` argument, waywall will instead attempt to
read and execute a configuration file from `$XDG_CONFIG_HOME/waywall/foo.lua`.
Any number of profiles can exist within the waywall configuration directory.

# Hot reload

waywall attempts to watch for any and all changes to `.lua` files within the
waywall configuration directory. When it detects a change, it will attempt to
automatically reload your configuration. If successful, the Lua VM will be
recreated and any changes to variables or other state within will not be
transferred to the new configuration.

[Lua]: https://lua.org
[Programming in Lua]: https://www.lua.org/pil/contents.html
[Lua 5.1 Reference Manual]: https://www.lua.org/manual/5.1/
[Lua changes]: 03_lua_changes.md
