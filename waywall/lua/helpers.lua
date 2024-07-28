--[[
    This file contains various "helper" functions which make it easier and more
    convenient to build a fully functional waywall configuration.

    The lower level Lua API can be found in `api.lua`.
]]

local waywall = require("waywall")

local M = {}

local function check_type(tbl, key, typ)
    if type(tbl[key]) ~= typ then
        error("expected " .. key .. " to be of type " .. typ)
    end
end

local function generate_wall(width, height)
    local num_instances = waywall.num_instances()
    local win_width, win_height = waywall.window_size()

    if win_width < width or win_height < height then
        return {}
    end

    -- The window is closed. Don't generate a layout.
    if win_width == 0 or win_height == 0 then
        return nil
    end

    local inst_width = math.floor(win_width / width)
    local inst_height = math.floor(win_height / height)

    local layout = {}

    for i = 1, num_instances do
        local x = ((i - 1) % width) * inst_width
        local y = math.floor((i - 1) / width) * inst_height

        table.insert(layout, {
            "instance",
            i,
            x = x,
            y = y,
            w = inst_width,
            h = inst_height,
        })
    end

    return layout
end

--- Provides a simple benchmark implementation.
-- @param settings The benchmark settings to use.
-- @return benchmark An object which can be used to stop and start benchmarking.
M.benchmark = function(settings)
    if type(settings) ~= "table" then
        error("expected settings table")
    end

    if not settings.count then
        settings.count = 2000
    end
    check_type(settings, "count", "number")
    if settings.count < 1 then
        error("expected settings.count to be at least 1")
    end

    local state = {
        count = settings.count,
    }
    local benchmark = {}

    local function finish()
        local resets = state.count - state.run.remaining
        local seconds = (waywall.current_time() - state.run.start_time) / 1000

        print(
            string.format(
                "Completed %d resets in %.2f seconds (%.2f/s)",
                resets,
                seconds,
                resets / seconds
            )
        )

        waywall.listen(state.run.prev)
        state.run = nil
    end

    local function reset(instance)
        if instance > state.run.inst_count then
            return
        end

        if not waywall.reset(instance, false) then
            print("Failed to reset instance " .. instance)
            return
        end

        state.run.remaining = state.run.remaining - 1
        if state.run.remaining == 0 then
            finish()
        end
    end

    local function update()
        local size = math.ceil(math.sqrt(waywall.num_instances()))
        local layout = generate_wall(size, size)

        if layout then
            waywall.set_layout(layout)
        end
    end

    benchmark.start = function()
        if waywall.active_instance() then
            return false
        end

        if state.run then
            error("Benchmark already running")
        end
        if waywall.num_instances() == 0 then
            error("Cannot start benchmark with no instances")
        end

        state.run = {}
        state.run.prev = waywall.listen({
            death = update,
            preview_start = reset,
            resize = update,
            spawn = update,
        })

        update()

        state.run.inst_count = waywall.num_instances()
        state.run.remaining = state.count
        state.run.start_time = waywall.current_time()

        for i = 1, state.run.inst_count do
            if waywall.reset(i, false) then
                state.run.remaining = state.run.remaining - 1
            end
        end
        return true
    end

    benchmark.stop = function()
        if not state.run then
            error("Benchmark is not running")
        end

        finish()
        return true
    end

    return benchmark
end

--- Provides a basic static wall implementation.
-- @param settings The wall settings to use.
-- @return wall An object which can be used to query and modify the wall state.
M.wall = function(settings)
    if type(settings) ~= "table" then
        error("expected settings table")
    end

    check_type(settings, "width", "number")
    if settings.width < 1 then
        error("expected settings.width to be at least 1")
    end

    check_type(settings, "height", "number")
    if settings.height < 1 then
        error("expected settings.height to be at least 1")
    end

    if not settings.grace_period then
        settings.grace_period = 0
    end
    check_type(settings, "grace_period", "number")
    if settings.grace_period < 0 or settings.grace_period > 1000 then
        error("expected settings.grace_period to be between 0 and 1000 ms")
    end

    if not settings.bypass then
        settings.bypass = false
    end
    check_type(settings, "bypass", "boolean")

    check_type(settings, "stretch_width", "number")
    if settings.stretch_width < 1 then
        error("expected settings.stretch_width to be at least 1")
    end

    check_type(settings, "stretch_height", "number")
    if settings.stretch_height < 1 then
        error("expected settings.stretch_height to be at least 1")
    end

    if settings.lock_color and type(settings.lock_color) ~= "string" then
        error("expected settings.lock_color to be of type string")
    end

    -- Setup wall objects.
    local state = {
        locked = {},

        width = math.floor(settings.width),
        height = math.floor(settings.height),
        grace_period = math.floor(settings.grace_period),
        bypass = settings.bypass,

        stretch_width = math.floor(settings.stretch_width),
        stretch_height = math.floor(settings.stretch_height),
        lock_color = settings.lock_color or "#00000099",
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

    local function update()
        local layout = generate_wall(state.width, state.height)
        if not layout then
            return
        end

        local win_width, win_height = waywall.window_size()
        local inst_width = math.floor(win_width / state.width)
        local inst_height = math.floor(win_height / state.height)

        for i, _ in pairs(state.locked) do
            table.insert(layout, {
                "rectangle",
                state.lock_color,
                x = ((i - 1) % state.width) * inst_width,
                y = math.floor((i - 1) / state.width) * inst_height,
                w = inst_width,
                h = inst_height,
            })
        end

        waywall.set_layout(layout)
    end

    local function death(instance)
        state.locked[instance] = nil
        update()
    end

    wall.is_locked = function(instance)
        return state.locked[instance]
    end
    wall.lock = function(instance)
        if state.locked[instance] then
            return
        end

        state.locked[instance] = true
        update()
        waywall.set_priority(instance, true)
    end
    wall.unlock = function(instance)
        if not state.locked[instance] then
            return
        end

        state.locked[instance] = nil
        update()
        waywall.set_priority(instance, false)
    end
    wall.focus_reset = function()
        local hovered = waywall.hovered()
        if not hovered then
            return false
        end

        local num_instances = waywall.num_instances()
        for i = 1, num_instances do
            if i ~= hovered and check_reset(i) then
                waywall.reset(i)
            end
        end

        waywall.play(hovered)
        return true
    end
    wall.play = function()
        local hovered = waywall.hovered()
        if not hovered then
            return false
        end

        waywall.play(hovered)
        if state.locked[hovered] then
            state.locked[hovered] = nil
            waywall.set_priority(hovered, false)
            update()
        end
        return true
    end
    wall.reset = function()
        local hovered = waywall.hovered()
        if not hovered then
            return false
        end

        if check_reset(hovered) then
            waywall.reset(hovered)
        end
        return true
    end
    wall.reset_all = function()
        if waywall.active_instance() then
            return false
        end

        local num_instances = waywall.num_instances()
        local to_reset = {}
        for i = 1, num_instances do
            if check_reset(i) then
                table.insert(to_reset, i)
            end
        end

        waywall.reset(to_reset)
        return true
    end
    wall.reset_ingame = function()
        local active = waywall.active_instance()
        if not active then
            return false
        end

        if state.bypass then
            for lock, _ in pairs(state.locked) do
                local inst_state = waywall.instance(lock)
                if inst_state.screen == "inworld" then
                    state.locked[lock] = nil
                    waywall.set_priority(lock, false)
                    waywall.play(lock)

                    waywall.reset(active)
                    waywall.set_resolution(active, state.stretch_width, state.stretch_height)
                    update()
                    return true
                end
            end
        end

        waywall.goto_wall()
        waywall.reset(active)
        waywall.set_resolution(active, state.stretch_width, state.stretch_height)
        return true
    end
    wall.toggle_lock = function()
        local hovered = waywall.hovered()
        if not hovered then
            return false
        end

        if state.locked[hovered] then
            wall.unlock(hovered)
        else
            wall.lock(hovered)
        end
        return true
    end

    -- Assign the wall event listeners. Error if there was already a set of registered listeners.
    local previous = waywall.listen({
        death = death,
        install = update,
        resize = update,
        spawn = update,
    })

    if next(previous) ~= nil then
        error("overwrote event listeners")
    end

    return wall
end

package.loaded["waywall.helpers"] = M
