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