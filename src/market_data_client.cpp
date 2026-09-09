// market_data_client.cpp - Market-Data Client (read-only).
//
// Usage:  ./market_data_client [ip] [port] [instrument ...]
//
// Every instrument named on the command line is subscribed to automatically.
// After subscribing the client does not have to ask for anything: the server
// pushes a TRADE line whenever a trade in that instrument happens.
//
// SUBSCRIBE, UNSUBSCRIBE and QUIT can also be typed one per line.

#include "common.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    const char *ip = (argc > 1) ? argv[1] : "127.0.0.1";
    int port = (argc > 2) ? atoi(argv[2]) : 5000;

    std::vector<std::string> startup;
    for (int i = 3; i < argc; i++)
        startup.push_back(std::string("SUBSCRIBE ") + argv[i]);

    if (startup.empty())
        fprintf(stderr, "no instrument given: type SUBSCRIBE <instrument> to "
                        "start receiving market data\n");

    return run_client(ip, port, startup);
}