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

if __c_waywall_force_jit then
    __c_waywall_log("JIT has been enabled - broken layout generators may cause waywall to hang")
else
    jit.off()
    _G.jit = nil
end

local force_jit = __c_waywall_force_jit

--[[
    Layout generator code
]]--

local generator = nil

_G.__waywall_request = function(state, width, height)
    -- 1000000 is a fairly arbitrary upper limit on the number of instructions, but I don't think any
    -- reasonable layout generator should need to run that many.
    if not force_jit then
        debug.sethook(function()
            error("layout generator ran for too long")
        end, "", 1000000)
    end

    local ret = generator.request(state, width, height)
    if not ret or type(ret) ~= "table" then
        error("invalid return from layout generator: " .. tostring(ret))
    end

    if not force_jit then
        debug.sethook(nil, "")
    end

    return ret
end

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
    __c_waywall_log(str)
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
if not generator.init or type(generator.init) ~= "function" then
    error("layout generator did not provide init function")
end
if not generator.request or type(generator.request) ~= "function" then
    error("layout generator did not provide request function")
end

__c_waywall_config.generator_name = nil
generator.init(__c_waywall_config)
