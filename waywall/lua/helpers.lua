--[[
    This file contains various "helper" functions which make it easier and more
    convenient to build a fully functional waywall configuration.

    The lower level Lua API can be found in `api.lua`.
]]

local waywall = require("waywall")

local M = {}

--- Provides an easy way to switch between different resolutions.
-- If the Minecraft window has no set resolution (or a resolution different
-- to the one provided to `toggle_res`), the function will set it to the given
-- resolution. Otherwise, the window will be set to have no resolution.
-- @param width The width to set the Minecraft window to.
-- @param height The height to set the Minecraft window to.
-- @return toggle_res A function which can be used to switch resolutions.
M.toggle_res = function(width, height)
    return function()
        local act_width, act_height = waywall.get_active_res()
        if act_width == width and act_height == height then
            waywall.set_resolution(0, 0)
        else
            waywall.set_resolution(width, height)
        end

        return true
    end
end

package.loaded["waywall.helpers"] = M
