local f, msg = loadstring("local a;" .. ("a,"):rep(2^18) .. "a=nil", "") assert(not f and msg:find("variable names"), msg)
assert(loadstring("local a;" .. ("a,"):rep(200) .. "a=nil", ""))
