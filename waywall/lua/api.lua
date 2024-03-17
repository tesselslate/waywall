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

--- Get the number of currently running instances.
-- @return The number of instances.
M.num_instances = priv.num_instances

--- Play the given instance.
-- @param instance The ID of the instance to play.
-- This function will throw an error if the given instance does not exist, or if
-- the given instance is already active.
M.play = priv.play

--- Reset the given instance(s).
-- The caller can provide either a number (single instance ID), or an array
-- containing one or more instance IDs.
-- This function will throw an error in the event of an allocation failure.
-- @param instances The ID of the instance(s) to reset.
-- @return The number of instances which were actually reset.
M.reset = priv.reset

--- Sets the sensitivity multiplier for relative pointer motion (3D ingame aim).
-- @param sensitivity The multiplier to use. Must be greater than zero.
M.set_sensitivity = priv.set_sensitivity

--- Sets the resolution of the active instance.
-- This function will throw an error if there is no active instance or if the
-- provided resolution is invalid.
-- Providing a resolution of 0 width and 0 height will cause the instance to be
-- set back to the size of the waywall window.
-- @param width The width to set the instance window to.
-- @param height The height to set the instance window to.
M.set_resolution = priv.set_resolution

package.loaded["waywall"] = M
