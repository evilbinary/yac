#include "config.h"
#include "oscompat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <stdint.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef PROCESSOR_ARCHITECTURE_ARM64
#define PROCESSOR_ARCHITECTURE_ARM64 12
#endif

static void yac_uts_copy(char *dst, size_t cap, const char *src) {
    size_t n;
    if (!dst || cap == 0) return;
    n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

int yac_uname(yac_utsname *u) {
    if (!u) return -1;
    memset(u, 0, sizeof(*u));
#ifdef _WIN32
    {
        DWORD nlen;
        OSVERSIONINFOA vi;
        SYSTEM_INFO si;
        const char *mach;

        yac_uts_copy(u->sysname, sizeof(u->sysname), "Windows_NT");
        nlen = (DWORD)sizeof(u->nodename);
        if (!GetComputerNameA(u->nodename, &nlen))
            u->nodename[0] = '\0';
        memset(&vi, 0, sizeof(vi));
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (GetVersionExA(&vi)) {
            snprintf(u->release, sizeof(u->release), "%lu.%lu.%lu",
                     (unsigned long)vi.dwMajorVersion,
                     (unsigned long)vi.dwMinorVersion,
                     (unsigned long)vi.dwBuildNumber);
            yac_uts_copy(u->version, sizeof(u->version), vi.szCSDVersion);
        }
        GetNativeSystemInfo(&si);
        if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
            mach = "x86_64";
        else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64)
            mach = "aarch64";
        else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL)
            mach = "i686";
        else
            mach = "unknown";
        yac_uts_copy(u->machine, sizeof(u->machine), mach);
        return 0;
    }
#else
    {
        struct utsname n;
        if (uname(&n) != 0) return -1;
        yac_uts_copy(u->sysname, sizeof(u->sysname), n.sysname);
        yac_uts_copy(u->nodename, sizeof(u->nodename), n.nodename);
        yac_uts_copy(u->release, sizeof(u->release), n.release);
        yac_uts_copy(u->version, sizeof(u->version), n.version);
        yac_uts_copy(u->machine, sizeof(u->machine), n.machine);
        return 0;
    }
#endif
}

int yac_uname_into(void *buf, size_t n) {
    if (!buf) return -1;
#ifdef _WIN32
    {
        yac_utsname u;
        char *p = (char *)buf;
        if (n < (size_t)YAC_UTSNAME_LEN * 5) return -1;
        if (yac_uname(&u) != 0) return -1;
        memset(p, 0, n);
        memcpy(p + 0 * YAC_UTSNAME_LEN, u.sysname, YAC_UTSNAME_LEN);
        memcpy(p + 1 * YAC_UTSNAME_LEN, u.nodename, YAC_UTSNAME_LEN);
        memcpy(p + 2 * YAC_UTSNAME_LEN, u.release, YAC_UTSNAME_LEN);
        memcpy(p + 3 * YAC_UTSNAME_LEN, u.version, YAC_UTSNAME_LEN);
        memcpy(p + 4 * YAC_UTSNAME_LEN, u.machine, YAC_UTSNAME_LEN);
        return 0;
    }
#else
    if (n < sizeof(struct utsname)) return -1;
    memset(buf, 0, n);
    return uname((struct utsname *)buf);
#endif
}

int yac_clock_gettime(int clk, struct timespec *ts) {
#ifdef _WIN32
    if (!ts) return -1;
    if (clk == CLOCK_REALTIME) {
        FILETIME ft;
        ULARGE_INTEGER u;
        uint64_t t;
        GetSystemTimeAsFileTime(&ft);
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        /* 100-ns ticks from 1601-01-01 to Unix epoch */
        t = u.QuadPart - 116444736000000000ULL;
        ts->tv_sec = (time_t)(t / 10000000ULL);
        ts->tv_nsec = (long)((t % 10000000ULL) * 100);
        return 0;
    }
    if (clk == CLOCK_MONOTONIC) {
        static LARGE_INTEGER freq;
        LARGE_INTEGER ctr;
        if (!freq.QuadPart) {
            if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0)
                return -1;
        }
        if (!QueryPerformanceCounter(&ctr)) return -1;
        ts->tv_sec = (time_t)(ctr.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)((ctr.QuadPart % freq.QuadPart) * 1000000000ULL /
                             (uint64_t)freq.QuadPart);
        return 0;
    }
    return -1;
#else
    return clock_gettime(clk, ts);
#endif
}

struct tm *yac_localtime_r(const time_t *t, struct tm *out) {
#ifdef _WIN32
    if (!t || !out) return NULL;
    if (localtime_s(out, t) != 0) return NULL;
    return out;
#else
    return localtime_r(t, out);
#endif
}

#ifndef _WIN32
static int wait_status_to_rc(int st) {
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return st;
}
#endif

int yac_system(const char *cmd) {
    int st = system(cmd);
    if (st == -1) return -1;
#ifdef _WIN32
    return st;
#else
    return wait_status_to_rc(st);
#endif
}

typedef struct {
    char *p;
    size_t n, cap;
} CapBuf;

static int capbuf_append(CapBuf *b, const char *src, size_t n) {
    if (n == 0) return 1;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        char *p;
        while (cap < b->n + n) cap *= 2;
        p = (char *)realloc(b->p, cap);
        if (!p) return 0;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, src, n);
    b->n += n;
    return 1;
}

static void fail_capture(char **out, char **err, const char **errmsg, const char *msg) {
    if (out) *out = NULL;
    if (err) *err = NULL;
    if (errmsg) *errmsg = msg;
}

#ifdef _WIN32
typedef struct {
    HANDLE h;
    CapBuf *b;
    volatile int ok;
} PipeReadCtx;

static unsigned __stdcall pipe_reader_th(void *arg) {
    PipeReadCtx *pr = (PipeReadCtx *)arg;
    char tmp[4096];
    DWORD n;
    pr->ok = 1;
    for (;;) {
        if (!ReadFile(pr->h, tmp, (DWORD)sizeof tmp, &n, NULL)) {
            DWORD e = GetLastError();
            if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF)
                break;
            pr->ok = 0;
            break;
        }
        if (n == 0)
            break;
        if (!capbuf_append(pr->b, tmp, (size_t)n)) {
            pr->ok = 0;
            break;
        }
    }
    return 0;
}

static void close_h(HANDLE h) {
    if (h && h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

int yac_popen_capture(const char *cmd, const char *in, size_t in_len,
                      int *rc, char **out, size_t *out_n, char **err, size_t *err_n,
                      const char **errmsg) {
    SECURITY_ATTRIBUTES sa;
    HANDLE in_rd = NULL, in_wr = NULL;
    HANDLE out_rd = NULL, out_wr = NULL;
    HANDLE err_rd = NULL, err_wr = NULL;
    PROCESS_INFORMATION pi;
    STARTUPINFOA si;
    char cmdline[32768];
    int ncmd;
    CapBuf ob = {0}, eb = {0};
    PipeReadCtx rout, rerr;
    HANDLE th_out = NULL, th_err = NULL;
    DWORD exit_code = 1;
    size_t in_off = 0;
    int ok = 1;

    if (out) *out = NULL;
    if (err) *err = NULL;
    if (out_n) *out_n = 0;
    if (err_n) *err_n = 0;
    if (rc) *rc = 0;

    ZeroMemory(&sa, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    ZeroMemory(&pi, sizeof pi);

    if (!CreatePipe(&in_rd, &in_wr, &sa, 0) ||
        !CreatePipe(&out_rd, &out_wr, &sa, 0) ||
        !CreatePipe(&err_rd, &err_wr, &sa, 0) ||
        !SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(err_rd, HANDLE_FLAG_INHERIT, 0)) {
        close_h(in_rd); close_h(in_wr);
        close_h(out_rd); close_h(out_wr);
        close_h(err_rd); close_h(err_wr);
        fail_capture(out, err, errmsg, "popen: pipe failed");
        return -1;
    }

    ncmd = snprintf(cmdline, sizeof cmdline, "cmd.exe /c %s", cmd);
    if (ncmd < 0 || ncmd >= (int)sizeof cmdline) {
        close_h(in_rd); close_h(in_wr);
        close_h(out_rd); close_h(out_wr);
        close_h(err_rd); close_h(err_wr);
        fail_capture(out, err, errmsg, "popen: command too long");
        return -1;
    }

    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = in_rd;
    si.hStdOutput = out_wr;
    si.hStdError = err_wr;

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        close_h(in_rd); close_h(in_wr);
        close_h(out_rd); close_h(out_wr);
        close_h(err_rd); close_h(err_wr);
        fail_capture(out, err, errmsg, "popen: CreateProcess failed");
        return -1;
    }
    close_h(in_rd); in_rd = NULL;
    close_h(out_wr); out_wr = NULL;
    close_h(err_wr); err_wr = NULL;
    CloseHandle(pi.hThread);

    rout.h = out_rd;
    rout.b = &ob;
    rout.ok = 1;
    rerr.h = err_rd;
    rerr.b = &eb;
    rerr.ok = 1;
    th_out = (HANDLE)_beginthreadex(NULL, 0, pipe_reader_th, &rout, 0, NULL);
    th_err = (HANDLE)_beginthreadex(NULL, 0, pipe_reader_th, &rerr, 0, NULL);
    if (!th_out || !th_err) {
        TerminateProcess(pi.hProcess, 1);
        ok = 0;
    } else if (in && in_len) {
        while (in_off < in_len) {
            DWORD n = 0;
            DWORD chunk = (DWORD)(in_len - in_off);
            if (chunk > 1u << 20) chunk = 1u << 20;
            if (!WriteFile(in_wr, in + in_off, chunk, &n, NULL)) {
                DWORD e = GetLastError();
                if (e != ERROR_BROKEN_PIPE && e != ERROR_NO_DATA)
                    ok = 0;
                break;
            }
            if (n == 0) break;
            in_off += (size_t)n;
        }
    }
    close_h(in_wr); in_wr = NULL;

    WaitForSingleObject(pi.hProcess, INFINITE);
    if (th_out) {
        WaitForSingleObject(th_out, INFINITE);
        CloseHandle(th_out);
    }
    if (th_err) {
        WaitForSingleObject(th_err, INFINITE);
        CloseHandle(th_err);
    }
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    close_h(out_rd);
    close_h(err_rd);

    if (!ok || !rout.ok || !rerr.ok) {
        free(ob.p);
        free(eb.p);
        fail_capture(out, err, errmsg, "popen: failed to talk to child");
        return -1;
    }

    if (rc) *rc = (int)exit_code;
    if (out) *out = ob.p;
    if (out_n) *out_n = ob.n;
    if (err) *err = eb.p;
    if (err_n) *err_n = eb.n;
    if (errmsg) *errmsg = NULL;
    return 0;
}

#else

static int drain_fd(int fd, CapBuf *b, int *eof) {
    char tmp[4096];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n > 0) {
            if (!capbuf_append(b, tmp, (size_t)n)) return 0;
            continue;
        }
        if (n == 0) {
            *eof = 1;
            return 1;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
        return 0;
    }
}

static int feed_fd(int fd, const char *src, size_t len, size_t *off, int *done) {
    while (*off < len) {
        ssize_t n = write(fd, src + *off, len - *off);
        if (n > 0) {
            *off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
        if (n < 0 && errno == EPIPE) {
            *done = 1;
            return 1;
        }
        return 0;
    }
    *done = 1;
    return 1;
}

int yac_popen_capture(const char *cmd, const char *in, size_t in_len,
                      int *rc, char **out, size_t *out_n, char **err, size_t *err_n,
                      const char **errmsg) {
    int inp[2] = {-1, -1};
    int outp[2] = {-1, -1};
    int errp[2] = {-1, -1};
    pid_t pid;
    CapBuf ob = {0}, eb = {0};
    int out_eof = 0, err_eof = 0, in_done = (in_len == 0);
    size_t in_off = 0;
    int ok = 1;
    int st = 0;
    int wrc;
    void (*old_pipe)(int);

    if (out) *out = NULL;
    if (err) *err = NULL;
    if (out_n) *out_n = 0;
    if (err_n) *err_n = 0;
    if (rc) *rc = 0;

    if (pipe(inp) != 0 || pipe(outp) != 0 || pipe(errp) != 0) {
        if (inp[0] >= 0) { close(inp[0]); close(inp[1]); }
        if (outp[0] >= 0) { close(outp[0]); close(outp[1]); }
        if (errp[0] >= 0) { close(errp[0]); close(errp[1]); }
        fail_capture(out, err, errmsg, "popen: pipe failed");
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        close(inp[0]); close(inp[1]);
        close(outp[0]); close(outp[1]);
        close(errp[0]); close(errp[1]);
        fail_capture(out, err, errmsg, "popen: fork failed");
        return -1;
    }
    if (pid == 0) {
        close(inp[1]);
        close(outp[0]);
        close(errp[0]);
        dup2(inp[0], STDIN_FILENO);
        dup2(outp[1], STDOUT_FILENO);
        dup2(errp[1], STDERR_FILENO);
        if (inp[0] != STDIN_FILENO) close(inp[0]);
        if (outp[1] != STDOUT_FILENO) close(outp[1]);
        if (errp[1] != STDERR_FILENO) close(errp[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(inp[0]);
    close(outp[1]);
    close(errp[1]);
    old_pipe = signal(SIGPIPE, SIG_IGN);
    fcntl(outp[0], F_SETFL, O_NONBLOCK);
    fcntl(errp[0], F_SETFL, O_NONBLOCK);
    fcntl(inp[1], F_SETFL, O_NONBLOCK);

    if (in_done) {
        close(inp[1]);
        inp[1] = -1;
    }
    while (ok && (!out_eof || !err_eof)) {
        struct pollfd pfd[3];
        int n;
        pfd[0].fd = out_eof ? -1 : outp[0];
        pfd[0].events = POLLIN;
        pfd[1].fd = err_eof ? -1 : errp[0];
        pfd[1].events = POLLIN;
        pfd[2].fd = in_done ? -1 : inp[1];
        pfd[2].events = POLLOUT;
        n = poll(pfd, 3, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = 0;
            break;
        }
        if (!out_eof && pfd[0].fd >= 0 && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (!drain_fd(outp[0], &ob, &out_eof)) ok = 0;
        }
        if (!err_eof && pfd[1].fd >= 0 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (!drain_fd(errp[0], &eb, &err_eof)) ok = 0;
        }
        if (!in_done && pfd[2].fd >= 0 && (pfd[2].revents & (POLLOUT | POLLERR | POLLHUP))) {
            if (!feed_fd(inp[1], in ? in : "", in_len, &in_off, &in_done)) ok = 0;
            if (in_done && inp[1] >= 0) {
                close(inp[1]);
                inp[1] = -1;
            }
        }
    }
    if (inp[1] >= 0) close(inp[1]);
    close(outp[0]);
    close(errp[0]);
    signal(SIGPIPE, old_pipe);

    do {
        wrc = waitpid(pid, &st, 0);
    } while (wrc < 0 && errno == EINTR);

    if (!ok || wrc < 0) {
        free(ob.p);
        free(eb.p);
        fail_capture(out, err, errmsg, "popen: failed to talk to child");
        return -1;
    }

    if (rc) *rc = wait_status_to_rc(st);
    if (out) *out = ob.p;
    if (out_n) *out_n = ob.n;
    if (err) *err = eb.p;
    if (err_n) *err_n = eb.n;
    if (errmsg) *errmsg = NULL;
    return 0;
}

#endif

int yac_exe_dir(char *buf, size_t n) {
    if (!buf || n < 2) return -1;
#ifdef _WIN32
    {
        DWORD k = GetModuleFileNameA(NULL, buf, (DWORD)n);
        char *slash, *alt;
        if (k == 0 || k >= n) return -1;
        slash = strrchr(buf, '\\');
        alt = strrchr(buf, '/');
        if (alt && (!slash || alt > slash)) slash = alt;
        if (!slash) return -1;
        *slash = '\0';
        return 0;
    }
#else
    {
        ssize_t k = readlink("/proc/self/exe", buf, n - 1);
        char *slash;
        if (k < 0) return -1;
        buf[k] = '\0';
        slash = strrchr(buf, '/');
        if (!slash) return -1;
        *slash = '\0';
        return 0;
    }
#endif
}
