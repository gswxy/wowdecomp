assert(select("#", unpack({1,2,3}, 2^31-1, 2^31-1)) == 1)
assert(select("#", unpack({1,2,3}, -2^31, -2^31)) == 1)
