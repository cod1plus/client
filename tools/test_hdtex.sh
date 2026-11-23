#!/bin/sh
# Host test of the HD-textures package: fake pk3s + manifest served by python, then
# tools/test_hdtex_install.cpp against it. Run from the repo root (Git Bash).
set -e
export PATH=/c/msys64/mingw32/bin:$PATH
SRV=build/hdtex_test/srv
rm -rf build/hdtex_test build/main_test
mkdir -p "$SRV"
head -c 300000 /dev/urandom > "$SRV/zz_hd_a.pk3"
head -c 120000 /dev/urandom > "$SRV/zz_hd_b.pk3"
python ../cod1pluspam/tools/make_manifest.py "$SRV" --mod main_test --url http://127.0.0.1:8767/ -o "$SRV/hdtex.manifest"
printf 'cvar com_hunkMegs 512\n' >> "$SRV/hdtex.manifest"
# same files, a cvar line nothing may accept
sed '/^cvar/d' "$SRV/hdtex.manifest" > "$SRV/hdtex_badcvar.manifest"
printf 'cvar com_hunkMegs "512;quit"\n' >> "$SRV/hdtex_badcvar.manifest"
# the same pack served by a Range-less server (port 8768)
sed 's#127.0.0.1:8767#127.0.0.1:8768#' "$SRV/hdtex.manifest" > "$SRV/hdtex_norange.manifest"
echo "--- manifest ---"; cat "$SRV/hdtex.manifest"
g++ -std=c++17 -I src tools/test_hdtex_install.cpp src/features/pam_install.cpp src/core/logger.cpp \
    -lwininet -ladvapi32 -o build/test_hdtex_install.exe
python tools/range_server.py "$SRV" 8767 &
HTTP=$!
python tools/range_server.py "$SRV" 8768 --no-range &
HTTP2=$!
sleep 1
set +e
./build/test_hdtex_install.exe
RC=$?
kill $HTTP $HTTP2 2>/dev/null
exit $RC
