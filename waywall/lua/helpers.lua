--[[
    This file contains various "helper" functions which make it easier and more
    convenient to build a fully functional waywall configuration.

    The lower level Lua API can be found in `api.lua`.
]]

local waywall = require("waywall")

local M = {}

--- Wraps the given function to only run if the user is not in a menu.
-- This will only work if the Minecraft instance has state-output; otherwise,
-- the returned function will throw an error.
-- @param func The function to wrap.
-- @return ingame_func Wrapped version of func which only executes when ingame.
M.ingame_only = function(func)
    return function()
        local state = waywall.state()

        if state.screen == "inworld" and state.inworld == "unpaused" then
            return func()
        else
            return false
        end
    end
end

--- Toggles the visibility of floating windows.
M.toggle_floating = function()
    waywall.show_floating(not waywall.floating_shown())
end

--- Provides an easy way to switch between different resolutions.
-- If the Minecraft window has no set resolution (or a resolution different
-- to the one provided to `toggle_res`), the function will set it to the given
-- resolution. Otherwise, the window will be set to have no resolution.
--
-- If a sensitivity is provided, toggling to the resolution will set the mouse
-- sensitivity to that value. Toggling away will set the mouse sensitivity back
-- to the default value.
-- @param width The width to set the Minecraft window to.
-- @param height The height to set the Minecraft window to.
-- @param sens The sensitivity to use when toggling resolution, if any.
-- @return toggle_res A function which can be used to switch resolutions.
M.toggle_res = function(width, height, sens)
    return function()
        local act_width, act_height = waywall.active_res()
        if act_width == width and act_height == height then
            waywall.set_resolution(0, 0)
            waywall.set_sensitivity(0)
        else
            waywall.set_resolution(width, height)
            if type(sens) == "number" then
                waywall.set_sensitivity(sens)
            else
                waywall.set_sensitivity(0)
            end
        end

        return true
    end
end

package.loaded["waywall.helpers"] = M
