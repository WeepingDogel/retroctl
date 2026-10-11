/* wsock.h - Winsock2 function-pointer types + resolved-API table.
 *
 * Includes <windows.h> BEFORE this header so that the base types
 * (SOCKET, WSADATA, struct sockaddr_in, struct in_addr, WORD, INADDR_ANY,
 * AF_INET, SOCK_STREAM, INVALID_SOCKET, SOCKET_ERROR, SOL_SOCKET,
 * SO_REUSEADDR, MAKEWORD, ...) are already declared there.  This header
 * only adds the PFN_* function-pointer typedefs and the struct wsock table.
 *
 * Winsock entry points are resolved at runtime via
 *   LoadLibraryA("ws2_32.dll") + GetProcAddress
 * because the target's old Dev-C++ MinGW ships a broken <winsock2.h> and a
 * libws2_32.a whose import symbols use ancient __recv@16 stdcall mangling.
 * GetProcAddress looks up the DLL's real export table (clean names).
 *
 * Strict C89, ASCII only.
 */
#ifndef _WSOCK_H
#define _WSOCK_H

#define WCALL __stdcall

typedef int      (WCALL *PFN_WSAStartup)(WORD, WSADATA *);
typedef int      (WCALL *PFN_WSACleanup)(void);
typedef int      (WCALL *PFN_WSAGetLastError)(void);
typedef SOCKET   (WCALL *PFN_socket)(int, int, int);
typedef int      (WCALL *PFN_setsockopt)(SOCKET, int, int, const char *, int);
typedef int      (WCALL *PFN_bind)(SOCKET, const struct sockaddr *, int);
typedef int      (WCALL *PFN_listen)(SOCKET, int);
typedef SOCKET   (WCALL *PFN_accept)(SOCKET, struct sockaddr *, int *);
typedef int      (WCALL *PFN_recv)(SOCKET, void *, int, int);
typedef int      (WCALL *PFN_send)(SOCKET, const void *, int, int);
typedef int      (WCALL *PFN_closesocket)(SOCKET);
typedef int      (WCALL *PFN_ioctlsocket)(SOCKET, unsigned long, u_long *);
typedef unsigned short (WCALL *PFN_htons)(unsigned short);
typedef unsigned long  (WCALL *PFN_htonl)(unsigned long);
typedef char    *(WCALL *PFN_inet_ntoa)(struct in_addr);

/* the resolved API table, filled at startup */
struct wsock
{
    PFN_WSAStartup      WSAStartup;
    PFN_WSACleanup      WSACleanup;
    PFN_WSAGetLastError WSAGetLastError;
    PFN_socket          socket;
    PFN_setsockopt      setsockopt;
    PFN_bind            bind;
    PFN_listen          listen;
    PFN_accept          accept;
    PFN_recv            recv;
    PFN_send            send;
    PFN_closesocket     closesocket;
    PFN_ioctlsocket     ioctlsocket;
    PFN_htons           htons;
    PFN_htonl           htonl;
    PFN_inet_ntoa       inet_ntoa;
};

#endif /* _WSOCK_H */
