assert(((1 or false) and true) or false == true)
local v = ((1 or false) and true) or false assert(v == true, tostring(v))
