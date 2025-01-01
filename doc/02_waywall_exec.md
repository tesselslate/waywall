# exec

This function asynchronously executes the given command as a subprocess. No
sophisticated argument processing is performed; quoting arguments or performing
shell substitutions will require you to run a shell script. A maximum of 63
space-delimited arguments can be provided in the command string.

If the spawned subprocess does not exit before waywall, it will be killed with
`SIGKILL` when waywall closes.

### Arguments

  - `command`: string

### Return values

None

> This function cannot be called during startup.
