--[[
    This file contains the Lua portion of the waywall API, including some
    documentation comments. See `waywall/config/api.c` for the relevant C
    code.
]]--

local priv = _G.priv_waywall

local M = {}

--- Get the active instance, if any.
-- @return The ID of the active instance, or nil if no instance is active.
M.active_instance = priv.active_instance

--- Switch to the wall view.
-- This function will throw an error if the user is already on the wall.
M.goto_wall = priv.goto_wall

--- Get the instance the user's pointer is hovering over, if any.
-- This function will not throw an error if the user is not on the wall, and
-- will instead act as if no instance is hovered.
-- @return The ID of the hovered instance, or nil if no instance is hovered.
M.hovered = priv.hovered

--- Play the given instance.
-- @param instance The ID of the instance to play.
-- This function will throw an error if the given instance does not exist, or if
-- the given instance is already active.
M.play = priv.play

--- Reset the given instance.
-- @param instance The ID of the instance to reset.
-- @return Whether or not the instance was in a valid state to be reset.
M.reset = priv.reset

package.loaded["waywall"] = M
