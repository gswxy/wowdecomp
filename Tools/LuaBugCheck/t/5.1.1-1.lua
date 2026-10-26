local a = {"x={1"} for i = 2, 2^18 + 10 do a[i] = 1 end a[#a + 1] = "}"
assert(loadstring(table.concat(a, ",")))() assert(#x == 2^18 + 10)
