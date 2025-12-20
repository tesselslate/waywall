# Ninjabrain Bot

On most Wayland compositors, the contents of the clipboard are only offered to
clients with input focus. This means that Ninjabrain Bot may only become aware
of eye throws if you tab out of waywall and give it focus, which is not ideal.

To solve this, you can run Ninjabrain Bot inside of waywall so that it always
receives clipboard updates from the game immediately. For example, the following
action will start Ninjabrain Bot inside of waywall when run:

```lua
-- ... the rest of your configuration

config.actions = {
    -- ... the rest of your actions

    ["Ctrl-Shift-N"] = function()
        -- Change the path to your actual Ninjabrain Bot jar!
        waywall.exec("java -jar /path/to/ninjabrain-bot.jar")
    end
}

return config
```

Ninjabrain Bot is treated as a floating window, which are hidden by default. To
make it visible, you can create another action which uses
[`waywall.show_floating`] or [`helpers.toggle_floating`].

> [!TIP]
> If Ninjabrain Bot displays a blank window after opening, try launching it with a
> version of Java newer than Java 8 (i.e. Java 17).
>
> If you are using NixOS and Ninjabrain Bot fails to launch, try adding
> `-Dswing.defaultlaf=javax.swing.plaf.metal.MetalLookAndFeel` to your arguments
> when launching Ninjabrain Bot.

[`waywall.show_floating`]: 02_waywall_show_floating.md
[`helpers.toggle_floating`]: 02_helpers_toggle_floating.md
