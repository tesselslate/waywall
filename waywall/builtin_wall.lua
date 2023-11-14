--[[
    This file implements the built-in "wall" layout generator, which creates the classic wall
    layout. Instances are displayed in a grid with colored rectangles overlaid on top of locked
    instances.

    This is also intended to serve as an example layout generator, and is thoroughly commented.
]]--

local wall_width, wall_height, lock_color
local M = {}

--[[
    The init function is called once whenever the layout generator is initialized. Whenever the
    user changes their configuration, the layout generator (Lua VM) is destroyed and remade.
]]--
M.init = function(config)
    if not config or type(config) ~= "table" then
        error("no config")
    end
    if not config.wall_width or type(config.wall_width) ~= "number" then
        error("no wall width")
    end
    if not config.wall_height or type(config.wall_height) ~= "number" then
        error("no wall height")
    end
    if config.lock_color and type(config.lock_color) == "string" then
        lock_color = config.lock_color
    end

    local math = require("math")
    wall_width = math.floor(config.wall_width)
    wall_height = math.floor(config.wall_height)

    if wall_width < 1 or wall_width > 10 then
        error("invalid wall width")
    end
    if wall_height < 1 or wall_height > 10 then
        error("invalid wall height")
    end
end

--[[
    The request function is called every time waywall wants a new wall layout as a result of some
    event (e.g. instance state changed, instance booted up, instance died, etc.) Layout requests
    are only ever made when the wall is visible (the user is not playing an instance).

    instances is a table containing the state of all instances. Here is an example of its format:
    {
        {
            id = 0
            locked = false,
            screen = "title",
        },
        {
            id = 1,
            locked = false,
            screen = "inworld",
        },
        {
            id = 2,
            locked = false,
            screen = "generating",
            percent = 10,
        },
        {
            id = 3,
            locked = true,
            screen = "previewing",
            percent = 40,
            preview_start = 2438102,
        }
    }

    - The `id`, `locked`, and `screen` fields are always included.
    - `percent` is included when the instance is in worldgen (`generating` or `previewing`.)
    - `preview_start` contains the time at which the world preview started in milliseconds from
      an arbitrary epoch (it uses CLOCK_MONOTONIC.)

    width and height contain the current dimensions of the waywall window.

    The return value of this function will tell waywall what to display to the user. It consists
    of an array of "entries", each of which can either be an instance or colored rectangle.
]]--
M.request = function(instances, screen_width, screen_height)
    local instance_width = math.floor(screen_width / wall_width)
    local instance_height = math.floor(screen_height / wall_height)

    local scene = {}

    for _, instance in ipairs(instances) do
        local x = instance.id % wall_width
        local y = math.floor(instance.id / wall_width)

        local scene_instance = {
            "instance",
            instance.id,
            x = x * instance_width,
            y = y * instance_height,
            w = instance_width,
            h = instance_height,
        }
        table.insert(scene, scene_instance)

        if instance.locked and lock_color then
            local scene_lock = {
                "rectangle",
                x = x * instance_width,
                y = y * instance_height,
                w = instance_width,
                h = instance_height,
                color = lock_color,
            }
            table.insert(scene, scene_lock)
        end
    end

    return scene
end

return M
