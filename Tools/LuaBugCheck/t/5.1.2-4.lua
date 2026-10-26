local a = function(a) coroutine.wrap(a)(a) end
local ok, msg = pcall(a, a) assert(not ok and msg:find("C stack overflow"), msg)
