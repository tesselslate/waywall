--[[
    This file is loaded when the layout generator VM is initially created.

    It attempts to initialize the user's chosen layout generator and marshals requests between it
    and waywall's C code. It performs some basic dummy-proofing to prevent broken layout generators
    from causing waywall to lock up, but DOES NOT do any proper sandboxing. Play stupid games, win
    stupid prizes.

    The "dummy-proofing" in question is to disable JIT compilation and set a hook to stop the layout
    generator if it runs for too long. Unfortunately, disabling JIT compilation is needed, since
    JIT'd methods will not trigger hooks.
]]--

local waywall = _G.waywall

if __c_waywall_force_jit then
    waywall.__c_waywall_log("JIT has been enabled - broken layout generators may cause waywall to hang")
else
    jit.off()
    _G.jit = nil
end

local force_jit = __c_waywall_force_jit

--[[
    Initialization
]]--

-- Override the "print" global to use wlr_log.
_G.print = function(...)
    local str = nil
    for _, v in ipairs({...}) do
        if str then
            str = str .. " " .. tostring(v)
        else
            str = tostring(v)
        end
    end
    waywall.__c_waywall_log(str)
end

-- Update the package path to include the configuration folder (for layout generators.)
local getenv = require("os").getenv
local xdg_config_home = getenv("XDG_CONFIG_HOME")
local home = getenv("HOME")
local path = nil
if xdg_config_home then
    path = xdg_config_home .. "/waywall/"
elseif home then
    path = home .. "./config/waywall/"
else
    error("could not find layout generator path (no $HOME or $XDG_CONFIG_HOME)")
end
package.path = package.path .. ";" .. path .. "?.lua"

-- Load the generator.
generator = require(__c_waywall_generator_name)
if not generator or type(generator) ~= "table" then
    error("layout generator did not provide any table")
end

_G.__generator_init = generator.init
_G.__generator_instance_spawn = generator.instance_spawn
_G.__generator_instance_die = generator.instance_die
_G.__generator_preview_start = generator.preview_start
_G.__generator_lock = generator.lock
_G.__generator_unlock = generator.unlock
_G.__generator_reset = generator.reset
_G.__generator_reset_all = generator.reset_all
_G.__generator_reset_ingame = generator.reset_ingame
_G.__generator_resize = generator.resize
_G.__generator_get_locked = generator.get_locked
_G.__generator_get_reset_all = generator.get_reset_all
