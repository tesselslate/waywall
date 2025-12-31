# exec

This function asynchronously executes the given command as a subprocess.
Arguments are split by spaces and no further processing is performed; quoting
arguments or performing shell substitutions will require you to run a shell
script.

If the spawned subprocess does not exit before waywall, it will be killed with
`SIGKILL` when waywall closes.

### Arguments

  - `command`: string

### Return values

None

> This function cannot be called during startup.
