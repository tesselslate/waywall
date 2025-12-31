--[[
    This file contains the Lua portion of the waywall API, including some
    documentation comments. See `waywall/config/api.c` for the relevant C
    code.
]]

local priv = _G.priv_waywall

local function event_handler(name)
    local listeners = {}

    priv.register(name, function()
        for listener, _ in pairs(listeners) do
            local ok, result = pcall(listener)
            if not ok then
                priv.log_error("failed to call event listener (" .. name .. "): " .. result)
            end
        end
    end)

    return function(listener)
        if type(listener) ~= "function" then
            error("listener must be a function")
        end

        if listeners[listener] then
            return
        end

        listeners[listener] = true
        return function()
            listeners[listener] = nil
        end
    end
end

local events = {
    ["load"] = event_handler("load"),
    ["resolution"] = event_handler("resolution"),
    ["state"] = event_handler("state"),
}

local M = {}

--- Register a listener for a specific event.
-- @param event The name of the event to listen for.
-- @param listener The function to call when the event occurs.
-- @return unregister A function which can be used to remove the listener.
M.listen = function(event, listener)
    if not events[event] then
        error("unknown event: " .. event)
    end

    return events[event](listener)
end

--[[
    C API wrappers
]]

--- Get the current resolution of Minecraft.
-- @return width The width of the Minecraft window, or 0 if none has been set.
-- @return height The height of the Minecraft window, or 0 if none has been set.
M.active_res = priv.active_res

--- Get the current time, in milliseconds, with an arbitrary epoch.
M.current_time = priv.current_time

--- Forks and executes the given command.
-- The command will be run using fork() and execvp(). Arguments will be split by
-- spaces; no further processing of arguments will happen.
--
-- It is recommended that you use this function instead of io.popen(), which will
-- cause waywall to freeze if the configuration is reloaded while a subprocess is
-- still running.
--
-- @param command The command to run.
M.exec = priv.exec

--- Returns whether or not floating windows are currently visible.
-- @return shown Whether floating windows are shown.
M.floating_shown = priv.floating_shown

--- Creates an image object which displays a PNG image from the filesystem.
-- @param path The filepath to the image.
-- @param options The options to create the image with.
-- @return image The image object.
M.image = priv.image

--- Creates a "mirror" object which mirrors part of the Minecraft window.
-- @param options The options to create the mirror with.
-- @return mirror The mirror object.
M.mirror = priv.mirror

--- Press and immediately release the given key in the Minecraft window.
-- @param key The name of the key to press.
M.press_key = priv.press_key

--- Returns the current state of a key on the keyboard.
-- @param key The name of the key to press.
-- @return pressed (boolean) Whether the key is currently pressed.
M.get_key = priv.get_key

--- Get the name of the current profile.
-- @return The current profile, or nil if the default profile is active.
M.profile = priv.profile

--- Attempts to update the current keymap to one with the specified settings.
-- @param keymap The keymap options (layout, model, rules, variants, and options
-- are valid keys.)
M.set_keymap = priv.set_keymap

--- Sets the input remapping map, corresponding to config.input.remaps.
-- @param remaps A table containing the remapping map, with the same format as
-- config.input.remaps.
M.set_remaps = priv.set_remaps

--- Sets the resolution of the Minecraft window.
-- Providing a resolution of 0 width and 0 height will cause the instance to be
-- set back to the size of the waywall window.
-- @param width The width to set the Minecraft window to.
-- @param height The height to set the Minecraft window to.
M.set_resolution = priv.set_resolution

--- Sets the sensitivity multiplier for relative pointer motion (3D ingame aim).
-- @param sensitivity The multiplier to use. A value of zero resets sensitivity.
M.set_sensitivity = priv.set_sensitivity

--- Shows or hides floating windows.
-- @param show Whether or not to show floating windows.
M.show_floating = priv.show_floating

--- Pauses the current action for the given number of milliseconds.
-- This function may only be called from within a keybind handler. Calling it
-- from within another context will result in an error.
-- @param ms Number of milliseconds to sleep for
M.sleep = priv.sleep

--- Gets the state of the Minecraft instance.
-- StateOutput must be present and enabled on the instance for an accurate
-- result.
-- @return state A table containing information about the instance state.
M.state = priv.state

--- Creates a "text" object which displays arbitrary text.
-- @param options The options to create the text with.
-- @return text The text object.
M.text = priv.text

--- Toggle the Waywall window between fullscreen and not
M.toggle_fullscreen = priv.toggle_fullscreen

package.loaded["waywall"] = M
