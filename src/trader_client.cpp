// trader_client.cpp - Trader Client.
//
// Usage:  ./trader_client [ip] [port] [username]
//
// If a username is given, LOGIN <username> is sent automatically. After that,
// type commands one per line:
//
//     BUY JNST 100 238
//     SELL JNST 50 238
//     CANCEL 42
//     QUIT
//
// Replies from the exchange (OK, ERROR, ORDER_ACCEPTED, ORDER_CANCELLED,
// BOUGHT, SOLD) are printed as they arrive. BOUGHT and SOLD can turn up at any
// time, long after the order was submitted, because the connection is
// bidirectional and the server sends them when a matching order appears.

#include "common.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    const char *ip = (argc > 1) ? argv[1] : "127.0.0.1";
    int port = (argc > 2) ? atoi(argv[2]) : 5000;

    std::vector<std::string> startup;
    if (argc > 3)
        startup.push_back(std::string("LOGIN ") + argv[3]);
    else
        fprintf(stderr, "no username given: type LOGIN <username> before "
                        "sending orders\n");

    return run_client(ip, port, startup);
}