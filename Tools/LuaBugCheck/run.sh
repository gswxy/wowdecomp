#!/bin/sh
# Native Linux check of the LUA_OUR_BUGFIXES patches in Engine/Source/lua-5.1 (lua.org bug repro scripts).
# Builds the client's Lua twice (fixes on / off) with g++ and runs t/*.lua in each: with fixes on every
# script prints "ok"; with fixes off the bug repros error, crash or hang.
# Not covered by a script: 5.1.3-2 (needs internal asserts), 5.1.3-12 (needs a custom alignment).
# The 5.1.2-4 / 5.1.2-7 limits in luaconf.h were measured with this harness cross-built by clang-cl
# (/O2, /STACK:1048576) and run under Wine: t/limits.lua with LUA_N=<limit>.
set -e
HERE=$(cd "$(dirname "$0")" && pwd); SRC=$HERE/../../Engine/Source/lua-5.1/src; B=${1:-/tmp/luabugcheck}
for v in 1 0; do
  mkdir -p $B/$v
  for f in $SRC/*.c $HERE/harness.cpp; do
    g++ -x c++ -O1 -w -c -D'__int64=long long' -DLUA_OUR_BUGFIXES=$v -DLUA_USE_POSIX \
        -I$HERE/stub -I$HERE/../.. -I$SRC $f -o $B/$v/$(basename $f).o
  done
  g++ $B/$v/*.o -o $B/$v/lua -lm
done
cd $HERE/t
for v in 1 0; do
  echo "== LUA_OUR_BUGFIXES=$v"
  for f in 5.1.*.lua regress.lua; do
    r=$(timeout 20 $B/$v/lua $f 2>/dev/null | head -1 | cut -c1-120); echo "${r:-$f CRASH/HANG}"
  done
done
