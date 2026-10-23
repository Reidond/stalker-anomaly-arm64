#!/bin/zsh
# SIGTERM to the game while it is in the background, on the iPad: how long until the process is gone, and what the shell log says.
#
#   tools/ios/scripts/bg_term.sh [-n runs] [-o outdir]
#
# Each run: loads the save ipad_test (needs tools/ios/scripts/new_game.txt run once), waits for the level, launches Settings in front of the
# game (the game is backgrounded: WillEnterBackground, the background save, DidEnterBackground), waits 8 s, sends SIGTERM and polls the
# device's process list until the game is gone (limit 90 s, then SIGKILL). Writes <outdir>/run-NN/{console.txt,shell.log} and a line per run to results.txt:
#   run seconds-until-gone background-save-line quit-line
# Before the fix of main.cpp (quit while in the background) the process stayed until the system's SIGKILL, about 31 s.
here=${0:A:h}
ipad=$here/../ipad.sh
runs=3; out=ipad-bg-term
while getopts "n:o:" opt; do case $opt in n) runs=$OPTARG ;; o) out=$OPTARG ;; *) sed -n 2,12p "$0"; exit 2 ;; esac; done
mkdir -p $out
tmp=$(mktemp -d)
cat > $tmp/bg_term.txt <<EOT
8 console start server(ipad_test/single/alife/load) client(localhost)
45 key Space
60 key Space
400 quit
EOT
for ((i = 1; i <= runs; i++)); do
    d=$out/run-$(printf %02d $i); mkdir -p $d
    $ipad kill >/dev/null 2>&1
    ( $ipad script $tmp/bg_term.txt -dbg > $d/console.txt 2>&1 ) &
    launcher=$!
    sleep 85                                    # engine up, level loaded
    $ipad front com.apple.Preferences > $d/front.txt 2>&1
    sleep 8
    pid=$($ipad pid)
    t0=$SECONDS
    $ipad signal SIGTERM > $d/signal.txt 2>&1
    while [[ -n $($ipad pid) && $(( SECONDS - t0 )) -lt 90 ]]; do sleep 0.5; done
    secs=$(( SECONDS - t0 ))
    alive=no
    if [[ -n $($ipad pid) ]]; then   # it ignored SIGTERM (the build before the fix does): SIGKILL, like the system's after its grace period
        alive=yes; $ipad signal SIGKILL >/dev/null 2>&1; sleep 6
    fi
    kill $launcher 2>/dev/null; pkill -f "devicectl device process launch" 2>/dev/null; sleep 4
    $ipad get Documents/anomaly-shell.log $d/shell.log >/dev/null 2>&1
    echo "$i ${secs}s${alive:+ still-alive-at-limit=$alive} pid=$pid | $(grep -a 'background save' $d/shell.log | tail -1 | cut -c1-70) | $(grep -a 'SIGTERM\|quit (' $d/shell.log | tail -2 | tr '\n' ' ' | cut -c1-160)" | tee -a $out/results.txt
done
