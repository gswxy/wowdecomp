local Var
local function main() NoSuchName (function() Var=0 end) end
local ok, msg = pcall(main) assert(not ok and msg:find("global 'NoSuchName'"), msg)
