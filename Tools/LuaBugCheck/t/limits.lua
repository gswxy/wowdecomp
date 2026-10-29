-- fixed build: deepest resume nesting / assignment list allowed must still run, one more errors cleanly
local depth = 0
local function a(f) depth = depth + 1 coroutine.wrap(f)(f) end
local ok, msg = pcall(a, a) assert(not ok and msg:find("C stack overflow"), msg)
io.write("resume depth reached ", depth, "\n")
local f, m = loadstring("local a;" .. ("a,"):rep(N) .. "a=nil", "") io.write("assign ", N, ": ", tostring(m), "\n")
