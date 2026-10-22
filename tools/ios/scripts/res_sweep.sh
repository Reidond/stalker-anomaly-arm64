#!/bin/zsh
# GPU ms and fps at several render sizes, in the same Cordon spot: tools/ios/scripts/res_sweep.sh [WxH ...] > out.txt
# (each run: load ipad_test, teleport to a field, wait 40 s, 20 s of windows). Needs the app installed and the iPad unlocked.
here=${0:A:h}
sizes=("$@"); (( $#sizes )) || sizes=(1024x768 1366x1024 1600x1200 1920x1440 2732x2048)
tmp=$(mktemp -d)
for res in $sizes; do
    cat > $tmp/res_$res.txt <<EOT
5 console vid_mode $res
8 console start server(ipad_test/single/alife/load) client(localhost)
45 key Space
50 console run_string ipad_debug.tp(54.9, 3.5, 157.1)
70 log RES $res
100 quit
EOT
    echo "== $res"
    $here/../ipad.sh script $tmp/res_$res.txt -dbg 2>&1 | tee -a ${TMPDIR:-/tmp}/res_sweep.raw | grep -a -E "RES|fps" | grep -a -v "script t=" | cut -c1-200
done
