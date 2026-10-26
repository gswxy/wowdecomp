print = function(a, b) assert(a == 2 and b == 2, a .. " " .. b) end local h = io.open("orig-5.1.1-6.lua") assert(loadstring(h:read("*a")))()
