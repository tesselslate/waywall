# window_visible

Query or change the first floating window whose title exactly matches `title`.
Minecraft itself is not a floating window. Returns `nil` if no window matches,
otherwise its visibility after applying the request.

```lua
waywall.window_visible("toolwall")              -- query without changing focus
waywall.window_visible("toolwall", true)        -- show and focus
waywall.window_visible("Ninjabrain Bot", true, false) -- show without taking focus
waywall.window_visible("toolwall", false)       -- hide only this window
```

Hiding the focused window returns keyboard focus to Minecraft, if it exists.
Other floating windows retain their visibility and the default for newly created
windows is unchanged. `show_floating()` still applies to every floating window,
including individually hidden or shown windows. If several windows share a title,
the first match in creation order (newest first) is used.

This function cannot be called during startup.
