#include "common.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

bool g_quiet = false;

void logf(const char *fmt, ...) {
    if (g_quiet)
        return;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tmv;
    time_t secs = (time_t)tv.tv_sec;
    localtime_r(&secs, &tmv);

    fprintf(stderr, "[%02d:%02d:%02d.%03d] ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
            (int)(tv.tv_usec / 1000));

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n");
    fflush(stderr);
}

std::string escape(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c >= 32 && c < 127)
            out += c;
        else
            out += '.';
    }
    return out;
}


static bool fill_addr(const char *ip, int port, struct sockaddr_in &addr) {
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "not a valid IPv4 address: %s\n", ip);
        return false;
    }
    return true;
}

int make_listening_socket(const char *ip, int port) {
    struct sockaddr_in addr;
    if (!fill_addr(ip, port, addr))
        return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 1024) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int connect_to_server(const char *ip, int port) {
    struct sockaddr_in addr;
    if (!fill_addr(ip, port, addr))
        return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved = errno;
        perror("connect");
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl F_GETFL");
        return;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        perror("fcntl F_SETFL");
}

static std::string addr_to_string(const struct sockaddr_in &a) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    char buf[64];
    snprintf(buf, sizeof(buf), "%s:%d", ip, ntohs(a.sin_port));
    return std::string(buf);
}

std::string local_addr(int fd) {
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &len) < 0)
        return "?";
    return addr_to_string(a);
}

std::string peer_addr(int fd) {
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    if (getpeername(fd, (struct sockaddr *)&a, &len) < 0)
        return "?";
    return addr_to_string(a);
}


long current_fd_limit() {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0)
        return -1;
    return (long)rl.rlim_cur;
}

long raise_fd_limit(long want) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0) {
        perror("getrlimit");
        return -1;
    }
    if ((long)rl.rlim_cur >= want)
        return (long)rl.rlim_cur;

    rlim_t target = (rlim_t)want;
    if (rl.rlim_max != RLIM_INFINITY && target > rl.rlim_max)
        target = rl.rlim_max;

    rl.rlim_cur = target;
    if (setrlimit(RLIMIT_NOFILE, &rl) < 0) {
        perror("setrlimit");
        return current_fd_limit();
    }
    return (long)target;
}


std::vector<std::string> split_words(const std::string &s) {
    std::vector<std::string> words;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            i++;
        size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t')
            i++;
        if (i > start) {
            std::string w = s.substr(start, i - start);
            if (!w.empty() && w[w.size() - 1] == '\r')
                w.erase(w.size() - 1);
            if (!w.empty())
                words.push_back(w);
        }
    }
    return words;
}

bool parse_int(const std::string &s, long lo, long hi, long &out) {
    if (s.empty() || s.size() > 10)
        return false;
    long v = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + (s[i] - '0');
    }
    if (v < lo || v > hi)
        return false;
    out = v;
    return true;
}

bool extract_line(std::string &buf, std::string &line) {
    size_t pos = buf.find('\n');
    if (pos == std::string::npos)
        return false;
    line = buf.substr(0, pos);
    buf.erase(0, pos + 1);
    return true;
}


static bool send_string(int fd, const std::string &s) {
    size_t sent = 0;
    while (sent < s.size()) {
        ssize_t n = send(fd, s.data() + sent, s.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("send");
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

int run_client(const char *ip, int port, const std::vector<std::string> &startup) {
    signal(SIGPIPE, SIG_IGN);

    int fd = connect_to_server(ip, port);
    if (fd < 0)
        return 1;

    logf("connected: local %s -> server %s", local_addr(fd).c_str(),
         peer_addr(fd).c_str());

    for (size_t i = 0; i < startup.size(); i++) {
        logf("sending: %s", startup[i].c_str());
        if (!send_string(fd, startup[i] + "\n")) {
            close(fd);
            return 1;
        }
    }

    std::string sock_buf;
    std::string stdin_buf;
    bool stdin_open = true;

    for (;;) {
        struct pollfd fds[2];
        int nfds = 0;

        fds[nfds].fd = fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;

        if (stdin_open) {
            fds[nfds].fd = STDIN_FILENO;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        int ready = poll(fds, nfds, -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if (fds[0].revents != 0) {
            char buf[4096];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n == 0) {
                logf("server closed the connection");
                break;
            }
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                perror("recv");
                break;
            }
            sock_buf.append(buf, (size_t)n);

            std::string line;
            while (extract_line(sock_buf, line)) {
                printf("%s\n", line.c_str());
                fflush(stdout);
            }
        }

        if (nfds > 1 && fds[1].revents != 0) {
            char buf[1024];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                stdin_open = false;
                logf("stdin closed; still listening for messages "
                     "(Ctrl-C to quit)");
                continue;
            }
            stdin_buf.append(buf, (size_t)n);

            std::string line;
            while (extract_line(stdin_buf, line)) {
                if (line.empty())
                    continue;
                if (!send_string(fd, line + "\n")) {
                    close(fd);
                    return 1;
                }
            }
        }
    }

    close(fd);
    return 0;
}