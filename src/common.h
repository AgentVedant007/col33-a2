// common.h - small helpers shared by the server and the two clients.
#ifndef COMMON_H
#define COMMON_H

#include <string>
#include <vector>

// ---- logging (everything goes to stderr) ----------------------------------
// Logging is ON by default because the experiments need to see what the
// server received. Pass -q to turn it off.
extern bool g_quiet;
void logf(const char *fmt, ...);
// Turn raw bytes into something printable, e.g. "LOGIN alice\n" -> "LOGIN alice\\n"
std::string escape(const std::string &s);

// ---- sockets ---------------------------------------------------------------
// socket() + bind() + listen(). Returns the listening fd, or -1 on failure.
int make_listening_socket(const char *ip, int port);
// socket() + connect(). Returns the connected fd, or -1 on failure.
int connect_to_server(const char *ip, int port);
void set_nonblocking(int fd);
// "127.0.0.1:5000" for the local or remote end of a socket
std::string local_addr(int fd);
std::string peer_addr(int fd);

// ---- file descriptor limit -------------------------------------------------
// Each connection costs one descriptor, so the default soft limit is the first
// thing that stops a large number of simultaneous connections.
long current_fd_limit();
long raise_fd_limit(long want); // returns the limit actually granted

// ---- protocol helpers ------------------------------------------------------
// Split a line on spaces.
std::vector<std::string> split_words(const std::string &s);
// Strict integer parse: digits only, must land inside [lo, hi].
bool parse_int(const std::string &s, long lo, long hi, long &out);
// If buf holds a complete '\n'-terminated message, move it into line (without
// the '\n') and remove it from buf. This is what makes the code independent of
// how TCP happened to split the byte stream.
bool extract_line(std::string &buf, std::string &line);

// ---- shared client loop ----------------------------------------------------
// Connects, sends the startup commands, then relays stdin -> socket and
// socket -> stdout until the connection closes.
int run_client(const char *ip, int port, const std::vector<std::string> &startup);

#endif