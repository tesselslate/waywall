local wall_width, wall_height, lock_color

local locked = {}
local instance_count = 0

local M = {}

local function generate_layout()
    local screen_width, screen_height = waywall.get_screen_size()
    local instance_width = math.floor(screen_width / wall_width)
    local instance_height = math.floor(screen_height / wall_height)

    local scene = {}

    for i = 0, instance_count - 1 do
        local x = i % wall_width
        local y = math.floor(i / wall_width)

        local scene_instance = {
            "instance", i,
            x = x * instance_width,
            y = y * instance_height,
            w = instance_width,
            h = instance_height,
        }
        table.insert(scene, scene_instance)

        if locked[i] and lock_color then
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

M.init = function(config, instances)
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

    wall_width = math.floor(config.wall_width)
    wall_height = math.floor(config.wall_height)

    if wall_width < 1 or wall_width > 10 then
        error("invalid wall width")
    end
    if wall_height < 1 or wall_height > 10 then
        error("invalid wall height")
    end

    instance_count = #instances
    for _, instance in ipairs(instances) do
        if instance.locked then
            locked[instance.id] = true
        end
    end
    return generate_layout()
end

M.instance_spawn = function(id)
    instance_count = instance_count + 1
    return generate_layout()
end

M.instance_die = function(id)
    instance_count = instance_count - 1
    locked[id] = nil
    for k, _ in pairs(locked) do
        if k > id then
            locked[k] = nil
            locked[k - 1] = true
        end
    end
    return generate_layout()
end

M.reset_ingame = function(id)
    locked[id] = nil
    return generate_layout()
end

M.lock = function(id)
    locked[id] = true
    return generate_layout()
end

M.unlock = function(id)
    locked[id] = nil
    return generate_layout()
end

M.resize = function(_, _)
    return generate_layout()
end

M.get_locked = function()
    local ids = {}
    for id, _ in pairs(locked) do
        table.insert(ids, id)
    end
    table.sort(ids)
    return ids
end

M.get_reset_all = function()
    local reset_all = {}
    for i = 0, instance_count - 1 do
        table.insert(reset_all, i)
    end
    return reset_all
end

return M
