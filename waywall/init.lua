--[[
    This script contains the first code to be run when the internal Lua VM is
    created. It sets up the environment for the user's configuration to run in,
    and then requires the user's `init.lua` file.

    The user's `init.lua` file should return a table containing their
    configuration. Any user-defined functions which then attempt to modify the
    configuration must do so through the (more limited) set of functions for
    that purpose, which are provided by C code.
]]--

-- Setup the environment for the user's configuration.
local priv = _G.priv_waywall
_G.priv_waywall = nil

_G.print = function(...)
    local str = nil
    for _, v in ipairs({...}) do
        if str then
            str = str .. " " .. tostring(v)
        else
            str = tostring(v)
        end
    end
    priv.log(str)
end

_G.waywall = {}
setmetatable(_G.waywall, {
    __metatable = "waywall",

    __index = function(_, k)
        return priv[k]
    end,
    __newindex = function(_, k, v)
        error("modification of the waywall table is not permitted")
    end
})

-- Setup the package path to include the waywall configuration directory.
local getenv = require("os").getenv
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
if type(user_config) ~= "table" then
    error(string.format(
        "expected config to be of type 'table', was '%s'",
        type(user_config)
    ))
end
return user_config
