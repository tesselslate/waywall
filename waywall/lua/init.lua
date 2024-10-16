--[[
    This script is the 2nd piece of code to run in the internal Lua VM. The
    API module is run first and inserts itself into `package.loaded`. This
    script is then called, and it in turn requires the user's `init.lua` to
    load their configuration.
]]

--[[
    Setup the environment for the user's configuration.
]]

local priv = _G.priv_waywall

-- User code should not have access to private Lua API functions.
_G.priv_waywall = nil

-- User code cannot directly use the coroutine library because waywall's Lua
-- interop code needs to know when coroutines should be resumed.
_G.coroutine = nil

-- The load* functions can be used to circumvent security measures in LuaJIT.
_G.load = nil
_G.loadfile = nil
_G.loadstring = nil

-- pcall and xpcall must be overridden to prevent user code from accidentally
-- catching the "instruction count exceeded" debug hook error.
local orig_pcall = _G.pcall
_G.pcall = function(fn, ...)
    local ret = { orig_pcall(fn, ...) }

    if not ret[1] and ret[2] == "instruction count exceeded" then
        error("instruction count exceeded")
    end

    return unpack(ret)
end

local orig_xpcall = _G.xpcall
_G.xpcall = function(fn, msgh, ...)
    local ret = { orig_xpcall(fn, msgh, ...) }

    if not ret[1] and ret[2] == "instruction count exceeded" then
        error("instruction count exceeded")
    end

    return unpack(ret)
end

-- The print function is overridden to annotate calls to Lua print() in stdout.
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

-- Lua does not provide a setenv function, so we do.
package.loaded["os"].setenv = priv.setenv

--[[
    Run the user's configuration.
]]

-- Setup the package path to include the waywall configuration directory.
local xdg_config_home = os.getenv("XDG_CONFIG_HOME")
local path = nil
if xdg_config_home then
    path = xdg_config_home .. "/waywall/"
else
    local home = os.getenv("HOME")
    if not home then
        error("no $XDG_CONFIG_HOME or $HOME")
    end
    path = home .. "/.config/waywall/"
end
package.path = package.path .. ";" .. path .. "?.lua"

-- Run the user's configuration file.
local user_config = require(priv.profile() or "init")

-- If the user's configuration does not return a table, then `require` will
-- return a boolean to say whether or not the call was successful.
if type(user_config) ~= "table" then
    return {}
end

return user_config
