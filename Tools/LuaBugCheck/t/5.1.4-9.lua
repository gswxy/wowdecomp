local a = (('hi' or true) and true) or true assert(a == true, tostring(a))
local b = ((nil and nil) or false) and true assert(b == false, tostring(b))
