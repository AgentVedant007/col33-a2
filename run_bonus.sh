set -u

HOST=127.0.0.1
PORT=5000
PORTS=4
NOFILE=200000
COUNTS="0 10000 20000 30000 40000 50000 60000 70000"
OUTDIR=$HOME/a2evidence/bonus
TIMEWAIT_LIMIT=2000
TIMEWAIT_MAXWAIT=180
DO_TRADEOFF=1


mkdir -p "$OUTDIR"
cd "$(dirname "$0")" || exit 1

say() { echo ""; echo "=== $* ==="; }

cleanup() {
    pkill conn_flood 2>/dev/null
    pkill exchange_server 2>/dev/null
}
trap 'echo ""; echo "interrupted - cleaning up"; cleanup; exit 1' INT TERM


if [ ! -x ./bin/exchange_server ] || [ ! -x ./bin/conn_flood ]; then
    echo "bin/exchange_server or bin/conn_flood missing - run 'make' first."
    exit 1
fi

AM_ROOT=0
[ "$(id -u)" = "0" ] && AM_ROOT=1
if [ $AM_ROOT -eq 0 ]; then
    echo "WARNING: not running as root."
    echo "Kernel limits cannot be raised, so large runs will fail early."
    echo "That is still a valid (reportable) result, but re-running as root"
    echo "is strongly recommended. Continuing in 10 seconds..."
    sleep 10
fi


say "recording default kernel limits"
{
    echo "collected: $(date)"
    uname -a
    sysctl kern.maxfiles kern.maxfilesperproc kern.ipc.somaxconn \
           kern.ipc.maxsockbuf net.inet.tcp.sendspace net.inet.tcp.recvspace \
           net.inet.ip.portrange.first net.inet.ip.portrange.last hw.physmem
} 2>&1 | tee "$OUTDIR/00_defaults.txt"

if [ $AM_ROOT -eq 1 ]; then
    say "raising kernel limits"
    sysctl kern.maxfiles=500000            >/dev/null 2>&1
    sysctl kern.maxfilesperproc=300000     >/dev/null 2>&1
    sysctl kern.ipc.somaxconn=4096         >/dev/null 2>&1
    sysctl net.inet.ip.portrange.first=10000 >/dev/null 2>&1
    sysctl net.inet.ip.portrange.last=65535  >/dev/null 2>&1
fi
{
    echo "collected: $(date)"
    sysctl kern.maxfiles kern.maxfilesperproc kern.ipc.somaxconn \
           net.inet.ip.portrange.first net.inet.ip.portrange.last
} 2>&1 | tee "$OUTDIR/00_tuned.txt"

zone_used() {
    vmstat -z 2>/dev/null | awk -F'[:,]' -v z="$1" \
        '$1 == z { gsub(/ /,"",$4); print $4; exit }'
}

wait_for_timewait() {
    waited=0
    while [ $waited -lt $TIMEWAIT_MAXWAIT ]; do
        tw=$(netstat -an -p tcp 2>/dev/null | grep -c TIME_WAIT)
        [ "$tw" -lt $TIMEWAIT_LIMIT ] && return 0
        sleep 5
        waited=$((waited + 5))
    done
    return 0
}


measure() {
    LABEL=$1
    SRV=$2
    F="$OUTDIR/run_${LABEL}.txt"

    TCPFD=$(procstat -f "$SRV" 2>/dev/null | grep -c TCP)
    EST=$((TCPFD - PORTS))
    [ $EST -lt 0 ] && EST=0

    RSS=$(ps -o rss= -p "$SRV" 2>/dev/null | tr -d ' ')
    VSZ=$(ps -o vsz= -p "$SRV" 2>/dev/null | tr -d ' ')
    PCPU=$(ps -o %cpu= -p "$SRV" 2>/dev/null | tr -d ' ')
    CPUTIME=$(ps -o time= -p "$SRV" 2>/dev/null | tr -d ' ')
    FDS=$(procstat -f "$SRV" 2>/dev/null | wc -l | tr -d ' ')
    OPENFILES=$(sysctl -n kern.openfiles 2>/dev/null)
    MAXFILES=$(sysctl -n kern.maxfiles 2>/dev/null)
    CLUSTERS=$(netstat -m 2>/dev/null | awk '/mbuf clusters in use/ \
        { split($1, a, "/"); print a[1] "/" a[4]; exit }')
    WIREPAGES=$(sysctl -n vm.stats.vm.v_wire_count 2>/dev/null)
    PAGESIZE=$(sysctl -n hw.pagesize 2>/dev/null)
    WIREDMB=$(( (WIREPAGES * PAGESIZE) / 1048576 ))
    ZSOCK=$(zone_used "socket")
    ZTCPCB=$(zone_used "tcpcb")
    ZINPCB=$(zone_used "tcp_inpcb")

    {
        echo "=========================================================="
        echo " BONUS MEASUREMENT - label $LABEL"
        echo " taken at: $(date)"
        echo " server pid: $SRV   listening ports: $PORTS"
        echo "=========================================================="
        echo
        echo "--- 1. connections established (server side) ---"
        echo "server TCP descriptors : $TCPFD"
        echo "listening sockets      : $PORTS"
        echo "ESTABLISHED CONNECTIONS: $EST"
        echo
        echo "netstat cross-check (loopback shows BOTH endpoints, so this is"
        echo "roughly twice the number above):"
        netstat -an -p tcp 2>/dev/null | grep -c ESTABLISHED
        echo
        echo "--- 2. server memory ---"
        ps -o pid,rss,vsz,%cpu,%mem,time,comm -p "$SRV"
        echo "RSS(KB)=$RSS  VSZ(KB)=$VSZ"
        echo
        echo "--- 3. server CPU ---"
        echo "%CPU=$PCPU  accumulated TIME=$CPUTIME"
        echo
        echo "--- 4. open file descriptors held by the server ---"
        echo "procstat -f lines: $FDS"
        procstat -f "$SRV" 2>/dev/null | head -12
        echo "  ... (truncated; TCP descriptor count above)"
        echo
        echo "--- 5. system-wide open files ---"
        echo "kern.openfiles=$OPENFILES  kern.maxfiles=$MAXFILES"
        sysctl kern.openfiles kern.maxfiles kern.maxfilesperproc 2>/dev/null
        echo
        echo "--- 6. socket buffer / mbuf usage ---"
        netstat -m 2>/dev/null
        echo
        sysctl kern.ipc.maxsockbuf net.inet.tcp.sendspace \
               net.inet.tcp.recvspace 2>/dev/null
        echo
        echo "--- 7. kernel zones (per-connection structures) ---"
        vmstat -z 2>/dev/null | head -1
        vmstat -z 2>/dev/null | egrep '^(socket|tcpcb|tcp_inpcb|mbuf|mbuf_cluster):'
        echo "socket=$ZSOCK tcpcb=$ZTCPCB tcp_inpcb=$ZINPCB"
        echo
        echo "--- 8. wired kernel memory ---"
        echo "v_wire_count=$WIREPAGES pagesize=$PAGESIZE  => ${WIREDMB} MB"
        echo
        echo "--- 9. system memory ---"
        sysctl hw.physmem vm.stats.vm.v_free_count 2>/dev/null
        echo
        echo "=========================== end ==========================="
    } > "$F" 2>&1

    echo "$LABEL|$EST|$RSS|$VSZ|$PCPU|$CPUTIME|$FDS|$OPENFILES|$MAXFILES|$CLUSTERS|$WIREDMB|$ZSOCK|$ZTCPCB|$ZINPCB" \
        >> "$OUTDIR/.rows"

    echo "  established=$EST  rss=${RSS}KB  fds=$FDS  openfiles=$OPENFILES  wired=${WIREDMB}MB"
}


do_run() {
    TARGET=$1
    LABEL=$2

    say "run: target $TARGET connections (label $LABEL)"

    cleanup
    sleep 2

    ./bin/exchange_server "$HOST" "$PORT" --ports "$PORTS" -q \
        --nofile "$NOFILE" > "$OUTDIR/server_${LABEL}.log" 2>&1 &
    sleep 3

    SRV=$(pgrep -n exchange_server)
    if [ -z "$SRV" ]; then
        echo "  ERROR: server failed to start. See server_${LABEL}.log"
        cat "$OUTDIR/server_${LABEL}.log"
        return 1
    fi

    if [ "$TARGET" -gt 0 ]; then
        echo "  opening connections (this can take a while)..."
        ./bin/conn_flood --host "$HOST" --port "$PORT" --ports "$PORTS" \
            --count "$TARGET" --nofile "$NOFILE" \
            > "$OUTDIR/flood_${LABEL}.log" 2>&1 &

        waited=0
        limit=$(( TARGET / 200 + 120 ))
        while [ $waited -lt $limit ]; do
            if grep -q "are established and idle" "$OUTDIR/flood_${LABEL}.log" 2>/dev/null; then
                break
            fi
            if ! pgrep -n conn_flood >/dev/null 2>&1; then
                echo "  WARNING: generator exited early"
                break
            fi
            sleep 5
            waited=$((waited + 5))
        done
        sleep 3

        if grep -q "failed:" "$OUTDIR/flood_${LABEL}.log" 2>/dev/null; then
            echo "  NOTE: generator reported a failure --"
            grep "failed:" "$OUTDIR/flood_${LABEL}.log" | head -3
        fi
    fi

    measure "$LABEL" "$SRV"

    cleanup
    echo "  waiting for TIME_WAIT to drain..."
    wait_for_timewait
}


rm -f "$OUTDIR/.rows"

for N in $COUNTS; do
    do_run "$N" "$N"
done


if [ $DO_TRADEOFF -eq 1 ] && [ $AM_ROOT -eq 1 ]; then
    say "trade-off run: 70000 connections with 4KB socket buffers"
    OLD_SND=$(sysctl -n net.inet.tcp.sendspace)
    OLD_RCV=$(sysctl -n net.inet.tcp.recvspace)
    sysctl net.inet.tcp.sendspace=4096 >/dev/null 2>&1
    sysctl net.inet.tcp.recvspace=4096 >/dev/null 2>&1

    do_run 70000 "70000_smallbuf"

    sysctl net.inet.tcp.sendspace="$OLD_SND" >/dev/null 2>&1
    sysctl net.inet.tcp.recvspace="$OLD_RCV" >/dev/null 2>&1
    echo "  socket buffer defaults restored ($OLD_SND / $OLD_RCV)"
fi


say "building summary table"

{
    echo "Bonus resource measurements"
    echo "collected: $(date)"
    echo "host: $(uname -srm)"
    echo "server: kqueue, single process, single thread, $PORTS listening ports"
    echo ""
    printf "%-16s %10s %10s %8s %8s %10s %10s %8s %8s\n" \
        "TARGET" "ESTAB" "RSS(KB)" "%CPU" "SRV_FDS" "OPENFILES" "CLUSTERS" "WIRED_MB" "SOCKETS"
    echo "---------------------------------------------------------------------------------------------------"
    while IFS='|' read -r label est rss vsz pcpu cputime fds openf maxf clus wired zsock ztcpcb zinpcb; do
        printf "%-16s %10s %10s %8s %8s %10s %10s %8s %8s\n" \
            "$label" "$est" "$rss" "$pcpu" "$fds" "$openf" "$clus" "$wired" "$zsock"
    done < "$OUTDIR/.rows"
    echo ""
    echo "Column notes:"
    echo "  ESTAB     connections the server actually held (TCP fds minus listeners)"
    echo "  RSS       server resident memory, KB"
    echo "  SRV_FDS   descriptor lines from procstat -f on the server"
    echo "  OPENFILES kern.openfiles, system-wide (max: $(sysctl -n kern.maxfiles))"
    echo "  CLUSTERS  mbuf clusters in use (netstat -m)"
    echo "  WIRED_MB  wired kernel memory, MB"
    echo "  SOCKETS   'socket' zone entries in use (vmstat -z)"
    echo ""
    echo "Failures reported by the generator:"
    grep -H "failed:" "$OUTDIR"/flood_*.log 2>/dev/null || echo "  none"
} | tee "$OUTDIR/SUMMARY.txt"

{
    echo "target,established,rss_kb,vsz_kb,pcpu,cputime,server_fds,openfiles,maxfiles,mbuf_clusters,wired_mb,zone_socket,zone_tcpcb,zone_inpcb"
    tr '|' ',' < "$OUTDIR/.rows"
} > "$OUTDIR/SUMMARY.csv"

say "done"
echo "Everything is in $OUTDIR"
echo ""
echo "Screenshot these three files for the report (required by the assignment):"
echo "   $OUTDIR/run_10000.txt"
echo "   $OUTDIR/run_40000.txt"
echo "   $OUTDIR/run_70000.txt"
echo "and the summary table:"
echo "   $OUTDIR/SUMMARY.txt"