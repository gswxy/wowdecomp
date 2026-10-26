local gp = {} gp.__newindex = function(s,_,_) seen = s end
local parent = {} parent.__newindex = parent setmetatable(parent, gp)
local child = setmetatable({}, parent) child.foo = 10 assert(seen == parent)
for i = 1, 1000 do local c = setmetatable({}, parent) c["k" .. i] = i end
