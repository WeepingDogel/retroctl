/* Minimal Win32 stub so the agent's C89 code can be SYNTAX-checked on the
 * host with clang -std=c89.  NOT a real header; just enough surface to
 * type-check our own code.  The real build happens on the box with Dev-C++
 * against real windows.h. */
#ifndef _WIN32_STUB_H
#define _WIN32_STUB_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

typedef unsigned long DWORD;
typedef unsigned short WORD;
typedef unsigned int UINT;
typedef unsigned long ULONG;
typedef unsigned char BYTE;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef int BOOL;
typedef void *HANDLE;
typedef void *HMODULE;
typedef unsigned int u_long;
typedef unsigned int SOCKET;
typedef void *LPVOID;
typedef DWORD *LPDWORD;
typedef HANDLE SC_HANDLE;

#define NULL 0
#define TRUE 1
#define FALSE 0
#define INFINITE 0xFFFFFFFF
#define INVALID_HANDLE_VALUE ((HANDLE)(ULONG)0xFFFFFFFF)
#define INVALID_SOCKET ((SOCKET)0xFFFFFFFF)
#define SOCKET_ERROR (-1)
#define MAX_PATH 260
#define MAX_COMPUTERNAME_LENGTH 32
#define SOMAXCONN 255
#define FIONBIO 0x800466C1

#define AF_INET 2
#define SOCK_STREAM 1
#define INADDR_ANY 0x10000000UL
#define MAKEWORD(a, b) ((WORD)(((a) & 0xff) | (((b) & 0xff) << 8)))
#define SOL_SOCKET 0xffff
#define SO_REUSEADDR 4
#define WSADESCRIPTION_LEN 128
#define WSACATALOG_LEN 128
#define SEEK_SET 0
#define WINAPI

typedef struct in_addr { unsigned long s_addr; } IN_ADDR;
typedef struct sockaddr { short sa_family; char sa_data[14]; } SOCKADDR;
typedef struct sockaddr_in {
    short sin_family;
    unsigned short sin_port;
    IN_ADDR sin_addr;
    char sin_zero[8];
} SOCKADDR_IN;

/* The host's libc may #define htons/htonl (and friends) as macros; that
 * shadows the struct-wsock member names on this host only and breaks the
 * syntax check.  Drop them.  No-op on a real Windows build, where these are
 * Winsock functions and the stub is never used. */
#undef htons
#undef htonl
#undef inet_ntoa

typedef struct WSADATA {
    WORD wVersion, wHighVersion;
    char szDescription[WSADESCRIPTION_LEN + 1];
    char szSystemStatus[WSACATALOG_LEN + 1];
    WORD iMaxSockets, iMaxUdpDg;
    char *lpVendorInfo;
} WSADATA;

typedef struct _SERVICE_STATUS {
    DWORD dwServiceType, dwCurrentState, dwControlsAccepted;
    DWORD dwWin32ExitCode, dwServiceSpecificExitCode, dwCheckPoint, dwWaitHint;
} SERVICE_STATUS;
typedef HANDLE SERVICE_STATUS_HANDLE;
typedef SERVICE_STATUS *LPSERVICE_STATUS;
typedef SERVICE_STATUS_HANDLE *LPSERVICE_STATUS_HANDLE;

#define SERVICE_WIN32_OWN_PROCESS 0x10
#define SERVICE_STOPPED 1
#define SERVICE_START_PENDING 2
#define SERVICE_STOP_PENDING 3
#define SERVICE_RUNNING 4
#define SERVICE_ACCEPT_STOP 1
#define SERVICE_ACCEPT_SHUTDOWN 4096
#define SERVICE_CONTROL_STOP 1
#define SERVICE_CONTROL_SHUTDOWN 5
#define SERVICE_AUTO_START 2
#define SERVICE_ERROR_IGNORE 0
#define SERVICE_ALL_ACCESS 0xF001F
#define SC_MANAGER_ALL_ACCESS 0xF001F
#define DELETE 1
#define QUERY_STATUS 2
#define SERVICE_STOP 4
#define SERVICE_START 4

typedef int (*LPHANDLER_FUNCTION)(DWORD);
typedef int (*LPSERVICE_MAIN_FUNCTIONA)(DWORD, char **);
typedef struct _SERVICE_TABLE_ENTRYA {
    char *lpServiceName;
    LPSERVICE_MAIN_FUNCTIONA lpServiceProc;
} SERVICE_TABLE_ENTRYA;

typedef struct _STARTUPINFOA {
    DWORD cb; char *lpReserved; char *lpTitle; DWORD dwFlags;
} STARTUPINFOA;
#define CREATE_NO_WINDOW 0x08000000

typedef struct _PROCESS_INFORMATION {
    HANDLE hProcess, hThread;
    DWORD dwProcessId, dwThreadId;
} PROCESS_INFORMATION;

typedef struct _WIN32_FIND_DATAA {
    DWORD dwLowDateTime, dwHighDateTime;
    DWORD dwReserved1, dwReserved2;
    DWORD nFileSizeHigh, nFileSizeLow;
    DWORD dwCreateLow, dwCreateHigh, dwAccessLow, dwAccessHigh;
    DWORD dwFileAttributes;
    char cFileName[260];
} WIN32_FIND_DATAA;
#define FILE_ATTRIBUTE_DIRECTORY 16
#define MOVEFILE_REPLACE_EXISTING 1

typedef struct _OSVERSIONINFOA {
    DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion;
} OSVERSIONINFOA;

typedef struct _CRITICAL_SECTION { int dummy; } CRITICAL_SECTION;

#define TH32CS_SNAPPROCESS 2
#define WAIT_TIMEOUT 258
#define WAIT_OBJECT_0 0
#define PROCESS_TERMINATE 0x0001

/* ---- function prototypes used by the agent (stubs, never really called) ---- */
HMODULE LoadLibraryA(const char *);
void *GetProcAddress(HMODULE, const char *);
int FreeLibrary(HMODULE);
DWORD GetModuleFileNameA(HMODULE, char *, int);
int GetTempPathA(int, char *);
int GetComputerNameA(char *, DWORD *);
int lstrcmpiA(const char *, const char *);
int lstrcmpA(const char *, const char *);
int GetVersionExA(OSVERSIONINFOA *);
DWORD GetTickCount(void);
DWORD GetLastError(void);
HANDLE CreateEventA(void *s, int, int, char *);
int SetEvent(HANDLE);
DWORD WaitForSingleObject(HANDLE, DWORD);
void Sleep(DWORD);
int CreateProcessA(char *, char *, void *, void *, int, DWORD, void *,
                  char *, STARTUPINFOA *, PROCESS_INFORMATION *);
int TerminateProcess(HANDLE, DWORD);
HANDLE OpenProcess(DWORD, int, DWORD);
int GetExitCodeProcess(HANDLE, DWORD *);
HANDLE FindFirstFileA(const char *, WIN32_FIND_DATAA *);
int FindNextFileA(HANDLE, WIN32_FIND_DATAA *);
int FindClose(HANDLE);
int MoveFileExA(const char *, const char *, DWORD);
int DeleteFileA(const char *);
void InitializeCriticalSection(CRITICAL_SECTION *);
void EnterCriticalSection(CRITICAL_SECTION *);
void LeaveCriticalSection(CRITICAL_SECTION *);
int localtime_s(struct tm *, const time_t *);
void CloseHandle(HANDLE);

#endif
