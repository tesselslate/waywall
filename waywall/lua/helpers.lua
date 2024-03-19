--[[
    This file contains various "helper" functions which make it easier and more
    convenient to build a fully functional waywall configuration.

    The lower level Lua API can be found in `api.lua`.
]]--

local waywall = require("waywall")

local M = {}

--- Implements standard "wall" functionality in the user's configuration.
-- This function replaces the `layout` table of the provided configuration.
-- @param config The configuration table to modify.
-- @param settings The wall settings to use.
-- @return wall An object which can be used to query and modify the wall state.
M.wall = function(config, settings)
    -- Process settings.
    if type(settings) ~= "table" then
        error("expected settings table")
    end
    if type(settings.width) ~= "number" then
        error("expected settings.width to be of type number")
    end
    if settings.width < 1 then
        error("expected settings.width to be at least 1")
    end
    if type(settings.height) ~= "number" then
        error("expected settings.height to be of type number")
    end
    if settings.height < 1 then
        error("expected settings.height to be at least 1")
    end

    -- Setup wall objects.
    local state = {
        locked = {},
        width = math.floor(settings.width),
        height = math.floor(settings.height),
    }

    local wall = {
        is_locked = function(wall, instance)
            return state.locked[instance]
        end,
        lock = function(wall, instance)
            if state.locked[instance] then
                return
            end

            state.locked[instance] = true
            waywall.request_layout("lock")
        end,
        unlock = function(wall, instance)
            if not state.locked[instance] then
                return
            end

            state.locked[instance] = nil
            waywall.request_layout("unlock")
        end,

        focus_reset = function(wall)
            local hovered = waywall.hovered()
            if not hovered then
                return
            end

            local num_instances = waywall.num_instances()
            for i = 1, num_instances do
                if i ~= hovered and not state.locked[i] then
                    waywall.reset(i)
                end
            end

            waywall.play(i)
        end,
        play = function(wall)
            local hovered = waywall.hovered()
            if not hovered then
                return
            end

            waywall.play(hovered)
        end,
        reset = function(wall)
            local hovered = waywall.hovered()
            if not hovered then
                return
            end

            if not state.locked[hovered] then
                waywall.reset(hovered)
            end
        end,
        reset_all = function(wall)
            if waywall.active_instance() then
                return
            end

            local num_instances = waywall.num_instances()
            for i = 1, num_instances do
                if not state.locked[i] then
                    waywall.reset(i)
                end
            end
        end,
        reset_ingame = function(wall)
            local active = waywall.active_instance()
            if not active then
                return
            end

            waywall.goto_wall()
            waywall.reset(active)
        end,
        toggle_lock = function(wall)
            local hovered = waywall.hovered()
            if not hovered then
                return
            end

            if state.locked[hovered] then
                wall:unlock(hovered)
            else
                wall:lock(hovered)
            end
        end,
    }

    local function generate()
        local num_instances = waywall.num_instances()
        local win_width, win_height = waywall.window_size()

        -- The window is closed. Don't bother updating the layout.
        if win_width == 0 or win_height == 0 then
            return {}
        end

        local instance_width = math.floor(win_width / state.width)
        local instance_height = math.floor(win_height / state.height)

        local layout = {}

        for i = 1, num_instances do
            table.insert(layout, {
                "instance", i,
                x = ((i - 1) % state.width) * instance_width,
                y = math.floor((i - 1) / state.width) * instance_height,
                w = instance_width,
                h = instance_height,
            })
        end

        return layout
    end

    -- Do not maintain the layout table if it exists.
    config.layout = {
        death = generate,
        manual = generate,
        resize = generate,
        spawn = generate,
    }

    return wall
end

package.loaded["waywall.helpers"] = M
