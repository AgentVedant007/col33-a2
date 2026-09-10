#include "common.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>


static const long VALUE_MIN = 1;
static const long VALUE_MAX = 2147483647L;
static const long ORDER_ID_MIN = 0;
static const long ORDER_ID_MAX = 2147483647L;

static const int INST_JNST = 0;
static const int INST_IMCT = 1;
static const int NUM_INSTRUMENTS = 2;

static const char *instrument_name(int inst) {
    return inst == INST_JNST ? "JNST" : "IMCT";
}

static int parse_instrument(const std::string &s) {
    if (s == "JNST")
        return INST_JNST;
    if (s == "IMCT")
        return INST_IMCT;
    return -1;
}

static const int ROLE_UNKNOWN = 0;
static const int ROLE_TRADER = 1;
static const int ROLE_MARKET_DATA = 2;

static const size_t MAX_LINE = 1024;


struct Client {
    int fd;
    int id;
    int role;
    std::string username;

    std::string inbuf;
    std::string outbuf;

    bool subscribed[NUM_INSTRUMENTS];
    bool watching_write;
    bool send_blocked;
    bool remove_me;
};

struct Order {
    int id;
    bool is_buy;
    int instrument;
    long price;
    long quantity;
    int owner_id;
};

static int g_kq = -1;
static std::map<int, Client *> g_clients;
static std::set<std::string> g_usernames;
static std::set<Client *> g_subscribers[NUM_INSTRUMENTS];
static std::vector<Client *> g_dead;
static std::vector<Order> g_book;
static int g_next_order_id = 1;
static int g_next_client_id = 1;
static size_t g_live_clients = 0;

static Client *find_client_by_id(int id) {
    std::map<int, Client *>::iterator it = g_clients.find(id);
    if (it == g_clients.end() || it->second->remove_me)
        return NULL;
    return it->second;
}


static void kq_set(int fd, int filter, int flags, void *udata) {
    struct kevent change;
    EV_SET(&change, fd, filter, flags, 0, 0, udata);
    if (kevent(g_kq, &change, 1, NULL, 0, NULL) < 0 && errno != ENOENT)
        logf("kevent() change failed for fd %d: %s", fd, strerror(errno));
}

static void want_write(Client *c, bool on) {
    if (on == c->watching_write)
        return;
    kq_set(c->fd, EVFILT_WRITE, on ? EV_ADD : EV_DELETE, c);
    c->watching_write = on;
}


static void flush_output(Client *c) {
    while (!c->outbuf.empty()) {
        ssize_t n = send(c->fd, c->outbuf.data(), c->outbuf.size(), 0);
        if (n > 0) {
            c->outbuf.erase(0, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!c->send_blocked) {
                c->send_blocked = true;
                logf("client %d: send() would block, %zu bytes are now queued "
                     "in the server for this client (it is not reading)",
                     c->id, c->outbuf.size());
            }
            want_write(c, true);
            return;
        }
        logf("client %d: send() failed (%s), dropping the connection", c->id,
             strerror(errno));
        c->remove_me = true;
        return;
    }

    if (c->send_blocked) {
        c->send_blocked = false;
        logf("client %d: started reading again, the queued data has been sent",
             c->id);
    }
    want_write(c, false);
}

static void send_line(Client *c, const std::string &msg) {
    if (c->remove_me)
        return;
    c->outbuf += msg;
    c->outbuf += "\n";
    flush_output(c);
}

static void broadcast_trade(int instrument, long quantity, long price) {
    char msg[128];
    snprintf(msg, sizeof(msg), "TRADE %s %ld %ld", instrument_name(instrument),
             quantity, price);

    std::vector<Client *> targets(g_subscribers[instrument].begin(),
                                  g_subscribers[instrument].end());
    for (size_t i = 0; i < targets.size(); i++) {
        if (!targets[i]->remove_me)
            send_line(targets[i], msg);
    }
}


static void submit_order(Client *c, bool is_buy, int instrument, long quantity,
                         long price) {
    int order_id = g_next_order_id++;

    char accepted[64];
    snprintf(accepted, sizeof(accepted), "ORDER_ACCEPTED %d", order_id);
    send_line(c, accepted);

    long remaining = quantity;

    size_t i = 0;
    while (i < g_book.size() && remaining > 0) {
        Order &o = g_book[i];
        if (o.instrument != instrument || o.is_buy == is_buy ||
            o.price != price) {
            i++;
            continue;
        }

        long traded = (remaining < o.quantity) ? remaining : o.quantity;
        remaining -= traded;
        o.quantity -= traded;
        int other_owner = o.owner_id;
        bool filled = (o.quantity == 0);

        char bought[128], sold[128];
        snprintf(bought, sizeof(bought), "BOUGHT %s %ld %ld",
                 instrument_name(instrument), traded, price);
        snprintf(sold, sizeof(sold), "SOLD %s %ld %ld",
                 instrument_name(instrument), traded, price);

        Client *other = find_client_by_id(other_owner);
        if (is_buy) {
            send_line(c, bought);
            if (other != NULL)
                send_line(other, sold);
        } else {
            send_line(c, sold);
            if (other != NULL)
                send_line(other, bought);
        }

        logf("trade: %s %ld @ %ld", instrument_name(instrument), traded, price);
        broadcast_trade(instrument, traded, price);

        if (filled)
            g_book.erase(g_book.begin() + i);
        else
            i++;
    }

    if (remaining > 0) {
        Order o;
        o.id = order_id;
        o.is_buy = is_buy;
        o.instrument = instrument;
        o.price = price;
        o.quantity = remaining;
        o.owner_id = c->id;
        g_book.push_back(o);
    }
}

static void cancel_order(Client *c, long order_id) {
    for (size_t i = 0; i < g_book.size(); i++) {
        if (g_book[i].id != (int)order_id)
            continue;
        if (g_book[i].owner_id != c->id) {
            send_line(c, "ERROR not_your_order");
            return;
        }
        g_book.erase(g_book.begin() + i);
        char msg[64];
        snprintf(msg, sizeof(msg), "ORDER_CANCELLED %ld", order_id);
        send_line(c, msg);
        return;
    }

    if (order_id < g_next_order_id)
        send_line(c, "ERROR order_not_outstanding");
    else
        send_line(c, "ERROR unknown_order");
}


static void handle_message(Client *c, const std::string &line) {
    std::vector<std::string> w = split_words(line);
    if (w.empty()) {
        send_line(c, "ERROR empty_message");
        return;
    }
    const std::string &cmd = w[0];

    if (cmd == "LOGIN") {
        if (c->role == ROLE_MARKET_DATA) {
            send_line(c, "ERROR not_allowed_for_market_data_client");
            return;
        }
        if (w.size() != 2) {
            send_line(c, "ERROR bad_login");
            return;
        }
        if (c->role == ROLE_TRADER) {
            send_line(c, "ERROR already_logged_in");
            return;
        }
        if (g_usernames.count(w[1]) > 0) {
            send_line(c, "ERROR username_in_use");
            return;
        }
        c->role = ROLE_TRADER;
        c->username = w[1];
        g_usernames.insert(w[1]);
        logf("client %d is a trader, username '%s'", c->id, w[1].c_str());
        send_line(c, "OK");
        return;
    }

    if (cmd == "BUY" || cmd == "SELL") {
        if (c->role == ROLE_MARKET_DATA) {
            send_line(c, "ERROR not_allowed_for_market_data_client");
            return;
        }
        if (c->role != ROLE_TRADER) {
            send_line(c, "ERROR not_logged_in");
            return;
        }
        if (w.size() != 4) {
            send_line(c, "ERROR bad_order");
            return;
        }
        int instrument = parse_instrument(w[1]);
        if (instrument < 0) {
            send_line(c, "ERROR unknown_instrument");
            return;
        }
        long quantity = 0, price = 0;
        if (!parse_int(w[2], VALUE_MIN, VALUE_MAX, quantity)) {
            send_line(c, "ERROR invalid_quantity");
            return;
        }
        if (!parse_int(w[3], VALUE_MIN, VALUE_MAX, price)) {
            send_line(c, "ERROR invalid_price");
            return;
        }
        submit_order(c, cmd == "BUY", instrument, quantity, price);
        return;
    }

    if (cmd == "CANCEL") {
        if (c->role == ROLE_MARKET_DATA) {
            send_line(c, "ERROR not_allowed_for_market_data_client");
            return;
        }
        if (c->role != ROLE_TRADER) {
            send_line(c, "ERROR not_logged_in");
            return;
        }
        if (w.size() != 2) {
            send_line(c, "ERROR bad_cancel");
            return;
        }
        long order_id = 0;
        if (!parse_int(w[1], ORDER_ID_MIN, ORDER_ID_MAX, order_id)) {
            send_line(c, "ERROR invalid_order_id");
            return;
        }
        cancel_order(c, order_id);
        return;
    }

    if (cmd == "SUBSCRIBE" || cmd == "UNSUBSCRIBE") {
        if (c->role == ROLE_TRADER) {
            send_line(c, "ERROR not_allowed_for_trader_client");
            return;
        }
        if (w.size() != 2) {
            send_line(c, "ERROR bad_subscription");
            return;
        }
        int instrument = parse_instrument(w[1]);
        if (instrument < 0) {
            send_line(c, "ERROR unknown_instrument");
            return;
        }
        if (cmd == "SUBSCRIBE") {
            if (c->role == ROLE_UNKNOWN) {
                c->role = ROLE_MARKET_DATA;
                logf("client %d is a market-data client", c->id);
            }
            c->subscribed[instrument] = true;
            g_subscribers[instrument].insert(c);
            send_line(c, "OK");
        } else {
            if (!c->subscribed[instrument]) {
                send_line(c, "ERROR not_subscribed");
                return;
            }
            c->subscribed[instrument] = false;
            g_subscribers[instrument].erase(c);
            send_line(c, "OK");
        }
        return;
    }

    if (cmd == "QUIT") {
        logf("client %d sent QUIT, closing its connection", c->id);
        flush_output(c);
        shutdown(c->fd, SHUT_WR);
        c->remove_me = true;
        return;
    }

    send_line(c, "ERROR unknown_command");
}


static void handle_readable(Client *c) {
    char buf[4096];
    ssize_t n = recv(c->fd, buf, sizeof(buf), 0);

    if (n == 0) {
        logf("client %d: recv() returned 0, the client closed its end (FIN)",
             c->id);
        c->remove_me = true;
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        if (errno == ECONNRESET) {
            logf("client %d: recv() failed with ECONNRESET, the connection was "
                 "reset by the peer (RST)",
                 c->id);
        } else {
            logf("client %d: recv() failed: %s", c->id, strerror(errno));
        }
        c->remove_me = true;
        return;
    }

    logf("client %d: recv() returned %zd bytes: \"%s\"", c->id, n,
         escape(std::string(buf, (size_t)n)).c_str());

    c->inbuf.append(buf, (size_t)n);

    std::string line;
    int complete = 0;
    while (!c->remove_me && extract_line(c->inbuf, line)) {
        complete++;
        logf("client %d: complete message: \"%s\"", c->id, line.c_str());
        handle_message(c, line);
    }

    if (complete == 0)
        logf("client %d: no complete message yet, %zu byte(s) held in the "
             "buffer waiting for a newline",
             c->id, c->inbuf.size());

    if (!c->remove_me && c->inbuf.size() > MAX_LINE) {
        send_line(c, "ERROR message_too_long");
        c->remove_me = true;
    }
}


static void accept_new_clients(int listen_fd) {
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno == EMFILE || errno == ENFILE) {
                logf("accept() failed: %s (%zu clients connected). Raise the "
                     "descriptor limit with --nofile.",
                     strerror(errno), g_live_clients);
                usleep(100000);
                return;
            }
            logf("accept() failed: %s", strerror(errno));
            return;
        }

        set_nonblocking(fd);

        Client *c = new Client();
        c->fd = fd;
        c->id = g_next_client_id++;
        c->role = ROLE_UNKNOWN;
        c->watching_write = false;
        c->send_blocked = false;
        c->remove_me = false;
        for (int i = 0; i < NUM_INSTRUMENTS; i++)
            c->subscribed[i] = false;

        g_clients[c->id] = c;
        g_live_clients++;

        kq_set(fd, EVFILT_READ, EV_ADD, c);

        logf("accepted client %d: connected socket fd %d, server side %s, "
             "client side %s",
             c->id, fd, local_addr(fd).c_str(), peer_addr(fd).c_str());
    }
}

static void remove_finished_clients() {
    for (std::map<int, Client *>::iterator it = g_clients.begin();
         it != g_clients.end();) {
        Client *c = it->second;
        if (!c->remove_me) {
            ++it;
            continue;
        }
        g_clients.erase(it++);
        g_dead.push_back(c);
    }

    for (size_t i = 0; i < g_dead.size(); i++) {
        Client *c = g_dead[i];
        if (!c->username.empty())
            g_usernames.erase(c->username);
        for (int k = 0; k < NUM_INSTRUMENTS; k++)
            g_subscribers[k].erase(c);
        g_live_clients--;
        logf("closing client %d (fd %d); %zu client(s) still connected", c->id,
             c->fd, g_live_clients);
        close(c->fd);
        delete c;
    }
    g_dead.clear();
}


int main(int argc, char **argv) {
    const char *ip = "127.0.0.1";
    int port = 5000;
    int num_ports = 1;
    long nofile = 0;
    int positional = 0;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-q" || a == "--quiet") {
            g_quiet = true;
        } else if (a == "--ports" && i + 1 < argc) {
            num_ports = atoi(argv[++i]);
        } else if (a == "--nofile" && i + 1 < argc) {
            nofile = atol(argv[++i]);
        } else if (a == "-h" || a == "--help") {
            printf("usage: %s [ip] [port] [-q] [--ports n] [--nofile n]\n",
                   argv[0]);
            return 0;
        } else if (positional == 0) {
            ip = argv[i];
            positional++;
        } else if (positional == 1) {
            port = atoi(argv[i]);
            positional++;
        } else {
            fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            return 1;
        }
    }
    if (num_ports < 1)
        num_ports = 1;

    signal(SIGPIPE, SIG_IGN);

    if (nofile > 0)
        logf("file descriptor limit is now %ld", raise_fd_limit(nofile));
    else
        logf("file descriptor limit is %ld", current_fd_limit());

    g_kq = kqueue();
    if (g_kq < 0) {
        perror("kqueue");
        return 1;
    }

    std::vector<int> listen_fds;
    for (int i = 0; i < num_ports; i++) {
        int fd = make_listening_socket(ip, port + i);
        if (fd < 0)
            return 1;
        set_nonblocking(fd);
        kq_set(fd, EVFILT_READ, EV_ADD, NULL);
        listen_fds.push_back(fd);
        logf("exchange server listening on %s (listening socket fd %d)",
             local_addr(fd).c_str(), fd);
    }

    struct kevent events[256];

    for (;;) {
        int n = kevent(g_kq, NULL, 0, events, 256, NULL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("kevent");
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].udata == NULL) {
                accept_new_clients((int)events[i].ident);
                continue;
            }

            Client *c = (Client *)events[i].udata;
            if (c->remove_me)
                continue;

            if (events[i].filter == EVFILT_WRITE)
                flush_output(c);
            else if (events[i].filter == EVFILT_READ)
                handle_readable(c);
        }

        remove_finished_clients();
    }

    for (std::map<int, Client *>::iterator it = g_clients.begin();
         it != g_clients.end(); ++it) {
        close(it->second->fd);
        delete it->second;
    }
    for (size_t i = 0; i < listen_fds.size(); i++)
        close(listen_fds[i]);
    close(g_kq);
    return 0;
}