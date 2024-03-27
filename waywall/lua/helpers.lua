--[[
    This file contains various "helper" functions which make it easier and more
    convenient to build a fully functional waywall configuration.

    The lower level Lua API can be found in `api.lua`.
]]

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

    if settings.lock_color and type(settings.lock_color) ~= "string" then
        error("expected settings.lock_color to be of type string")
    end

    if not settings.grace_period then
        settings.grace_period = 0
    end
    if type(settings.grace_period) ~= "number" then
        error("expected settings.grace_period to be of type number")
    end
    if settings.grace_period < 0 or settings.grace_period > 1000 then
        error("expected settings.grace_period to be between 0 and 1000 ms")
    end

    -- Setup wall objects.
    local state = {
        locked = {},

        width = math.floor(settings.width),
        height = math.floor(settings.height),
        lock_color = settings.lock_color or "#00000099",

        grace_period = math.floor(settings.grace_period),
    }
    local wall = {}

    local function check_reset(instance)
        if state.locked[instance] then
            return false
        end

        local inst_state = waywall.instance(instance)
        if inst_state.screen == "previewing" and state.grace_period > 0 then
            if waywall.current_time() - inst_state.last_preview < state.grace_period then
                return false
            end
        end

        return true
    end

    wall.is_locked = function(instance)
        return state.locked[instance]
    end
    wall.lock = function(instance)
        if state.locked[instance] then
            return
        end

        state.locked[instance] = true
        waywall.request_layout("lock")
        waywall.set_priority(instance, true)
    end
    wall.unlock = function(instance)
        if not state.locked[instance] then
            return
        end

        state.locked[instance] = nil
        waywall.request_layout("unlock")
        waywall.set_priority(instance, false)
    end
    wall.focus_reset = function()
        local hovered = waywall.hovered()
        if not hovered then
            return
        end

        local num_instances = waywall.num_instances()
        for i = 1, num_instances do
            if i ~= hovered and check_reset(i) then
                waywall.reset(i)
            end
        end

        waywall.play(hovered)
    end
    wall.play = function()
        local hovered = waywall.hovered()
        if not hovered then
            return
        end

        waywall.play(hovered)
        if state.locked[hovered] then
            state.locked[hovered] = nil
            waywall.set_priority(hovered, false)
        end
    end
    wall.reset = function()
        local hovered = waywall.hovered()
        if not hovered then
            return
        end

        if check_reset(hovered) then
            waywall.reset(hovered)
        end
    end
    wall.reset_all = function()
        if waywall.active_instance() then
            return
        end

        local num_instances = waywall.num_instances()
        for i = 1, num_instances do
            if check_reset(i) then
                waywall.reset(i)
            end
        end
    end
    wall.reset_ingame = function()
        local active = waywall.active_instance()
        if not active then
            return
        end

        waywall.goto_wall()
        waywall.reset(active)
    end
    wall.toggle_lock = function()
        local hovered = waywall.hovered()
        if not hovered then
            return
        end

        if state.locked[hovered] then
            wall.unlock(hovered)
        else
            wall.lock(hovered)
        end
    end

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
            local x = ((i - 1) % state.width) * instance_width
            local y = math.floor((i - 1) / state.width) * instance_height

            table.insert(layout, {
                "instance",
                i,
                x = x,
                y = y,
                w = instance_width,
                h = instance_height,
            })

            if state.locked[i] then
                table.insert(layout, {
                    "rectangle",
                    state.lock_color,
                    x = x,
                    y = y,
                    w = instance_width,
                    h = instance_height,
                })
            end
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
