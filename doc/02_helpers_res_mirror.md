# res_mirror

This function creates a mirror which only appears when a specific resolution
is active, and is not present otherwise. It is effectively equivalent to:

```lua
waywall.listen("resolution", function()
    local width, height = waywall.active_res()

    if width == DESIRED_WIDTH and height == DESIRED_HEIGHT then
        -- make mirror...
    else
        -- destroy mirror...
    end
end)
```

The mirror will be created with the given `options` and will only appear on
screen while the active resolution is equal to `width` x `height`.

### Arguments

  - `options`: table
  - `width`: number
  - `height`: number

### Return values

  - `cancel`: function
