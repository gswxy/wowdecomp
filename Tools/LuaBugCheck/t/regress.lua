-- sanity: ordinary code behaves the same either way
local t = {} for i = 1, 100 do t[i] = i * 2 end assert(#t == 100 and t[50] == 100)
assert(select("#", unpack({1,2,3})) == 3 and select("#", unpack({}, 1, 0)) == 0)
local co = coroutine.wrap(function(a) local b = coroutine.yield(a + 1) return b * 2 end)
assert(co(1) == 2 and co(5) == 10)
local x, y = 3, 4 assert(-x == -3 and x < y and not (x > y) and (x or y) == 3)
local s = "abc" assert(s:upper() == "ABC" and ("%d"):format(5) == "5")
