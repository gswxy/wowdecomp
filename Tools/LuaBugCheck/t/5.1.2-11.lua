for i = 1, 200 do collectgarbage("step") assert(not pcall(debug.setfenv, 3, {})) end
