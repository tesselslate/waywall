--[[
    This file contains the Lua portion of the waywall API, including some
    documentation comments. See `waywall/config/api.c` for the relevant C
    code.
]]

local priv = _G.priv_waywall

local M = {}

--- Get the active instance, if any.
-- @return The ID of the active instance, or nil if no instance is active.
M.active_instance = priv.active_instance

--- Get the current time, in milliseconds, with an arbitrary epoch.
-- The epoch used by current_time is the same as what is given in instance
-- state information, such as last_load and last_preview.
M.current_time = priv.current_time

--- Get the resolution of the active instance, if any.
-- Throws an error if there is no active instance.
-- @return width The width of the active instance, or 0 if none has been set.
-- @return height The height of the active instance, or 0 if none has been set.
M.get_active_res = priv.get_active_res

--- Switch to the wall view.
-- This function will throw an error if the user is already on the wall.
M.goto_wall = priv.goto_wall

--- Get the instance the user's pointer is hovering over, if any.
-- This function will not throw an error if the user is not on the wall, and
-- will instead act as if no instance is hovered.
-- @return The ID of the hovered instance, or nil if no instance is hovered.
M.hovered = priv.hovered

--- Gets information about the state of the provided instance.
-- This function will throw an error if the given instance does not exist.
-- @param instance The ID of the instance to get information for.
-- @return state A table containing the state of the instance.
M.instance = priv.instance

--- Replace the existing set of event handlers (if any) with the provided ones.
-- @param handlers The new set of event handlers to use.
-- @return previous The previous set of event handlers.
M.listen = priv.listen

--- Get the number of currently running instances.
-- @return The number of instances.
M.num_instances = priv.num_instances

--- Play the given instance.
-- @param instance The ID of the instance to play.
-- This function will throw an error if the given instance does not exist, or if
-- the given instance is already active.
M.play = priv.play

--- Get the name of the current profile.
-- @return The current profile, or nil if the default profile is active.
M.profile = priv.profile

--- Reset the given instance(s).
-- The caller can provide either a number (single instance ID), or an array
-- containing one or more instance IDs.
-- This function will throw an error in the event of an allocation failure.
-- @param instances The ID of the instance(s) to reset.
-- @param count Optional. If `false` is provided, the reset will not be counted.
-- @return The number of instances which were actually reset.
M.reset = priv.reset

--- Attempts to update the current keymap to one with the specified settings.
-- @param keymap The keymap options (layout, model, rules, variants, and options
-- are valid keys.)
M.set_keymap = priv.set_keymap

--- Updates the current wall layout to the provided one.
-- @param layout The table containing the new layout.
M.set_layout = priv.set_layout

--- Updates the CPU priority of the given instance.
-- @param instance The instance to update priority for.
-- @param priority Whether or not the instance should be given more CPU time.
M.set_priority = priv.set_priority

--- Sets the sensitivity multiplier for relative pointer motion (3D ingame aim).
-- @param sensitivity The multiplier to use. Must be greater than zero.
M.set_sensitivity = priv.set_sensitivity

--- Sets the resolution of the given instance.
-- This function will throw an error if the given instance or resolution is
-- invalid.
-- Providing a resolution of 0 width and 0 height will cause the instance to be
-- set back to the size of the waywall window.
-- @param instance The instance to set the resolution of.
-- @param width The width to set the instance window to.
-- @param height The height to set the instance window to.
M.set_resolution = priv.set_resolution

--- Gets the size of the waywall window.
-- This function will return (0, 0) if the window is not open.
-- @return width The width of the window, in pixels
-- @return height The height of the window, in pixels
M.window_size = priv.window_size

package.loaded["waywall"] = M
