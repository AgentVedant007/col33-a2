
#include "common.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unistd.h>
#include <vector>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = 5000;
    int num_ports = 1;
    long count = 10000;
    long nofile = 0;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--host" && i + 1 < argc)
            host = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (a == "--ports" && i + 1 < argc)
            num_ports = atoi(argv[++i]);
        else if (a == "--count" && i + 1 < argc)
            count = atol(argv[++i]);
        else if (a == "--nofile" && i + 1 < argc)
            nofile = atol(argv[++i]);
        else {
            fprintf(stderr,
                    "usage: %s [--host ip] [--port p] [--ports n] "
                    "[--count n] [--nofile n]\n",
                    argv[0]);
            return 1;
        }
    }
    if (num_ports < 1)
        num_ports = 1;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    long want = (nofile > 0) ? nofile : count + 1000;
    long limit = raise_fd_limit(want);
    printf("file descriptor limit: %ld (asked for %ld)\n", limit, want);
    if (limit < count + 10)
        printf("warning: the limit is below the requested connection count; "
               "raise kern.maxfiles and kern.maxfilesperproc\n");

    std::vector<int> fds;
    fds.reserve((size_t)count);
    std::map<int, long> failures;

    printf("opening %ld connections to %s ports %d..%d\n", count, host, port,
           port + num_ports - 1);

    long opened = 0;
    for (long i = 0; i < count && !g_stop; i++) {
        int p = port + (int)(i % num_ports);

        int fd = connect_to_server(host, p);
        if (fd < 0) {
            failures[errno]++;
            printf("connection %ld failed: %s\n", i + 1, strerror(errno));
            break;
        }
        fds.push_back(fd);
        opened++;

        if (opened % 5000 == 0) {
            printf("  %ld connections established\n", opened);
            fflush(stdout);
        }
    }

    printf("\n%ld connections are established and idle.\n", opened);
    for (std::map<int, long>::iterator it = failures.begin();
         it != failures.end(); ++it)
        printf("failures with %s: %ld\n", strerror(it->first), it->second);

    printf("\nTake the measurements now, for example:\n");
    printf("  sockstat -4 | grep -c ':%d'          (connections)\n", port);
    printf("  procstat -f <server pid> | wc -l      (open descriptors)\n");
    printf("  sysctl kern.openfiles kern.maxfiles   (system-wide)\n");
    printf("  netstat -m                            (socket buffer memory)\n");
    printf("  top -b -n 1 | grep exchange_server     (memory and CPU)\n");
    printf("\nPress Ctrl-C to close all the connections.\n");
    fflush(stdout);

    while (!g_stop)
        sleep(1);

    printf("\nclosing %zu connections\n", fds.size());
    for (size_t i = 0; i < fds.size(); i++)
        close(fds[i]);
    return 0;
}