# Lua changes

waywall makes use of [LuaJIT], an alternative implementation of Lua 5.1 which
provides better performance and additional functionality. A list of LuaJIT's
additions is available [here](https://luajit.org/extensions.html).

> Note: The `jit` package from LuaJIT is not available in waywall.

## Instruction count limit

waywall will allow a maximum of 50 million Lua instructions to be executed when
it calls into user code (e.g. actions). This limit is in place to prevent buggy
configurations from hard-locking waywall.

If this limit is exceeded, an error will be thrown, causing Lua execution to
stop. This error cannot be caught with `pcall` or `xpcall`.

> [!WARNING]
> [Enabling the JIT] may cause the instruction limit to behave inconsistently.
> If your configuration has infinite loops, waywall may freeze permanently.

## Standard library changes

waywall makes a few changes and additions to the Lua standard library:

  - `package.path` is automatically updated to include the waywall configuration
    directory, so you can `require()` other files contained within it.
  - `pcall` and `xpcall` have been modified to prevent user code from disabling
    the instruction count limit by accident.
  - `print` has been modified so that its output appears in a similar format to
    other waywall log messages.
  - `os.setenv` is a **new** function added by waywall which behaves much like
    C's `setenv()` and allows for changing and deleting environment variables.
    - Calling `os.setenv` with two strings (a name and value) will behave like
      C's `setenv()`.
    - Calling `os.setenv` with a string and nil will unset the given environment
      variable.

You can refer to the [startup code] to see all of the changes waywall makes in
more detail.

[LuaJIT]: https://luajit.org
[Enabling the JIT]: 01_options_experimental.md#jit
[startup code]: https://github.com/tesselslate/waywall/blob/main/waywall/lua/init.lua
