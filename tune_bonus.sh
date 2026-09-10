if [ "$(id -u)" != "0" ]; then
    echo "must be run as root:  su -   then   sh $0"
    exit 1
fi

echo "=== BEFORE ==="
sysctl kern.maxfiles kern.maxfilesperproc kern.ipc.somaxconn \
       net.inet.ip.portrange.first net.inet.ip.portrange.last \
       net.inet.tcp.sendspace net.inet.tcp.recvspace

sysctl kern.maxfiles=500000
sysctl kern.maxfilesperproc=300000

sysctl kern.ipc.somaxconn=4096

sysctl net.inet.ip.portrange.first=10000
sysctl net.inet.ip.portrange.last=65535

echo ""
echo "=== AFTER ==="
sysctl kern.maxfiles kern.maxfilesperproc kern.ipc.somaxconn \
       net.inet.ip.portrange.first net.inet.ip.portrange.last

echo ""
echo "Tuning applied. Now run, as your normal user:"
echo "    cd ~/assignment2 && ./run_bonus.sh"
