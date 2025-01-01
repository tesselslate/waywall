# Options

waywall has a number of built-in options for configuring its appearance and
behavior. These options can be set by returning a table from your configuration
file, e.g.:

```lua
local config = {
    input = {
        layout = "us",

        -- ... more options here
    },

    -- ... more options here
}

return config
```

The following sections will cover all of the available options which can be set
using this configuration table.
