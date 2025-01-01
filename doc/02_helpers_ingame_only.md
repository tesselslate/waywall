# ingame_only

This function wraps the given function so that it only runs if the user is in a
world and not in any menu. If these conditions are not met, the given function
will not be called and `false` will be returned.

If State Output is not enabled, calling the returned function will throw an
error.

### Arguments

  - `func`: function

### Return values

  - `ingame_func`: function

> The returned function cannot be called during startup.
