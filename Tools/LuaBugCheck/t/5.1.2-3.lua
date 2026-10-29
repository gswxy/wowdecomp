local bad debug.sethook(function(ev) if ev == "count" then bad = true end end, "cl")
local x = 0 for i = 1, 10 do x = x + i end debug.sethook() assert(not bad, "count hook called")
