local args = { ... }

if #args < 2 then
    print("embed.lua INPUT OUTPUT")
    os.exit(1)
end

local input = args[1]
local output = args[2]

local inf = assert(io.open(input, "rb"))
local data = inf:read("*all")

local outf = assert(io.open(output, "w"))
local cased_output = output:gsub(".", string.upper):gsub("%.", "_"):gsub("/", "_")

outf:write(string.format("#ifndef EMBED_%s\n#define EMBED_%s\n\n", cased_output, cased_output))
outf:write(string.format("static const char %s[] = {", cased_output))

for char in string.gmatch(data, ".") do
    outf:write(string.format("%d,", string.byte(char)))
end

outf:write("0};\n\n#endif\n")

assert(outf:close())
