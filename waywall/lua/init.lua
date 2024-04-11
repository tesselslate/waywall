--[[
    This script is the 2nd piece of code to run in the internal Lua VM. The
    API module is run first and inserts itself into `package.loaded`. This
    script is then called, and it in turn requires the user's `init.lua` to
    load their configuration.
]]

-- Setup the environment for the user's configuration.
local priv = _G.priv_waywall

local orig_pcall = _G.pcall
local orig_xpcall = _G.xpcall

_G.priv_waywall = nil
_G.load = nil
_G.loadfile = nil
_G.loadstring = nil

_G.pcall = function(fn, ...)
    local ret = { orig_pcall(fn, ...) }

    if not ret[1] and ret[2] == "instruction count exceeded" then
        error("instruction count exceeded")
    end

    return unpack(ret)
end

_G.xpcall = function(fn, msgh, ...)
    local ret = { orig_xpcall(fn, msgh, ...) }

    if not ret[1] and ret[2] == "instruction count exceeded" then
        error("instruction count exceeded")
    end

    return unpack(ret)
end

_G.print = function(...)
    local str = nil
    for _, v in ipairs({ ... }) do
        if str then
            str = str .. " " .. tostring(v)
        else
            str = tostring(v)
        end
    end
    priv.log(str)
end

-- Setup the package path to include the waywall configuration directory.
local getenv = priv.getenv
local xdg_config_home = getenv("XDG_CONFIG_HOME")
local path = nil
if xdg_config_home then
    path = xdg_config_home .. "/waywall/"
else
    local home = getenv("HOME")
    if not home then
        error("no $XDG_CONFIG_HOME or $HOME")
    end
    path = home .. "/.config/waywall/"
end
package.path = package.path .. ";" .. path .. "?.lua"

-- Run the user's configuration file.
local user_config = require("init")

-- If the user's configuration does not return a table, then `require` will
-- return a boolean to say whether or not the call was successful.
if type(user_config) ~= "table" then
    return {}
end

return user_config
