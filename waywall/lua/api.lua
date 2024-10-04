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
    ["state"] = event_handler("state"),
}

local M = {}

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
-- A maximum of 63 arguments may be provided.
-- @param command The command to run.
M.exec = priv.exec

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

--- Press and immediately release the given key in the Minecraft window.
-- @param key The name of the key to press.
M.press_key = priv.press_key

--- Get the name of the current profile.
-- @return The current profile, or nil if the default profile is active.
M.profile = priv.profile

--- Attempts to update the current keymap to one with the specified settings.
-- @param keymap The keymap options (layout, model, rules, variants, and options
-- are valid keys.)
M.set_keymap = priv.set_keymap

--- Sets the sensitivity multiplier for relative pointer motion (3D ingame aim).
-- @param sensitivity The multiplier to use. Must be greater than zero.
M.set_sensitivity = priv.set_sensitivity

--- Sets the resolution of the Minecraft window.
-- Providing a resolution of 0 width and 0 height will cause the instance to be
-- set back to the size of the waywall window.
-- @param width The width to set the Minecraft window to.
-- @param height The height to set the Minecraft window to.
M.set_resolution = priv.set_resolution

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

--- Gets the size of the waywall window.
-- This function will return (0, 0) if the window is not open.
-- @return width The width of the window, in pixels
-- @return height The height of the window, in pixels
M.window_size = priv.window_size

package.loaded["waywall"] = M
