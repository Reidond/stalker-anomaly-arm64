#!/bin/zsh
# Repeated start / play / quit runs on the iPad, to count heap aborts (libmalloc "pointer being freed was not allocated") and hangs.
#
#   tools/ios/scripts/heap_runs.sh [-n runs] [-s scenario[,scenario...]] [-m none|scribble|full] [-o outdir] [-t timeout-s]
#
#   scenarios  menu      cold start to the main menu, quit from the menu after 25 s
#              loading   load ipad_test, quit 30 s in (during the load)
#              play      load ipad_test, "press any key", walk a minute, quit from the level
#              churn     load ipad_test, teleport to four places of the Cordon 15 s apart (objects go online and offline, the spawn
#                        prefetcher works) and quit 3 s after the last teleport, with its queue full
#              Each run uses a cold shader cache (ANOMALY_CLEAR_SHADER_CACHE=1) unless the scenario is suffixed with "+warm".
#   -m         malloc debugging passed as environment variables (devicectl --environment-variables):
#                none      plain
#                scribble  MallocScribble MallocPreScribble MallocGuardEdges MallocErrorAbort
#                full      scribble + MallocCheckHeapStart/Each (every 1000th allocation after the 5000th checks the whole heap)
#                (MallocStackLogging is added to scribble and full: the abort report then names the allocating stack)
#   Runs go round-robin through the scenarios. The default is 12 runs of menu,loading,play with -m none.
#
# For each run it writes <outdir>/run-NN-<scenario>/{console.txt,shell.log,crash.log,engine.log,script.txt} and one line to <outdir>/results.txt:
#   run scenario cold|warm malloc result seconds quit <s>s   (quit: from "script asked to quit" to the shell's "quit (ok)")
# result is ok (the shell logged "anomaly: quit (ok)" and left no crash log), abort (libmalloc abort in the console output or the crash log),
# crash (another fatal signal), hang (timeout), or other. Needs the app installed and the iPad unlocked and the only user of it.
here=${0:A:h}
ipad=$here/../ipad.sh
runs=12; scenarios=(menu loading play); mode=none; out=ipad-heap-runs; limit=420
while getopts "n:s:m:o:t:" opt; do
    case $opt in
        n) runs=$OPTARG ;; s) scenarios=(${(s:,:)OPTARG}) ;; m) mode=$OPTARG ;; o) out=$OPTARG ;; t) limit=$OPTARG ;;
        *) sed -n 2,26p "$0"; exit 2 ;;
    esac
done
mkdir -p "$out"
bundle=com.andriishafar.anomaly

env_json() {
    local cold=$1 e='"ANOMALY_CLEAR_SHADER_CACHE":"1"'
    [[ $cold == warm ]] && e=''
    local add=''
    case $mode in
        scribble) add='"MallocScribble":"1","MallocPreScribble":"1","MallocGuardEdges":"1","MallocErrorAbort":"1","MallocStackLogging":"1"' ;;
        full) add='"MallocScribble":"1","MallocPreScribble":"1","MallocGuardEdges":"1","MallocErrorAbort":"1","MallocStackLogging":"1","MallocCheckHeapStart":"5000","MallocCheckHeapEach":"1000","MallocCheckHeapAbort":"1"' ;;
    esac
    [[ -n $e && -n $add ]] && e="$e,"
    echo "{$e$add}"
}

write_script() { # scenario file
    case $1 in
        menu) printf '%s\n' '25 quit' > $2 ;;
        loading) printf '%s\n' '8 console start server(ipad_test/single/alife/load) client(localhost)' '38 quit' > $2 ;;
        play) printf '%s\n' '8 console start server(ipad_test/single/alife/load) client(localhost)' \
            '60 key Space' '90 key Space' '120 key Space' '150 key Space' '180 keydown W' '195 keyup W' '205 keydown A' '215 keyup A' '230 quit' > $2 ;;
        churn) printf '%s\n' '8 console start server(ipad_test/single/alife/load) client(localhost)' \
            '45 key Space' '60 key Space' '62 console run_string ipad_debug.tp(54.9, 3.5, 157.1)' \
            '77 console run_string ipad_debug.tp(-14.5, 2.5, 274.7)' '92 console run_string ipad_debug.tp(-81.3, 1.5, 113.2)' \
            '107 console run_string ipad_debug.tp(-215.0, -7.5, -40.6)' '110 quit' > $2 ;;
        *) echo "unknown scenario $1" >&2; return 1 ;;
    esac
}

pull() { # container-relative path, local file
    $ipad get "$1" "$2" >/dev/null 2>&1
}

total=0; fails=0
for ((i = 1; i <= runs; i++)); do
    spec=${scenarios[$(( (i - 1) % $#scenarios + 1 ))]}
    sc=${spec%+warm}; cold=cold; [[ $spec == *+warm ]] && cold=warm
    d=$out/run-$(printf %02d $i)-$spec; mkdir -p $d
    write_script $sc $d/script.txt || exit 2
    export ANOMALY_LAUNCH_ENV="$(env_json $cold)"
    t0=$SECONDS
    # a stale crash log from the previous run must not be counted: the shell moves it aside at the next start, so remove our copies first
    ( $ipad kill >/dev/null 2>&1; $ipad script $d/script.txt -dbg > $d/console.txt 2>&1 ) &
    pid=$!
    ( sleep $limit; kill $pid 2>/dev/null; pkill -f "devicectl device process launch.*$bundle" 2>/dev/null ) &
    wd=$!
    wait $pid 2>/dev/null; rc=$?
    kill $wd 2>/dev/null; wait $wd 2>/dev/null
    secs=$(( SECONDS - t0 ))
    pull Documents/anomaly-shell.log $d/shell.log
    pull Documents/anomaly-crash.log $d/crash.log
    pull "Library/Application Support/Anomaly/appdata/logs/xray_mobile.log" $d/engine.log
    result=other
    if (( secs >= limit )); then result=hang
    elif [[ -s $d/crash.log ]] && grep -a -q -i "SIGABRT\|abort" $d/crash.log; then result=abort
    elif grep -a -q -i "pointer being freed\|malloc: \*\*\* error\|malloc: Heap corruption detected\|corruption detected" $d/console.txt $d/crash.log 2>/dev/null; then result=abort
    elif [[ -s $d/crash.log ]]; then result=crash
    elif grep -a -q "anomaly: quit (ok)" $d/shell.log $d/console.txt 2>/dev/null && grep -a -q "exit code 0" $d/console.txt; then result=ok
    fi
    (( total++ )); [[ $result != ok ]] && (( fails++ ))
    qa=$(grep -a "script asked to quit" $d/shell.log 2>/dev/null | tail -1 | sed -E 's/^\[ *([0-9.]+)\].*/\1/')
    qb=$(grep -a "anomaly: quit (" $d/shell.log 2>/dev/null | tail -1 | sed -E 's/^\[ *([0-9.]+)\].*/\1/')
    qt=-; [[ -n $qa && -n $qb ]] && qt=$(awk -v a=$qa -v b=$qb 'BEGIN{printf "%.2f", b - a}')
    echo "$i $spec $cold $mode $result ${secs}s quit ${qt}s" | tee -a $out/results.txt
done
echo "== $total runs, $fails not ok" | tee -a $out/results.txt
