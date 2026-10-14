/* ============================================================================
 * retroctl_agent.c - agent side of retroctl.
 *
 * retroctl drives a Windows box from a host machine over a plain TCP port:
 * run commands, move files, list/kill processes, manage the agent's own
 * Windows service.  The agent is the "controlled side" that makes day-to-day
 * control painless (no per-call RemCom service, no output-file locks).
 *
 * Design goals
 *   - Compiles with a 1999-era toolchain: strict C89, old Dev-C++ MinGW.
 *     Only kernel32 is linked; winsock2, advapi32 and the toolhelp32 process
 *     enumeration entry points are resolved at RUNTIME via LoadLibrary.
 *     (The target's old MinGW ships a broken <winsock2.h> and ancient
 *     __recv@16 import mangling - see wsock.h - and its kernel32 import
 *     table lacks Process32First/Next.)
 *   - JSON-lines protocol, one object per line, shared-token handshake.
 *     See docs/protocol.md.
 *   - Runs as a Windows service (auto-start) or as a plain console process.
 *
 * Build on the target box (Dev-C++ / old MinGW):
 *   C:\Dev-C++\bin\gcc.exe -O2 -o retroctl_agent.exe retroctl_agent.c
 *
 * CLI:
 *   retroctl_agent.exe console            foreground (testing)
 *   retroctl_agent.exe service            SCM mode (started by the manager)
 *   retroctl_agent.exe install            create+autostart+start the service
 *   retroctl_agent.exe uninstall          stop+delete the service
 *   retroctl_agent.exe status             print service state, exit 0/2/1
 * Options: --port N  --token T  --ini PATH
 * Config precedence: args > RETROCTL_PORT / RETROCTL_TOKEN env > ini next to
 * the exe > defaults (port 46911, token "changeme").
 * ========================================================================== */
#include <windows.h>
#include "wsock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* some very old MinGW builds of <windows.h> do not export the service
 * control pointer typedef; provide a safe fallback so the advapi32
 * function-pointer signatures below always compile. */
#ifndef LPSERVICE_STATUS
#define LPSERVICE_STATUS SERVICE_STATUS *
#endif
#ifndef LPSERVICE_STATUS_HANDLE
#define LPSERVICE_STATUS_HANDLE SERVICE_STATUS_HANDLE *
#endif
/* Old MinGW headers sometimes omit the service-main function pointer type.
 * Provide a fallback so the dispatcher table below always compiles. */
#ifndef LPSERVICE_MAIN_FUNCTIONA
#define LPSERVICE_MAIN_FUNCTIONA DWORD (WINAPI *)(DWORD, LPSTR *)
#endif

#define AGENT_VER  "1.0.0"
#define DEF_PORT   46911
#define DEF_TOKEN  "changeme"
#define SVC_KEY    "retroctl_agent"
#define SVC_DISPLAY "RetroCtl Agent"
#define MAX_LINE   2000000       /* largest request line we accept */
#define PUT_CHUNK  65536         /* raw bytes per put/get frame */
#define INI_NAME   "retroctl_agent.ini"
#define LOG_NAME   "retroctl_agent.log"

/* ------------------------------- state ---------------------------------- */
static char g_token[129] = DEF_TOKEN;
static int  g_port      = DEF_PORT;
static char g_inipath[MAX_PATH];
static char g_logpath[MAX_PATH];
static char g_exe[MAX_PATH];

static struct wsock W;
static HMODULE hws;
static WSADATA wsd;
static SOCKET g_listen_sock = (SOCKET)~0;
static SOCKET g_cli_sock = (SOCKET)~0;
static char g_cli_ip[16] = "0.0.0.0";
static CRITICAL_SECTION g_send_cs;
static SERVICE_STATUS g_stat;
static SERVICE_STATUS_HANDLE g_hstat = (SERVICE_STATUS_HANDLE)0;
static HANDLE g_stopEvt = (HANDLE)0;

/* forward declarations (keep C89 declaration-before-use happy) */
static void send_line(SOCKET c, const char *line);
static int  net_read_line(SOCKET c, char *buf, int cap);
static void net_close(SOCKET c);
static void serve_loop(void);
static void dispatch_cmd(const char *line, SOCKET c);
static void op_exec(const char *line, SOCKET c, const char *id);
static void op_ps(const char *line, SOCKET c, const char *id);
static void op_kill(const char *line, SOCKET c, const char *id);
static void op_ls(const char *line, SOCKET c, const char *id);
static void op_info(const char *line, SOCKET c, const char *id);
static void op_put(const char *line, SOCKET c, const char *id);
static void op_get(const char *line, SOCKET c, const char *id);

/* ----------------------------- logging ----------------------------------- */
static void log_line(const char *msg)
{
    FILE *f;
    char ts[32], line[512];
    time_t t = time(NULL);
    struct tm *tmptr = localtime(&t);
    if (!g_logpath[0]) return;
    if (tmptr)
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tmptr);
    else
        ts[0] = 0;
    sprintf(line, "%s %s\n", ts, msg);
    f = fopen(g_logpath, "a");
    if (f) { fputs(line, f); fclose(f); }
}

/* --------------------------- string builder ------------------------------ */
typedef struct { char *b; size_t n, cap; } SB;
static void sb_init(SB *s)
{
    s->b = (char *)malloc(4096);
    s->cap = 4096;
    s->n = 0;
    s->b[0] = 0;
}
static void sb_grow(SB *s, size_t need)
{
    while (s->cap < s->n + need + 1) s->cap *= 2;
    s->b = (char *)realloc(s->b, s->cap);
}
static void sb_put(SB *s, const char *str, size_t len)
{
    sb_grow(s, len + 1);
    memcpy(s->b + s->n, str, len);
    s->n += len;
    s->b[s->n] = 0;
}
static void sb_puts(SB *s, const char *str) { sb_put(s, str, strlen(str)); }
static void sb_addf(SB *s, const char *fmt, ...)
{
    va_list ap;
    char tmp[2048];
    va_start(ap, fmt);
    vsprintf(tmp, fmt, ap);
    va_end(ap);
    sb_puts(s, tmp);
}
static void sb_addc(SB *s, char c)
{
    sb_put(s, &c, 1);
}
/* JSON-escape arbitrary bytes.  High bytes are emitted as \u00XX (the byte
 * value) so a line stays valid JSON/UTF-8 regardless of the box's codepage;
 * the CLI JSON-parses the line and reconstructs the original bytes. */
static void sb_json_bytes(SB *s, const unsigned char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
    {
        unsigned char c = src[i];
        switch (c)
        {
            case '"':  sb_put(s, "\\\"", 2); break;
            case '\\': sb_put(s, "\\\\", 2); break;
            case '\n': sb_put(s, "\\n", 2);  break;
            case '\r': sb_put(s, "\\r", 2);  break;
            case '\t': sb_put(s, "\\t", 2);  break;
            case '\b': sb_put(s, "\\b", 2);  break;
            case '\f': sb_put(s, "\\f", 2);  break;
            default:
                if (c < 0x20 || c >= 0x80)
                    sb_addf(s, "\\u%04x", (int)c);
                else
                    sb_put(s, &c, 1);
                break;
        }
    }
}

/* ----------------------------- base64 ------------------------------------ */
static const char B64T[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int b64enc(const unsigned char *in, size_t n, char *out)
{
    size_t i;
    for (i = 0; i < n; i += 3)
    {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned)in[i + 2];
        out[0] = B64T[(v >> 18) & 63];
        out[1] = B64T[(v >> 12) & 63];
        out[2] = (i + 1 < n) ? B64T[(v >> 6) & 63] : '=';
        out[3] = (i + 2 < n) ? B64T[v & 63] : '=';
        out += 4;
    }
    *out = 0;
    return (int)strlen(out);
}

/* --------------------- runtime symbol resolution -------------------------- */
/* Resolve Winsock.  wsock.h declares struct wsock + the PFN_* types; we load
 * ws2_32.dll and fill the table from its real export names. */
static int load_wsock(void)
{
    hws = LoadLibraryA("ws2_32.dll");
    if (!hws) hws = LoadLibraryA("winsock.dll");
    if (!hws) return 0;
    W.WSAStartup      = (PFN_WSAStartup)      GetProcAddress(hws, "WSAStartup");
    W.WSACleanup      = (PFN_WSACleanup)      GetProcAddress(hws, "WSACleanup");
    W.WSAGetLastError = (PFN_WSAGetLastError) GetProcAddress(hws, "WSAGetLastError");
    W.socket          = (PFN_socket)          GetProcAddress(hws, "socket");
    W.setsockopt      = (PFN_setsockopt)     GetProcAddress(hws, "setsockopt");
    W.bind            = (PFN_bind)           GetProcAddress(hws, "bind");
    W.listen          = (PFN_listen)         GetProcAddress(hws, "listen");
    W.accept          = (PFN_accept)         GetProcAddress(hws, "accept");
    W.recv            = (PFN_recv)           GetProcAddress(hws, "recv");
    W.send            = (PFN_send)           GetProcAddress(hws, "send");
    W.closesocket     = (PFN_closesocket)    GetProcAddress(hws, "closesocket");
    W.htons           = (PFN_htons)          GetProcAddress(hws, "htons");
    W.htonl           = (PFN_htonl)          GetProcAddress(hws, "htonl");
    W.inet_ntoa       = (PFN_inet_ntoa)      GetProcAddress(hws, "inet_ntoa");
    return (W.WSAStartup && W.socket && W.bind && W.listen && W.accept &&
            W.recv && W.send && W.closesocket && W.htons && W.htonl &&
            W.inet_ntoa);
}

/* toolhelp32 process enumeration (absent from the old MinGW import table). */
typedef HANDLE (WINAPI *fn_snap)(DWORD, DWORD);
typedef int    (WINAPI *fn_first)(HANDLE, unsigned char *);
typedef int    (WINAPI *fn_next)(HANDLE, unsigned char *);
static HMODULE hK32;
static fn_snap  z_Snap;
static fn_first z_First;
static fn_next  z_Next;
static int load_toolhelp(void)
{
    hK32 = LoadLibraryA("kernel32.dll");
    if (!hK32) return 0;
    z_Snap  = (fn_snap)  GetProcAddress(hK32, "CreateToolhelp32Snapshot");
    z_First = (fn_first) GetProcAddress(hK32, "Process32First");
    z_Next  = (fn_next)  GetProcAddress(hK32, "Process32Next");
    return z_Snap && z_First && z_Next;
}

/* advapi32 service-control-manager APIs (for install / uninstall / status). */
typedef SC_HANDLE (WINAPI *fn_oSC)(LPCSTR, LPCSTR, DWORD);
typedef SC_HANDLE (WINAPI *fn_oSV)(SC_HANDLE, LPCSTR, DWORD);
typedef int       (WINAPI *fn_QS)(SC_HANDLE, LPSERVICE_STATUS);
typedef int       (WINAPI *fn_CS)(SC_HANDLE, DWORD, LPSERVICE_STATUS);
typedef int       (WINAPI *fn_DS)(SC_HANDLE);
typedef SC_HANDLE (WINAPI *fn_CR)(SC_HANDLE, LPCSTR, LPCSTR, DWORD, DWORD,
                                  DWORD, DWORD, LPSTR, LPCSTR, LPDWORD,
                                  LPSTR, LPSTR, LPSTR);
typedef int       (WINAPI *fn_St)(SC_HANDLE, int, LPCSTR);
typedef int       (WINAPI *fn_SS)(SERVICE_STATUS_HANDLE, LPSERVICE_STATUS);
typedef SERVICE_STATUS_HANDLE (WINAPI *fn_RH)(LPCSTR, LPHANDLER_FUNCTION);
typedef int       (WINAPI *fn_SCD)(const SERVICE_TABLE_ENTRY *);
static HMODULE hAdv;
static fn_oSC  z_OpenSCM;
static fn_oSV  z_OpenSvc;
static fn_QS   z_QStat;
static fn_CS   z_CtrlSvc;
static fn_DS   z_DelSvc;
static fn_CR   z_CreateSvc;
static fn_St   z_StartSvc;
static fn_SS   z_SetStat;
static fn_RH   z_RegHandler;
static fn_SCD  z_ScDispatcher;
static int load_advapi(void)
{
    hAdv = LoadLibraryA("advapi32.dll");
    if (!hAdv) return 0;
    z_OpenSCM     = (fn_oSC)  GetProcAddress(hAdv, "OpenSCManagerA");
    z_OpenSvc     = (fn_oSV)  GetProcAddress(hAdv, "OpenServiceA");
    z_QStat       = (fn_QS)   GetProcAddress(hAdv, "QueryServiceStatus");
    z_CtrlSvc     = (fn_CS)   GetProcAddress(hAdv, "ControlService");
    z_DelSvc      = (fn_DS)   GetProcAddress(hAdv, "DeleteService");
    z_CreateSvc   = (fn_CR)   GetProcAddress(hAdv, "CreateServiceA");
    z_StartSvc    = (fn_St)   GetProcAddress(hAdv, "StartServiceA");
    z_SetStat     = (fn_SS)   GetProcAddress(hAdv, "SetServiceStatus");
    z_RegHandler  = (fn_RH)   GetProcAddress(hAdv, "RegisterServiceCtrlHandlerA");
    z_ScDispatcher= (fn_SCD)  GetProcAddress(hAdv, "StartServiceCtrlDispatcherA");
    return z_OpenSCM && z_QStat && z_SetStat && z_RegHandler && z_CreateSvc;
}

/* ----------------------------- config ------------------------------------ */
static void path_to_dir(const char *file, char *dir)
{
    char *slash;
    strncpy(dir, file, MAX_PATH - 1);
    dir[MAX_PATH - 1] = 0;
    slash = strrchr(dir, '\\');
    if (slash) *(slash + 1) = 0;
    else dir[0] = 0;
}
static void paths_from_exe(const char *exe)
{
    char dir[MAX_PATH];
    GetModuleFileNameA((HMODULE)0, g_exe, sizeof(g_exe));
    path_to_dir(g_exe, dir);
    sprintf(g_inipath, "%s%s", dir, INI_NAME);
    sprintf(g_logpath, "%s%s", dir, LOG_NAME);
}
static void load_ini(const char *path)
{
    FILE *f;
    char line[190];
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f))
    {
        char *eq = strchr(line, '=');
        char *key, *val;
        if (!eq) continue;
        key = line;
        while (*key == ' ' || *key == '\t') key++;
        *eq = 0;
        {
            size_t kl = (size_t)(eq - key);
            char *e = eq - 1;
            while (e > key && (e[-1] == ' ' || e[-1] == '\t')) *e-- = 0;
            key[kl > 0 ? 0 : 0] = 0;
        }
        val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        {
            char *e = val + strlen(val);
            while (e > val && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        }
        if (lstrcmpiA(key, "port") == 0)
        {
            int p = atoi(val);
            if (p > 0 && p < 65536) g_port = p;
        }
        else if (lstrcmpiA(key, "token") == 0)
        {
            strncpy(g_token, val, sizeof(g_token) - 1);
            g_token[sizeof(g_token) - 1] = 0;
        }
    }
    fclose(f);
}
static void apply_env(void)
{
    char *e = getenv("RETROCTL_PORT");
    char *t = getenv("RETROCTL_TOKEN");
    if (e && atoi(e) > 0) g_port = atoi(e);
    if (t && *t)
    {
        strncpy(g_token, t, sizeof(g_token) - 1);
        g_token[sizeof(g_token) - 1] = 0;
    }
}
static void parse_args(int argc, char **argv, int *want_install, int *want_uninstall,
                       int *want_status, int *want_service, int *want_console)
{
    int i;
    *want_install = *want_uninstall = *want_status = *want_service = *want_console = 0;
    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "console")) *want_console = 1;
        else if (!strcmp(argv[i], "service")) *want_service = 1;
        else if (!strcmp(argv[i], "install")) *want_install = 1;
        else if (!strcmp(argv[i], "uninstall")) *want_uninstall = 1;
        else if (!strcmp(argv[i], "status")) *want_status = 1;
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--token") && i + 1 < argc)
        {
            strncpy(g_token, argv[++i], sizeof(g_token) - 1);
            g_token[sizeof(g_token) - 1] = 0;
        }
    }
}

/* ------------------------------ network --------------------------------- */
/* The agent serves one client at a time (good enough for a LAN control
 * channel); send_line is serialized with a critical section so an in-flight
 * exec-stream handler can't interleave with a put handler's own sends. */
static void send_line(SOCKET c, const char *line)
{
    EnterCriticalSection(&g_send_cs);
    {
        int len = (int)strlen(line);
        int off = 0;
        while (off < len)
        {
            int r = W.send(c, line + off, len - off, 0);
            if (r <= 0) break;
            off += r;
        }
    }
    LeaveCriticalSection(&g_send_cs);
}

/* Read one \n-terminated line from the socket.  Returns 0 on success, -1 on
 * EOF / error.  Grows buf only up to cap. */
static int net_read_line(SOCKET c, char *buf, int cap)
{
    int n = 0;
    for (;;)
    {
        int r;
        char ch;
        if (n + 1 >= cap) return -1;      /* overflow guard */
        r = W.recv(c, &ch, 1, 0);
        if (r <= 0) return n ? 0 : -1;    /* EOF */
        if (ch == '\n')
        {
            buf[n] = 0;
            return 0;
        }
        if (ch != '\r')
            buf[n++] = ch;
    }
}

static void net_close(SOCKET c)
{
    if (c != (SOCKET)~0)
    {
        W.closesocket(c);
        g_cli_sock = (SOCKET)~0;
    }
}

/* --------------------------- command dispatch ---------------------------- */
/* Every request line is a flat JSON object with an "op" and an "id".  We
 * answer with frames that carry the same id; exec is special: it streams
 * "out" frames (b64) then a terminal "done" frame. */
static void reply_ok(const char *id, const char *extra_json)
{
    SB m;
    sb_init(&m);
    sb_addf(&m, "{\"id\":\"%s\",\"ok\":true", id);
    if (extra_json && *extra_json) sb_puts(&m, extra_json);
    sb_puts(&m, "}\n");
    send_line(g_cli_sock, m.b);
    free(m.b);
}
static void reply_err(const char *id, const char *why)
{
    SB m;
    sb_init(&m);
    sb_addf(&m, "{\"id\":\"%s\",\"ok\":false,\"err\":\"", id);
    sb_json_bytes(&m, (const unsigned char *)why, strlen(why));
    sb_puts(&m, "\"}\n");
    send_line(g_cli_sock, m.b);
    free(m.b);
}

/* Minimal flat-object JSON field reader.  We only need to pull named fields
 * out of single-line machine-generated objects, so a hand scanner is safe.
 * Strings are JSON-decoded into a returned malloc'd buffer (caller frees). */
typedef struct { int found; int is_num; long num; char *str; } JF;
static const char *jf_find(const char *line, const char *key, JF *out)
{
    char pat[48];
    const char *p;
    size_t kl;
    out->found = 0;
    kl = strlen(key);
    if (kl >= sizeof(pat) - 4) return 0;
    pat[0] = '"';
    memcpy(pat + 1, key, kl);
    pat[1 + kl] = '"';
    pat[2 + kl] = 0;
    p = strstr(line, pat);
    if (!p) return 0;
    p += 2 + kl;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    out->found = 1;
    out->is_num = 0;
    out->num = 0;
    out->str = 0;
    return p;
}
static int jf_get_str(const char *line, const char *key, char **out)
{
    JF f;
    const char *v = jf_find(line, key, &f);
    char *dec = 0;
    *out = 0;
    if (!v || *v != '"') return 0;
    v++;
    /* decode JSON string */
    {
        SB s;
        sb_init(&s);
        while (*v && *v != '"')
        {
            unsigned char c = (unsigned char)*v;
            if (c == '\\')
            {
                v++;
                c = (unsigned char)*v;
                switch (c)
                {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case '"': c = '"';  break;
                    case '\\': c = '\\'; break;
                    case '/': c = '/';  break;
                    case 'u':
                    {
                        unsigned vval = 0;
                        int d;
                        v++;
                        for (d = 0; d < 4 && *v; d++)
                        {
                            char h = *v++;
                            vval <<= 4;
                            if (h >= '0' && h <= '9') vval |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') vval |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') vval |= (unsigned)(h - 'A' + 10);
                        }
                        sb_addc(&s, (char)(vval & 0xFF));
                        continue;
                    }
                    default: break;
                }
                sb_addc(&s, (char)c);
                v++;
                continue;
            }
            sb_addc(&s, (char)c);
            v++;
        }
        dec = s.b;
    }
    *out = dec;
    return 1;
}
static long jf_get_num(const char *line, const char *key, long dflt)
{
    JF f;
    const char *v = jf_find(line, key, &f);
    if (!v) return dflt;
    return atol(v);
}
static int jf_get_bool(const char *line, const char *key)
{
    JF f;
    const char *v = jf_find(line, key, &f);
    if (!v) return 0;
    return (!strncmp(v, "true", 4)) ? 1 : 0;
}

/* --------------------------- command handlers ---------------------------- */
/* forward decls (C89 requires these before use) */
static int  GetWindowsVersion(void);
static size_t b64dec_len(const char *in, unsigned char *out);
static void op_put_begin(const char *line, SOCKET c, const char *id);
static void op_put_chunk(const char *line, SOCKET c, const char *id);
static void op_put_end(const char *line, SOCKET c, const char *id);

/* exec: run `cmd` under cmd /d /c, capture stdout+stderr to a unique temp
 * file (no shared o.txt lock), stream it back in base64 "out" frames, then a
 * terminal "done" frame carrying the exit code.  cwd/timeout optional. */
#define EXEC_CHUNK 32768
static void op_exec(const char *line, SOCKET c, const char *id)
{
    char *cmd = 0, *cwd = 0;
    long timeout_ms;
    char tmppath[MAX_PATH];
    char fullcmd[8192];
    char tmpbase[MAX_PATH];
    char b64buf[EXEC_CHUNK * 4 + 8];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD exitcode = 0;
    int started = 0;
    FILE *f;
    long waitres;
    int timed_out = 0;
    size_t raw;
    unsigned char rb[EXEC_CHUNK];
    SB m;

    jf_get_str(line, "cmd", &cmd);
    jf_get_str(line, "cwd", &cwd);
    timeout_ms = jf_get_num(line, "timeout", 60000);
    if (!cmd) cmd = (char *)"";

    GetTempPathA(sizeof(tmpbase), tmpbase);
    sprintf(tmppath, "%srctl_%ld.tmp", tmpbase, (long)GetTickCount());

    /* cmd /d /c  usercmd  > "tmppath"  2>&1   (cmd interprets the redirect) */
    if (strlen(cmd) > 60000)
    {
        reply_err(id, "command too long");
        if (cmd) free(cmd);
        if (cwd) free(cwd);
        return;
    }
    sprintf(fullcmd, "cmd /d /c %s > \"%s\" 2>&1", cmd, tmppath);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    started = CreateProcessA(NULL, fullcmd, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW, NULL,
                             (cwd && *cwd) ? cwd : NULL, &si, &pi);
    if (!started)
    {
        SB e;
        sb_init(&e);
        sb_addf(&e, "{\"id\":\"%s\",\"ok\":false,\"err\":\"spawn failed: ", id);
        sb_addf(&e, "%lu\"}\n", (unsigned long)GetLastError());
        send_line(c, e.b);
        free(e.b);
        if (cmd) free(cmd);
        if (cwd) free(cwd);
        return;
    }
    CloseHandle(pi.hThread);

    waitres = WaitForSingleObject(pi.hProcess, (DWORD)timeout_ms);
    if (waitres == WAIT_TIMEOUT)
    {
        timed_out = 1;
        TerminateProcess(pi.hProcess, (DWORD)-1);
    }
    GetExitCodeProcess(pi.hProcess, &exitcode);
    CloseHandle(pi.hProcess);

    /* stream the captured output */
    f = fopen(tmppath, "rb");
    m.b = 0;
    while (f && (raw = fread(rb, 1, EXEC_CHUNK, f)) > 0)
    {
        int blen = b64enc(rb, raw, b64buf);
        (void)blen;
        sb_init(&m);
        sb_addf(&m, "{\"id\":\"%s\",\"type\":\"out\",\"b64\":\"%s\"}\n", id, b64buf);
        send_line(c, m.b);
        free(m.b);
        m.b = 0;
    }
    if (f) fclose(f);

    /* terminal frame */
    sb_init(&m);
    sb_addf(&m, "{\"id\":\"%s\",\"type\":\"done\",\"code\":%ld", id,
            (long)(timed_out ? -1 : exitcode));
    if (timed_out) sb_puts(&m, ",\"timed_out\":true");
    sb_puts(&m, "}\n");
    send_line(c, m.b);
    free(m.b);

    DeleteFileA(tmppath);
    if (cmd) free(cmd);
    if (cwd) free(cwd);
}

/* ps: walk the toolhelp32 process snapshot.  On Win2000 the ANSI
 * PROCESSENTRY32 is 512 bytes wide and the image name lives at byte +36
 * (validated on this box).  We emit a JSON array under "procs". */
#define TH32_SNAPPROCESS 0x00000002
#define TH32_DWSIZE 512
#define TH32_PID_OFF 8
#define TH32_NAME_OFF 36
#define TH32_PPID_OFF 20
static void op_ps(const char *line, SOCKET c, const char *id)
{
    HANDLE h;
    unsigned char buf[TH32_DWSIZE];
    char name[260];
    int firstok;
    SB m;
    int started = 0;
    int count = 0;
    char ppid_raw[16];
    long ppid = 0;
    long pid = 0;

    if (!load_toolhelp())
    {
        reply_err(id, "toolhelp32 unavailable");
        return;
    }
    h = z_Snap(TH32_SNAPPROCESS, 0);
    if (h == (HANDLE)~0)
    {
        reply_err(id, "snapshot failed");
        return;
    }
    memset(buf, 0, sizeof(buf));
    *(DWORD *)(buf + 0) = TH32_DWSIZE;
    firstok = z_First(h, buf);
    sb_init(&m);
    sb_addf(&m, "{\"id\":\"%s\",\"ok\":true,\"procs\":[", id);
    if (firstok)
    {
        for (;;)
        {
            pid = (long)*(DWORD *)(buf + TH32_PID_OFF);
            ppid = (long)*(DWORD *)(buf + TH32_PPID_OFF);
            name[0] = 0;
            {
                int i = 0;
                unsigned char *ns = buf + TH32_NAME_OFF;
                while (i < 256 && ns[i])
                {
                    name[i] = (char)ns[i];
                    i++;
                }
                name[i] = 0;
            }
            if (started) sb_puts(&m, ",");
            started = 1;
            sb_addf(&m, "{\"pid\":%ld,\"ppid\":%ld,\"name\":\"", pid, ppid);
            sb_json_bytes(&m, (const unsigned char *)name, strlen(name));
            sb_addf(&m, "\"}");
            count++;
            if (!z_Next(h, buf)) break;
            if (count > 4000) break;   /* runaway guard */
        }
    }
    CloseHandle(h);
    sb_addf(&m, "],\"count\":%d}\n", count);
    send_line(c, m.b);
    free(m.b);
    (void)ppid_raw;
}

/* kill: terminate the process with a given pid (or, if name given, every
 * matching process name).  OpenProcess + TerminateProcess, kernel32 only. */
static void op_kill(const char *line, SOCKET c, const char *id)
{
    char *name = 0;
    long pid = jf_get_num(line, "pid", -1);
    int killed = 0;
    int matched = 0;
    int want_all = jf_get_bool(line, "all");

    jf_get_str(line, "name", &name);

    if (pid > 0 && !want_all)
    {
        HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
        if (p)
        {
            if (TerminateProcess(p, (DWORD)0xFFFE)) killed = 1;
            else reply_err(id, "terminate failed");
            CloseHandle(p);
        }
        else
        {
            reply_err(id, "open failed");
            if (name) free(name);
            return;
        }
        reply_ok(id, ",\"killed\":true");
        if (name) free(name);
        return;
    }

    /* kill by name (and optionally the whole family) */
    if (!name)
    {
        reply_err(id, "need pid or name");
        return;
    }
    if (!load_toolhelp())
    {
        reply_err(id, "toolhelp32 unavailable");
        free(name);
        return;
    }
    {
        HANDLE h = z_Snap(TH32_SNAPPROCESS, 0);
        unsigned char buf[TH32_DWSIZE];
        int firstok;
        if (h == (HANDLE)~0)
        {
            reply_err(id, "snapshot failed");
            free(name);
            return;
        }
        memset(buf, 0, sizeof(buf));
        *(DWORD *)(buf + 0) = TH32_DWSIZE;
        firstok = z_First(h, buf);
        if (firstok)
        {
            int nvisited = 0;
            for (;;)
            {
                char nm[260];
                long p;
                int i = 0;
                unsigned char *ns = buf + TH32_NAME_OFF;
                while (i < 256 && ns[i]) { nm[i] = (char)ns[i]; i++; }
                nm[i] = 0;
                p = (long)*(DWORD *)(buf + TH32_PID_OFF);
                if (lstrcmpiA(nm, name) == 0 && p > 4)
                {
                    HANDLE pp = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)p);
                    matched++;
                    if (pp)
                    {
                        if (TerminateProcess(pp, (DWORD)0xFFFE)) killed++;
                        CloseHandle(pp);
                    }
                }
                nvisited++;
                if (!z_Next(h, buf)) break;
                if (nvisited >= 4000) break;
            }
        }
        CloseHandle(h);
    }
    {
        SB m;
        sb_init(&m);
        sb_addf(&m, "{\"id\":\"%s\",\"ok\":true,\"matched\":%d,\"killed\":%d}\n",
                id, matched, killed);
        send_line(c, m.b);
        free(m.b);
    }
    free(name);
}

/* info: report the box's OS, hostname, uptime-ish and the agent's own view. */
static void op_info(const char *line, SOCKET c, const char *id)
{
    SB m;
    char comp[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD clen = sizeof(comp);
    int b;
    char osver[128];

    comp[0] = 0;
    GetComputerNameA(comp, &clen);
    b = GetWindowsVersion();
    switch (b)
    {
        case 0:  strcpy(osver, "Windows 2000/NT4"); break;
        case 1:  strcpy(osver, "Windows XP/2003");   break;
        case 2:  strcpy(osver, "Windows Vista/7/2008/8/8.1/10/11"); break;
        default: strcpy(osver, "unknown");            break;
    }
    sb_init(&m);
    sb_addf(&m, "{\"id\":\"%s\",\"ok\":true,\"agent_ver\":\"" AGENT_VER
            "\",\"hostname\":\"%s\",\"os_family\":\"%s\"}\n",
            id, comp, osver);
    send_line(c, m.b);
    free(m.b);
}

/* Return a GetVersionEx-free OS family guess.  Returns 0/1/2 for
 * (pre-XP / XP-era / Vista-and-newer).  On Win2000 we expect 0. */
static int GetWindowsVersion(void)
{
    OSVERSIONINFOA v;
    v.dwOSVersionInfoSize = sizeof(v);
    if (GetVersionExA(&v))
    {
        if (v.dwMajorVersion < 5) return 0;          /* NT4 */
        if (v.dwMajorVersion == 5) return 0;         /* 2000 */
        if (v.dwMajorVersion == 6 && v.dwMinorVersion < 1) return 1; /* XP/2003 */
        if (v.dwMajorVersion == 6) return 2;         /* Vista/2008 */
        if (v.dwMajorVersion == 10) return 2;        /* 8/8.1/10/11 */
    }
    return -1;
}

/* ls: list a directory.  Emit objects {name,dir,size} under "entries". */
static void op_ls(const char *line, SOCKET c, const char *id)
{
    char *path = 0;
    char pattern[MAX_PATH + 16];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    SB m;
    BOOL first = TRUE;

    jf_get_str(line, "path", &path);
    if (!path || !*path) path = (char *)"";
    if (strlen(path) > MAX_PATH - 2)
    {
        reply_err(id, "path too long");
        free(path);
        return;
    }
    sprintf(pattern, "%s\\*", path);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        reply_err(id, "cannot open directory");
        free(path);
        return;
    }
    sb_init(&m);
    sb_addf(&m, "{\"id\":\"%s\",\"ok\":true,\"entries\":[", id);
    for (;;)
    {
        char *bn = strrchr(fd.cFileName, '\\');
        if (bn) bn++; else bn = fd.cFileName;
        if (!strcmp(bn, ".") || !strcmp(bn, ".."))
        {
            if (FindNextFileA(h, &fd)) continue;
            if (!FindNextFileA(h, &fd)) break;
            continue;
        }
        if (!first) sb_puts(&m, ",");
        first = FALSE;
        sb_addf(&m, "{\"name\":\"");
        sb_json_bytes(&m, (const unsigned char *)bn, strlen(bn));
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            sb_puts(&m, "\",\"dir\":true}");
        else
            sb_addf(&m, "\",\"dir\":false,\"size\":%lu}",
                    (unsigned long)fd.nFileSizeHigh * 4294967296UL +
                    (unsigned long)fd.nFileSizeLow);
        if (!FindNextFileA(h, &fd)) break;
    }
    FindClose(h);
    sb_addf(&m, "],\"path\":\"");
    sb_json_bytes(&m, (const unsigned char *)path, strlen(path));
    sb_addf(&m, "\"}\n");
    send_line(c, m.b);
    free(m.b);
    free(path);
}

/* put: the CLI sends "put" with a target path, then a series of
 * "puchunk" frames (b64 body) and finally a "putend".  We buffer to a temp
 * file and move it into place when done. */
#define PUT_TMP_PREFIX "rctl_put_"
static void op_put_begin(const char *line, SOCKET c, const char *id)
{
    char *path = 0;
    char tmpp[MAX_PATH + 16];
    FILE *f;
    jf_get_str(line, "path", &path);
    if (!path) path = (char *)"";
    if (strlen(path) > MAX_PATH - 40)
    {
        reply_err(id, "path too long");
        free(path);
        return;
    }
    sprintf(tmpp, "%s~rctl.tmp", path);
    f = fopen(tmpp, "wb");
    if (!f)
    {
        reply_err(id, "cannot open target for write");
        free(path);
        return;
    }
    /* Remember the staging path for the CLI by echoing it back so putend
     * (which re-sends the target path) lines up.  We keep it deterministic:
     * "<path>~rctl.tmp". */
    fclose(f);
    {
        SB m;
        sb_init(&m);
        sb_addf(&m, "{\"id\":\"%s\",\"ok\":true,\"staging\":\"", id);
        sb_json_bytes(&m, (const unsigned char *)tmpp, strlen(tmpp));
        sb_addf(&m, "\"}\n");
        send_line(c, m.b);
        free(m.b);
    }
    free(path);
}

/* puchunk: append decoded b64 payload to the staging file. */
static void op_put_chunk(const char *line, SOCKET c, const char *id)
{
    char *b64 = 0;
    unsigned char raw[PUT_CHUNK * 4];
    size_t n;
    char *path = 0;
    char tmpp[MAX_PATH + 16];
    FILE *f;

    jf_get_str(line, "data", &b64);
    jf_get_str(line, "path", &path);
    if (!path) path = (char *)"";
    sprintf(tmpp, "%s~rctl.tmp", path);
    f = fopen(tmpp, "ab");
    if (!f)
    {
        reply_err(id, "staging file missing");
        free(b64); free(path);
        return;
    }
    n = b64dec_len(b64, raw);
    if (n > 0) fwrite(raw, 1, n, f);
    fclose(f);
    {
        SB m;
        sb_init(&m);
        sb_addf(&m, "{\"id\":\"%s\",\"ok\":true,\"written\":%u}\n",
                id, (unsigned)n);
        send_line(c, m.b);
        free(m.b);
    }
    free(b64);
    free(path);
}

/* putend: rename the staging file over the real path (atomic-ish). */
static void op_put_end(const char *line, SOCKET c, const char *id)
{
    char *path = 0;
    char tmpp[MAX_PATH + 16];
    jf_get_str(line, "path", &path);
    if (!path) path = (char *)"";
    sprintf(tmpp, "%s~rctl.tmp", path);
    if (MoveFileExA(tmpp, path, MOVEFILE_REPLACE_EXISTING))
    {
        reply_ok(id, ",\"moved\":true");
    }
    else
    {
        reply_err(id, "move failed");
    }
    free(path);
}

/* b64 decode with a known-length output buffer. */
static size_t b64dec_len(const char *in, unsigned char *out)
{
    static int init;
    static int vals[256];
    size_t n = strlen(in), i = 0, o = 0;
    int q[4], qn = 0;
    if (!init)
    {
        int d;
        memset(vals, 0, sizeof(vals));
        for (d = 0; d < 256; d++) vals[d] = -1;
        for (d = 0; d < 26; d++) vals['A' + d] = d;
        for (d = 0; d < 26; d++) vals['a' + d] = d + 26;
        for (d = 0; d < 10; d++) vals['0' + d] = d + 52;
        vals['+'] = 62;
        vals['/'] = 63;
        init = 1;
    }
    while (i < n)
    {
        int c = vals[(unsigned char)in[i++]];
        if (c < 0) continue;
        q[qn++] = c;
        if (qn == 4)
        {
            out[o++] = (unsigned char)((q[0] << 2) | (q[1] >> 4));
            out[o++] = (unsigned char)((q[1] << 4) | (q[2] >> 2));
            out[o++] = (unsigned char)((q[2] << 6) | q[3]);
            qn = 0;
        }
    }
    if (qn == 2)
        out[o++] = (unsigned char)((q[0] << 2) | (q[1] >> 4));
    else if (qn == 3)
    {
        out[o++] = (unsigned char)((q[0] << 2) | (q[1] >> 4));
        out[o++] = (unsigned char)((q[1] << 4) | (q[2] >> 2));
    }
    return o;
}

/* get: send a file back to the CLI in b64 chunks, ending with "getdone". */
static void op_get(const char *line, SOCKET c, const char *id)
{
    char *path = 0;
    FILE *f;
    unsigned char raw[PUT_CHUNK];
    char b64buf[PUT_CHUNK * 4 + 8];
    size_t n;

    jf_get_str(line, "path", &path);
    if (!path) path = (char *)"";
    f = fopen(path, "rb");
    if (!f)
    {
        reply_err(id, "cannot open file");
        free(path);
        return;
    }
    while ((n = fread(raw, 1, PUT_CHUNK, f)) > 0)
    {
        int blen = b64enc(raw, n, b64buf);
        SB m;
        (void)blen;
        sb_init(&m);
        sb_addf(&m, "{\"id\":\"%s\",\"type\":\"getchunk\",\"b64\":\"%s\"}\n",
                id, b64buf);
        send_line(c, m.b);
        free(m.b);
    }
    {
        SB m;
        sb_init(&m);
        sb_addf(&m, "{\"id\":\"%s\",\"type\":\"getdone\"}\n", id);
        send_line(c, m.b);
        free(m.b);
    }
    fclose(f);
    free(path);
}

/* --------------------------- dispatch ------------------------------------ */
static void dispatch_cmd(const char *line, SOCKET c)
{
    char *op = 0, *id = 0;
    int have_op, have_id;

    have_op = jf_get_str(line, "op", &op);
    have_id = jf_get_str(line, "id", &id);
    if (!have_id) id = 0;
    if (!have_op || !op || !*op)
    {
        if (op) free(op);
        if (id) free(id);
        reply_err("(none)", "missing op");
        return;
    }
    if (!id) id = (char *)"0";

    if      (!strcmp(op, "exec"))      op_exec(line, c, id);
    else if (!strcmp(op, "ps"))        op_ps(line, c, id);
    else if (!strcmp(op, "kill"))      op_kill(line, c, id);
    else if (!strcmp(op, "ls"))        op_ls(line, c, id);
    else if (!strcmp(op, "info"))      op_info(line, c, id);
    else if (!strcmp(op, "put"))       op_put_begin(line, c, id);
    else if (!strcmp(op, "putchunk"))  op_put_chunk(line, c, id);
    else if (!strcmp(op, "putend"))    op_put_end(line, c, id);
    else if (!strcmp(op, "get"))       op_get(line, c, id);
    else
        reply_err(id, "unknown op");

    if (have_op) free(op);
    if (have_id) free(id);
}

/* --------------------------- connection loop ----------------------------- */
/* One client at a time.  A client MUST open with a "hello" carrying the
 * shared token; a wrong token is answered with err "bad token" and dropped.
 * After hello, the connection stays open and each line is one command. */
static void handle_client(SOCKET c)
{
    char *line = (char *)malloc(MAX_LINE);
    for (;;)
    {
        int rc;
        char *opstr = 0;
        int is_hello;

        rc = net_read_line(c, line, MAX_LINE);
        if (rc != 0) break;
        if (!line[0]) continue;

        is_hello = jf_get_str(line, "op", &opstr) && opstr && !strcmp(opstr, "hello");
        if (is_hello)
        {
            char *tok = 0;
            int ok = 0;
            jf_get_str(line, "token", &tok);
            if (tok && *tok && !strcmp(tok, g_token)) ok = 1;
            if (ok)
            {
                SB m;
                sb_init(&m);
                sb_addf(&m, "{\"id\":\"0\",\"ok\":true,\"agent\":\"" AGENT_VER "\"}\n");
                send_line(c, m.b);
                free(m.b);
            }
            else
            {
                reply_err("0", "bad token");
                free(line);
                net_close(c);
                return;
            }
            if (tok) free(tok);
            free(opstr);
            continue;
        }
        if (opstr) free(opstr);
        dispatch_cmd(line, c);
        if (g_cli_sock == (SOCKET)~0) break;
    }
    free(line);
    net_close(c);
}

/* Open the listening socket, then serve.  In service mode we poll the stop
 * event so a stop request tears the service down cleanly. */
static int open_listener(void)
{
    struct sockaddr_in a;
    int optv = 1;

    g_listen_sock = W.socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_sock == (SOCKET)~0)
    {
        log_line("socket() failed");
        return 0;
    }
    W.setsockopt(g_listen_sock, SOL_SOCKET, SO_REUSEADDR,
                 (const char *)&optv, sizeof(optv));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = W.htons((unsigned short)g_port);
    a.sin_addr.s_addr = W.htonl(INADDR_ANY);
    if (W.bind(g_listen_sock, (const struct sockaddr *)&a, sizeof(a)) == SOCKET_ERROR)
    {
        log_line("bind() failed - is the port already in use?");
        return 0;
    }
    if (W.listen(g_listen_sock, 5) == SOCKET_ERROR)
        return 0;
    return 1;
}

static void serve_loop(void)
{
    for (;;)
    {
        SOCKET c;
        struct sockaddr_in ca;
        int cla;

        if (g_listen_sock == (SOCKET)~0)   /* listener was closed on stop */
            break;
        cla = sizeof(ca);
        c = W.accept(g_listen_sock, (struct sockaddr *)&ca, &cla);
        if (c == (SOCKET)~0)
        {
            /* accept failed: either the stop handler closed the listener, or
             * it was transient.  Check the stop event, then retry/back off. */
            if (g_stopEvt &&
                WaitForSingleObject(g_stopEvt, 0) == WAIT_OBJECT_0)
                break;
            if (g_listen_sock == (SOCKET)~0)
                break;
            Sleep(50);
            continue;
        }
        g_cli_sock = c;
        g_cli_ip[0] = 0;
        if (W.inet_ntoa) strncpy(g_cli_ip, W.inet_ntoa(ca.sin_addr), 15);
        {
            char lg[96];
            sprintf(lg, "client connected from %s", g_cli_ip);
            log_line(lg);
        }
        handle_client(c);
        g_cli_sock = (SOCKET)~0;
    }
}

/* ------------------------- service control handler ----------------------- */
static int WINAPI svc_ctrl(DWORD code)
{
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN)
    {
        if (g_stopEvt) SetEvent(g_stopEvt);
        /* wake a blocking accept() so the serve loop sees the stop event and
         * unwinds; the loop re-checks g_listen_sock == INVALID before looping */
        if (g_listen_sock != (SOCKET)~0 && W.closesocket)
        {
            W.closesocket(g_listen_sock);
            g_listen_sock = (SOCKET)~0;
        }
        g_stat.dwCurrentState = SERVICE_STOP_PENDING;
        g_stat.dwWaitHint = 3000;
        if (z_SetStat) z_SetStat(g_hstat, &g_stat);
    }
    return 0;
}

/* The service path, driven from main() in the classic single-service style:
 * register the control handler, push START_PENDING -> RUNNING, serve, then
 * STOPPED.  No StartServiceCtrlDispatcher table needed. */
/* Service-thread entry point, invoked by StartServiceCtrlDispatcher in its
 * own thread.  RegisterServiceCtrlHandlerA is honoured by the SCM only from
 * this service context, NOT from main() -- the legacy main-thread pattern
 * was the 0x41d "not responding to control function" we hit on Win2000. */
static DWORD WINAPI service_main(DWORD argc, LPSTR *argv)
{
    char lg[96];
    DWORD e;
    (void)argc;
    (void)argv;

    log_line("service thread entered");
    g_stopEvt = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_hstat = z_RegHandler(SVC_KEY, svc_ctrl);
    if (!g_hstat)
    {
        e = GetLastError();
        sprintf(lg, "RegisterServiceCtrlHandler failed (gle=%lu)",
                (unsigned long)e);
        log_line(lg);
        return 1;
    }
    g_stat.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_stat.dwCurrentState = SERVICE_START_PENDING;
    g_stat.dwControlsAccepted = 0;
    g_stat.dwWaitHint = 5000;
    g_stat.dwCheckPoint = 0;
    g_stat.dwServiceSpecificExitCode = 0;
    if (z_SetStat) z_SetStat(g_hstat, &g_stat);

    if (!load_wsock())
    {
        log_line("winsock unavailable, stopping");
        g_stat.dwCurrentState = SERVICE_STOPPED;
        if (z_SetStat) z_SetStat(g_hstat, &g_stat);
        return 1;
    }
    if (W.WSAStartup(MAKEWORD(2, 2), &wsd) != 0)
    {
        log_line("WSAStartup failed, stopping");
        g_stat.dwCurrentState = SERVICE_STOPPED;
        if (z_SetStat) z_SetStat(g_hstat, &g_stat);
        return 1;
    }
    InitializeCriticalSection(&g_send_cs);
    if (open_listener())
    {
        g_stat.dwCurrentState = SERVICE_RUNNING;
        g_stat.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
        g_stat.dwWaitHint = 0;
        if (z_SetStat) z_SetStat(g_hstat, &g_stat);
        log_line("service RUNNING on listener");
        serve_loop();
        g_stat.dwCurrentState = SERVICE_STOPPED;
        g_stat.dwWaitHint = 0;
        if (z_SetStat) z_SetStat(g_hstat, &g_stat);
    }
    else
    {
        log_line("listener failed, stopping");
        g_stat.dwCurrentState = SERVICE_STOPPED;
        if (z_SetStat) z_SetStat(g_hstat, &g_stat);
    }
    W.WSACleanup();
    return 0;
}

/* Launch the SCM dispatcher with a single-service table.  The dispatcher
 * creates the service thread that runs service_main; it returns non-zero on
 * success (a service-thread fatal error) or zero on dispatch failure. */
static int run_service(void)
{
    char lg[96];
    DWORD e;
    /* Classic (ANSI, un-suffixed) service table type: the oldest form,
     * present in every Win32 header including old MinGW-for-Win2000.  The
     * newer SERVICE_TABLE_ENTRYA / LPSERVICE_MAIN_FUNCTIONA spellings are
     * not guaranteed to exist in that toolchain. */
    const SERVICE_TABLE_ENTRY table[2] = {
        { (LPSTR)SVC_KEY, service_main },
        { NULL, NULL }
    };

    if (!load_advapi() || !z_ScDispatcher)
    {
        log_line("advapi32 dispatcher unavailable, cannot run as service");
        return 1;
    }
    log_line("service entered, launching dispatcher");
    z_ScDispatcher(table);
    /* Reaching here means a service thread called ReportServiceStatus with a
     * terminal state.  Surface the last SCM error for the log. */
    e = GetLastError();
    sprintf(lg, "dispatcher returned (gle=%lu)", (unsigned long)e);
    log_line(lg);
    return 0;
}

/* --------------------------- install / uninstall ------------------------- */
static int install_service(void)
{
    SC_HANDLE scm, sv;
    char binpath[MAX_PATH + 16];
    const char *disp = SVC_DISPLAY;

    if (!load_advapi() || !z_OpenSCM || !z_CreateSvc)
    {
        log_line("advapi32 unavailable, cannot install");
        return 1;
    }
    sprintf(binpath, "\"%s\" service --port %d --token %s",
            g_exe, g_port, g_token);
    scm = z_OpenSCM(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm)
    {
        log_line("OpenSCManager failed");
        return 1;
    }
    sv = z_CreateSvc(scm, SVC_KEY, disp,
                     SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                     SERVICE_AUTO_START, SERVICE_ERROR_IGNORE,
                     binpath, NULL, NULL, NULL, NULL, 0);
    if (!sv)
    {
        log_line("CreateService failed (service may already exist)");
        CloseHandle(scm);
        return 1;
    }
    CloseHandle(sv);
    {
        /* start it now so the agent is live right away */
        sv = z_OpenSvc(scm, SVC_KEY, SERVICE_ALL_ACCESS);
        if (sv)
        {
            SERVICE_STATUS ss;
            if (z_QStat(sv, &ss) &&
                z_StartSvc(sv, 0, NULL))
            {
                log_line("service installed and started");
            }
            else
            {
                log_line("service installed (start deferred)");
            }
            CloseHandle(sv);
        }
    }
    CloseHandle(scm);
    return 0;
}

/* Note: CreateServiceA signature - fix the call shape below. */
static int uninstall_service(void)
{
    SC_HANDLE scm, sv;
    SERVICE_STATUS ss;
    int i;
    if (!load_advapi()) return 1;
    scm = z_OpenSCM(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) return 1;
    sv = z_OpenSvc(scm, SVC_KEY, SERVICE_ALL_ACCESS);
    if (!sv)
    {
        if (z_QStat) z_QStat((SC_HANDLE)0, &ss);
        CloseHandle(scm);
        /* service may not exist */
        return 0;
    }
    for (i = 0; i < 20; i++)
    {
        if (z_QStat) z_QStat(sv, &ss);
        if (ss.dwCurrentState == SERVICE_STOPPED) break;
        if (z_CtrlSvc) z_CtrlSvc(sv, SERVICE_CONTROL_STOP, &ss);
        Sleep(500);
    }
    if (z_DelSvc(sv))
        log_line("service deleted");
    CloseHandle(sv);
    CloseHandle(scm);
    return 0;
}

static int status_service(void)
{
    SC_HANDLE scm, sv;
    SERVICE_STATUS ss;
    int rc = 1;
    if (!load_advapi()) return 1;
    scm = z_OpenSCM(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) return 1;
    sv = z_OpenSvc(scm, SVC_KEY, SERVICE_QUERY_STATUS);
    if (!sv)
    {
        CloseHandle(scm);
        return 2;   /* not installed */
    }
    if (z_QStat(sv, &ss))
    {
        const char *s;
        switch (ss.dwCurrentState)
        {
            case SERVICE_STOPPED: s = "stopped"; rc = 0; break;
            case SERVICE_RUNNING: s = "running"; rc = 0; break;
            default: s = "pending"; rc = 0; break;
        }
        printf("service %s: %s (exitcode=%lu)\n", SVC_KEY, s,
               (unsigned long)ss.dwWin32ExitCode);
    }
    CloseHandle(sv);
    CloseHandle(scm);
    return rc;
}

/* ------------------------------- main ----------------------------------- */
int main(int argc, char **argv)
{
    int want_install = 0, want_uninstall = 0, want_status = 0;
    int want_service = 0, want_console = 0;

    paths_from_exe(0);
    load_ini(g_inipath);
    apply_env();
    parse_args(argc, argv, &want_install, &want_uninstall, &want_status,
               &want_service, &want_console);

    if (want_install)
        return install_service() ? 1 : 0;
    if (want_uninstall)
        return uninstall_service() ? 1 : 0;
    if (want_status)
        return status_service() ? 1 : 0;

    if (!load_wsock())
    {
        log_line("winsock unavailable");
        if (!want_service) printf("winsock unavailable\n");
        return 1;
    }
    if (W.WSAStartup(MAKEWORD(2, 2), &wsd) != 0)
    {
        log_line("WSAStartup failed");
        return 1;
    }

    if (want_service)
    {
        /* Runs under the SCM: register handler, report status, serve. */
        int rc;
        rc = run_service();
        W.WSACleanup();
        return rc;
    }

    /* console / foreground mode */
    InitializeCriticalSection(&g_send_cs);
    if (!open_listener())
    {
        log_line("listener failed");
        return 1;
    }
    printf("retroctl agent listening on 0.0.0.0:%d (token required)\n",
           g_port);
    fflush(stdout);
    serve_loop();
    W.WSACleanup();
    (void)want_console;
    return 0;
}




