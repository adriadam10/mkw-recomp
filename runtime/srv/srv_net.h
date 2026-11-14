// The online server's sockets, the same code on Windows (Winsock) and elsewhere (BSD sockets).
#pragma once
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET   // mstcpip.h of MinGW lacks it
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
typedef SOCKET Sock;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#define poll WSAPoll
#define poll_interrupted() 0
#define sock_would_block() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Sock;
#define SOCK_BAD (-1)
#define sock_close close
#define poll_interrupted() (errno == EINTR)
#define sock_would_block() (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
#endif

static inline int sock_nonblocking(Sock s) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
#else
    int f = fcntl(s, F_GETFL, 0);
    return f >= 0 && fcntl(s, F_SETFL, f | O_NONBLOCK) == 0;
#endif
}
