LABEL=$1
if [ -z "$LABEL" ]; then
    echo "usage: $0 <label, e.g. 10000>"
    exit 1
fi

mkdir -p ~/a2evidence
OUT=~/a2evidence/bonus_${LABEL}.txt

SRV=`pgrep -n exchange_server`
FLOOD=`pgrep -n conn_flood`

if [ -z "$SRV" ]; then
    echo "no exchange_server process found"
    exit 1
fi

{
echo "=========================================================="
echo " BONUS MEASUREMENT - target $LABEL idle connections"
echo " taken at: `date`"
echo " server pid: $SRV   generator pid: $FLOOD"
echo "=========================================================="
echo

echo "--- 1. connections actually established (server side) ---"
TCPFD=`procstat -f $SRV | grep -c TCP`
LISTEN=`sockstat -4 -l | grep -c exchange_s`
echo "server TCP descriptors : $TCPFD"
echo "of which listening     : $LISTEN"
echo "established connections: `expr $TCPFD - $LISTEN`"
echo
echo "cross-check with netstat (each loopback connection appears TWICE,"
echo "once per endpoint, so this number is about double the above):"
netstat -an -p tcp | grep -c ESTABLISHED
echo

echo "--- 2. server memory ---"
ps -o pid,rss,vsz,%cpu,%mem,time,comm -p $SRV
echo

echo "--- 3. server CPU (accumulated) ---"
ps -o pid,%cpu,time,comm -p $SRV
echo

echo "--- 4. open file descriptors held by the server ---"
echo "total descriptor lines (includes text/cwd/root/std*):"
procstat -f $SRV | wc -l
echo

echo "--- 5. system-wide open files ---"
sysctl kern.openfiles kern.maxfiles kern.maxfilesperproc
echo

echo "--- 6. socket buffer / mbuf usage ---"
netstat -m
echo
echo "socket buffer limits:"
sysctl kern.ipc.maxsockbuf net.inet.tcp.sendspace net.inet.tcp.recvspace
echo

echo "--- 7. kernel memory zones (per-connection structures) ---"
vmstat -z | head -1
vmstat -z | egrep '^(socket|tcpcb|tcp_inpcb|mbuf|mbuf_cluster):'
echo

echo "--- 8. wired kernel memory ---"
sysctl vm.stats.vm.v_wire_count hw.pagesize
echo "(wired bytes = v_wire_count * pagesize)"
echo

echo "--- 9. system memory ---"
sysctl hw.physmem vm.stats.vm.v_free_count
echo

if [ -n "$FLOOD" ]; then
echo "--- 10. generator process (for context, not part of the table) ---"
ps -o pid,rss,vsz,%cpu,time,comm -p $FLOOD
fi

echo
echo "=========================== end ==========================="
} 2>&1 | tee $OUT
