// Runs each Lua file given on the command line in a fresh state; prints ok/error.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
uint64_t OsGetAsyncTimeClocks() { return 0; }
int64_t OsGetAsyncClocksPerSecond() { return 1000; }
#include "Engine/Source/Base/ConvertUTF.h"
#include "Engine/Source/Os/System.h"
// harness stand-ins (ASCII only; the tests do not use the UTF-8 paths)
uint32_t sgetu8(const uint8_t* p, int32_t* chars) { if (chars) *chars = *p ? 1 : 0; return *p; }
char* sputu8(uint32_t c, char* p) { *p++ = (char)c; return p; }
int main(int argc, char** argv) {
  int fails = 0;
  for (int i = 1; i < argc; i++) {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    if (getenv("LUA_N")) { lua_pushnumber(L, atof(getenv("LUA_N"))); lua_setglobal(L, "N"); }
    int r = luaL_loadfile(L, argv[i]) || lua_pcall(L, 0, 0, 0);
    printf("%-28s %s %s\n", argv[i], r ? "ERROR" : "ok", r ? lua_tostring(L, -1) : "");
    fails += r != 0;
    lua_close(L);
  }
  return fails;
}
