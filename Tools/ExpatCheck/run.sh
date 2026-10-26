#!/bin/sh
# Compares the vendored expat (ThirdParty/expat) with expat 1.95.5 on every Interface\*.xml in the client:
# all MPQs in Data/ and Data/<locale>/ (via system StormLib, -lstorm) plus directory-style *.MPQ folders.
# Each XML is parsed the way XMLTree_Load does and the element/attribute/text event streams are diffed.
#   run.sh <client Data dir> <unpacked expat-1.95.5 dir> [work dir]
# 2026-10-27, expat 2.9.0: 731 files (503 distinct), 761,271 event lines, byte-identical; the 24 errors
# are 0-byte deletion stubs in patch MPQs and fail the same way in both.
set -e
HERE=$(cd "$(dirname "$0")" && pwd); NEW=$HERE/../../ThirdParty/expat; OLD=$2/lib; W=${3:-/tmp/expatcheck}
mkdir -p $W/out $W/cfg195
printf '#define BYTEORDER 1234\n#define HAVE_MEMMOVE 1\n#define XML_NS 1\n#define XML_DTD 1\n#define XML_CONTEXT_BYTES 1024\n' > $W/cfg195/expat_config.h
gcc -O2 -w $HERE/mpqxml.c -o $W/mpqxml -lstorm
gcc -O2 -w -I$W/cfg195 -I$OLD $HERE/xmlevents.c $OLD/xmlparse.c $OLD/xmlrole.c $OLD/xmltok.c -o $W/ev_old
gcc -O2 -w -I$NEW -I$NEW/lib $HERE/xmlevents.c $NEW/lib/xmlparse.c $NEW/lib/xmlrole.c $NEW/lib/xmltok.c \
    $NEW/lib/xcs.c $NEW/lib/random_dev_urandom.c -o $W/ev_new
$W/mpqxml $W/out $(ls "$1"/*.MPQ "$1"/*/*.MPQ 2>/dev/null) > $W/index.tsv 2>/dev/null || true
(cd "$1" && for d in *.MPQ */*.MPQ; do [ -d "$d" ] && find "$d" -iname '*.xml' -path '*nterface*'; done) |
    while read f; do mkdir -p "$W/out/loose/$(dirname "$f")"; cp "$1/$f" "$W/out/loose/$f"; done
find $W/out -iname '*.xml' | sort > $W/files.txt
$W/ev_old < $W/files.txt > $W/old.txt; $W/ev_new < $W/files.txt > $W/new.txt
echo "files: $(wc -l < $W/files.txt), ok: $(grep -c '^RESULT ok' $W/new.txt), events: $(wc -l < $W/new.txt)"
diff $W/old.txt $W/new.txt > $W/diff.txt && echo "event streams identical" || echo "DIFFERENT: see $W/diff.txt"
