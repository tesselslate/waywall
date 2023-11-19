local grid_width, grid_height, lock_count

local queue = {}
local locked = {}

local M = {}

local function fill_queue()
    local instances = waywall.get_instances()

    local available = {}
    local unavailable = {}
    for _, id in ipairs(queue) do
        unavailable[id] = true
    end
    for _, id in ipairs(locked) do
        unavailable[id] = true
    end

    for _, instance in ipairs(instances) do
        if not unavailable[instance.id] then
            local progress
            if instance.screen == "title" then
                progress = 1000
            elseif instance.screen == "inworld" then
                progress = 100
            elseif instance.screen == "previewing" then
                progress = instance.gen_percent
            end

            if progress then
                table.insert(available, {
                    id = instance.id,
                    progress = progress,
                })
            end
        end
    end

    table.sort(available, function(lhs, rhs) return lhs.progress < rhs.progress end)
    for _, instance in ipairs(available) do
        if #queue == grid_width * grid_height then
            return
        end
        table.insert(queue, instance.id)
    end
end

local function generate_layout()
    local screen_width, screen_height = waywall.get_screen_size()
    local lock_width = math.floor(screen_width / lock_count)
    local instance_width = math.floor(screen_width / grid_width)
    local instance_height = math.floor(math.floor(screen_height * 0.8) / grid_height)

    local scene = {}

    for i, id in ipairs(queue) do
        if id >= 0 then
            local x = (i - 1) % grid_width
            local y = math.floor((i - 1) / grid_width)

            local scene_instance = {
                "instance", id,
                x = x * instance_width,
                y = y * instance_height,
                w = instance_width,
                h = instance_height,
            }
            table.insert(scene, scene_instance)
        end
    end

    for i, id in ipairs(locked) do
        local scene_instance = {
            "instance", id,
            x = (i - 1) * lock_width,
            y = math.floor(screen_height * 0.8),
            w = lock_width,
            h = math.floor(screen_height * 0.2),
        }
        table.insert(scene, scene_instance)
    end

    return scene
end

local function remove_from_queue(id)
    for i, v in ipairs(queue) do
        if v == id then
            queue[i] = -1
            return
        end
    end
end

M.init = function(config, instances)
    grid_width = config.grid_width or 2
    grid_height = config.grid_height or 2
    lock_count = config.lock_count or 6
    if type(grid_width) ~= "number" or grid_width < 1 then
        error("grid width not a number")
    end
    if type(grid_height) ~= "number" or grid_height < 1 then
        error("grid height not a number")
    end
    if type(lock_count) ~= "number" or lock_count < 1 then
        error("lock count not a number")
    end

    fill_queue()
    return generate_layout()
end

M.instance_spawn = function(id)
    if #queue < grid_width * grid_height then
        table.insert(queue, id)
        return generate_layout()
    end
end

M.instance_die = function(id)
    for i, v in ipairs(queue) do
        if v == id then
            table.remove(queue, i)
        elseif v > id then
            queue[i] = v - 1
        end
    end

    for i, v in ipairs(locked) do
        if v == id then
            table.remove(locked, i)
        elseif v > id then
            locked[i] = v - 1
        end
    end

    return generate_layout()
end

M.preview_start = function(id)
    if #queue < grid_width * grid_height then
        table.insert(queue, id)
        return generate_layout()
    end
end

M.lock = function(id)
    remove_from_queue(id)
    table.insert(locked, id)
    return generate_layout()
end

M.unlock = function(id)
    for i, v in ipairs(locked) do
        if v == id then
            table.remove(locked, i)
            return generate_layout()
        end
    end
end

M.reset = function(id)
    remove_from_queue(id)
    return generate_layout()
end

M.reset_all = function()
    queue = {}
    fill_queue()
    return generate_layout()
end

M.reset_ingame = function(id)
    remove_from_queue(id)
    for i, v in ipairs(locked) do
        if v == id then
            table.remove(locked, i)
        end
    end

    return generate_layout()
end

M.resize = function(_, _)
    return generate_layout()
end

M.get_locked = function()
    return locked
end

M.get_reset_all = function()
    local reset_all = {}

    for _, id in ipairs(queue) do
        if id ~= -1 then
            table.insert(reset_all, id)
        end
    end

    return reset_all
end

return M
