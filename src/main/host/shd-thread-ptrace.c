#include "main/host/shd-thread-ptrace.h"

#include <errno.h>
#include <glib.h>
#include <inttypes.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#include <x86intrin.h>

#include "main/core/worker.h"
#include "main/host/shd-thread-protected.h"
#include "main/host/tsc.h"
#include "support/logger/logger.h"

#define THREADPTRACE_TYPE_ID 3024

typedef enum {
    // Doesn't exist yet.
    THREAD_PTRACE_CHILD_STATE_NONE = 0,
    // Waiting for initial ptrace call.
    THREAD_PTRACE_CHILD_STATE_TRACE_ME,
    THREAD_PTRACE_CHILD_STATE_SYSCALL_PRE,
    THREAD_PTRACE_CHILD_STATE_SYSCALL_POST,
    THREAD_PTRACE_CHILD_STATE_EXECVE,
    THREAD_PTRACE_CHILD_STATE_SIGNALLED,
    THREAD_PTRACE_CHILD_STATE_EXITED,
} ThreadPtraceChildState;

typedef struct _PendingWrite {
    PluginPtr pluginPtr;
    void *ptr;
    size_t n;
} PendingWrite;

typedef struct _ThreadPtrace {
    Thread base;

    SysCallHandler* sys;

    // GArray of PendingWrite, to be flushed before returning control to the
    // plugin.
    GArray* pendingWrites;

    // GArray of void*s that were previously returned by
    // threadptrace_readPluginPtr. These should be freed before returning
    // control to the plugin.
    GArray* readPointers;

    Tsc tsc;

    FILE* childMemFile;
    bool childMemFileIsDirty;

    pid_t childPID;

    int threadID;

    // Reason for the most recent transfer of control back to Shadow.
    ThreadPtraceChildState childState;

    int returnCode;

    // use for both PRE_SYSCALL and POST_SYSCALL
    struct {
        struct user_regs_struct regs;
        SysCallReturn sysCallReturn;
    } syscall;

    // Whenever we use ptrace to continue we may raise a signal.  Currently we
    // only use this to allow a signal that was already raise (e.g. SIGSEGV) to
    // be delivered.
    intptr_t signalToDeliver;
} ThreadPtrace;

// Forward declaration.
static void _threadptrace_memcpyToPlugin(ThreadPtrace* thread,
                                         PluginPtr plugin_dst, void* shadow_src,
                                         size_t n);
const void* threadptrace_readPluginPtr(Thread* base, PluginPtr plugin_src,
                                       size_t n);

static ThreadPtrace* _threadToThreadPtrace(Thread* thread) {
    utility_assert(thread->type_id == THREADPTRACE_TYPE_ID);
    return (ThreadPtrace*)thread;
}

static Thread* _threadPtraceToThread(ThreadPtrace* thread) {
    return (Thread*)thread;
}

static pid_t _threadptrace_fork_exec(const char* file, char* const argv[],
                                     char* const envp[]) {
    pid_t pid = fork();

    switch (pid) {
        case -1: {
            error("fork: %s", g_strerror(errno));
            return -1;
        }
        case 0: {
            // child
            // Disable RDTSC
            if (prctl(PR_SET_TSC, PR_TSC_SIGSEGV, 0, 0, 0) < 0) {
                error("prctl: %s", g_strerror(errno));
                return -1;
            }
            // Allow parent to trace.
            if (ptrace(PTRACE_TRACEME, 0, 0, 0) < 0) {
                error("ptrace: %s", g_strerror(errno));
                return -1;
            }
            // Wait for parent to attach.
            if (raise(SIGSTOP) < 0) {
                error("raise: %s", g_strerror(errno));
                return -1;
            }
            if (execvpe(file, argv, envp) < 0) {
                error("execvpe: %s", g_strerror(errno));
                return -1;
            }
            while (1) {
            } // here for compiler optimization
        }
        default: {
            // parent
            info("started process %s with PID %d", file, pid);
            return pid;
        }
    }
}

static void _threadptrace_enterStateTraceMe(ThreadPtrace* thread) {
    // PTRACE_O_EXITKILL: Kill child if our process dies.
    // PTRACE_O_TRACESYSGOOD: Handle syscall stops explicitly.
    // PTRACE_O_TRACEEXEC: Handle execve stops explicitly.
    if (ptrace(PTRACE_SETOPTIONS, thread->childPID, 0,
               PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEEXEC) <
        0) {
        error("ptrace: %s", strerror(errno));
        return;
    }
    // Get a handle to the child's memory.
    char path[64];
    snprintf(path, 64, "/proc/%d/mem", thread->childPID);
    thread->childMemFile = fopen(path, "r+");
    if (thread->childMemFile == NULL) {
        error("fopen %s: %s", path, g_strerror(errno));
        return;
    }
}

static void _threadptrace_enterStateSyscallPre(ThreadPtrace* thread) {
    struct user_regs_struct* const regs = &thread->syscall.regs;
    if (ptrace(PTRACE_GETREGS, thread->childPID, 0, regs) < 0) {
        error("ptrace: %s", g_strerror(errno));
        return;
    }
    SysCallArgs args = {
        .number = regs->orig_rax,
        .args = {regs->rdi, regs->rsi, regs->rdx, regs->r10, regs->r8},
    };
    thread->syscall.sysCallReturn = syscallhandler_make_syscall(
        thread->sys, _threadPtraceToThread(thread), &args);
}

static void _threadptrace_enterStateExecve(ThreadPtrace* thread) {
    // We have to reopen the handle to child's memory.
    char path[64];
    snprintf(path, 64, "/proc/%d/mem", thread->childPID);
    thread->childMemFile = freopen(path, "r+", thread->childMemFile);
    if (thread->childMemFile == NULL) {
        error("fopen %s: %s", path, g_strerror(errno));
        return;
    }
}

static void _threadptrace_enterStateSyscallPost(ThreadPtrace* thread) {
    switch (thread->syscall.sysCallReturn.state) {
        case SYSCALL_RETURN_BLOCKED: utility_assert(false); return;
        case SYSCALL_RETURN_DONE:
            // Return the specified result.
            thread->syscall.regs.rax =
                thread->syscall.sysCallReturn.retval.as_u64;
            if (ptrace(PTRACE_SETREGS, thread->childPID, 0,
                       &thread->syscall.regs) < 0) {
                error("ptrace: %s", g_strerror(errno));
                return;
            }
            break;
        case SYSCALL_RETURN_NATIVE:
            // Nothing to do. Just let it continue normally.
            break;
    }
}

static void _threadptrace_enterStateSignalled(ThreadPtrace* thread,
                                              int signal) {
    thread->childState = THREAD_PTRACE_CHILD_STATE_SIGNALLED;
    if (signal == SIGSEGV) {
        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, thread->childPID, 0, &regs) < 0) {
            error("ptrace: %s", g_strerror(errno));
            return;
        }
        uint64_t eip = regs.rip;
        const uint8_t* buf = thread_readPluginPtr(
            _threadPtraceToThread(thread), (PluginPtr){eip}, 16);
        if (isRdtsc(buf)) {
            Tsc_emulateRdtsc(&thread->tsc,
                             &regs,
                             worker_getCurrentTime() / SIMTIME_ONE_NANOSECOND);
            if (ptrace(PTRACE_SETREGS, thread->childPID, 0, &regs) < 0) {
                error("ptrace", g_strerror(errno));
                return;
            }
            return;
        }
        if (isRdtscp(buf)) {
            Tsc_emulateRdtscp(&thread->tsc,
                              &regs,
                              worker_getCurrentTime() / SIMTIME_ONE_NANOSECOND);
            if (ptrace(PTRACE_SETREGS, thread->childPID, 0, &regs) < 0) {
                error("ptrace", g_strerror(errno));
                return;
            }
            return;
        }
        error("Unhandled SIGSEGV addr:%016lx contents:%x %x %x %x %x %x %x %x",
              eip, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6],
              buf[7]);
        // fall through
    }
    // Deliver the signal.
    warning("Delivering signal %d", signal);
    thread->signalToDeliver = signal;
}

static void _threadptrace_nextChildState(ThreadPtrace* thread) {
    // Wait for child to stop.
    int wstatus;
    if (waitpid(thread->childPID, &wstatus, 0) < 0) {
        error("waitpid: %s", g_strerror(errno));
        return;
    }

    if (WIFSIGNALED(wstatus)) {
        // Killed by a signal.
        int signum = WTERMSIG(wstatus);
        debug("child %d terminated by signal %d", thread->childPID, signum);
        thread->childState = THREAD_PTRACE_CHILD_STATE_EXITED;
        thread->returnCode = -1;
    }
    if (WIFEXITED(wstatus)) {
        // Exited for some other reason.
        thread->childState = THREAD_PTRACE_CHILD_STATE_EXITED;
        thread->returnCode = WEXITSTATUS(wstatus);
        return;
    }
    if (!WIFSTOPPED(wstatus)) {
        // NOT stopped by a ptrace event.
        error("Unknown waitpid reason. wstatus: %x", wstatus);
        return;
    }
    const int signal = WSTOPSIG(wstatus);
    if (signal == SIGSTOP &&
        thread->childState == THREAD_PTRACE_CHILD_STATE_NONE) {
        // We caught the "raise(SIGSTOP)" just after forking.
        thread->childState = THREAD_PTRACE_CHILD_STATE_TRACE_ME;
        _threadptrace_enterStateTraceMe(thread);
        return;
    }
    // Condition taken from 'man ptrace' for PTRACE_O_TRACEEXEC
    if (wstatus >> 8 == (SIGTRAP | (PTRACE_EVENT_EXEC << 8))) {
        thread->childState = THREAD_PTRACE_CHILD_STATE_EXECVE;
        _threadptrace_enterStateExecve(thread);
        return;
    }
    // See PTRACE_O_TRACESYSGOOD in `man 2 ptrace`
    if (signal == (SIGTRAP | 0x80)) {
        if (thread->childState == THREAD_PTRACE_CHILD_STATE_SYSCALL_PRE ||
            thread->childState == THREAD_PTRACE_CHILD_STATE_EXECVE) {
            thread->childState = THREAD_PTRACE_CHILD_STATE_SYSCALL_POST;
            _threadptrace_enterStateSyscallPost(thread);
        } else {
            thread->childState = THREAD_PTRACE_CHILD_STATE_SYSCALL_PRE;
            _threadptrace_enterStateSyscallPre(thread);
        }
        return;
    }
    _threadptrace_enterStateSignalled(thread, signal);

    return;
}

void threadptrace_run(Thread* base, gchar** argv, gchar** envv) {
    ThreadPtrace* thread = _threadToThreadPtrace(base);

    thread->childPID = _threadptrace_fork_exec(argv[0], argv, envv);

    _threadptrace_nextChildState(thread);
    thread_resume(_threadPtraceToThread(thread));
}

void threadptrace_resume(Thread* base) {
    ThreadPtrace* thread = _threadToThreadPtrace(base);

    while (true) {
        switch (thread->childState) {
            case THREAD_PTRACE_CHILD_STATE_NONE:
                debug("THREAD_PTRACE_CHILD_STATE_NONE");
                utility_assert(false);
                break;
            case THREAD_PTRACE_CHILD_STATE_TRACE_ME:
                debug("THREAD_PTRACE_CHILD_STATE_TRACE_ME");
                break;
            case THREAD_PTRACE_CHILD_STATE_SYSCALL_PRE:
                debug("THREAD_PTRACE_CHILD_STATE_SYSCALL_PRE");
                switch (thread->syscall.sysCallReturn.state) {
                    case SYSCALL_RETURN_BLOCKED: return;
                    case SYSCALL_RETURN_DONE:
                        // We have to let the child make *a* syscall, so we
                        // ensure that it will fail.
                        thread->syscall.regs.orig_rax = -1;
                        if (ptrace(PTRACE_SETREGS, thread->childPID, 0,
                                   &thread->syscall.regs) != 0) {
                            error("ptrace: %s", g_strerror(errno));
                            return;
                        }
                        break;
                    case SYSCALL_RETURN_NATIVE: {
                        // Nothing to do. Just let it continue normally.
                        break;
                    }
                }
                break;
            case THREAD_PTRACE_CHILD_STATE_SYSCALL_POST:
                debug("THREAD_PTRACE_CHILD_STATE_SYSCALL_POST");
                break;
            case THREAD_PTRACE_CHILD_STATE_EXECVE:
                debug("THREAD_PTRACE_CHILD_STATE_EXECVE");
                break;
            case THREAD_PTRACE_CHILD_STATE_EXITED:
                debug("THREAD_PTRACE_CHILD_STATE_EXITED");
                return;
            case THREAD_PTRACE_CHILD_STATE_SIGNALLED:
                debug("THREAD_PTRACE_CHILD_STATE_SIGNALLED");
                break;
                // no default
        }
        // Free any read pointers
        if (thread->readPointers->len > 0) {
            for (int i = 0; i < thread->pendingWrites->len; ++i) {
                void* p = g_array_index(thread->pendingWrites, void*, i);
                g_free(p);
            }
            thread->readPointers = g_array_set_size(thread->readPointers, 0);
        }
        // Perform writes if needed
        if (thread->pendingWrites->len > 0) {
            for (int i = 0; i < thread->pendingWrites->len; ++i) {
                PendingWrite* write =
                    &g_array_index(thread->pendingWrites, PendingWrite, i);
                _threadptrace_memcpyToPlugin(
                    thread, write->pluginPtr, write->ptr, write->n);
                g_free(write->ptr);
            }
            thread->pendingWrites = g_array_set_size(thread->pendingWrites, 0);
            thread->childMemFileIsDirty = true;
        }
        // Flush writes if needed
        if (thread->childMemFileIsDirty) {
            if (fflush(thread->childMemFile) != 0) {
                error("fflush: %s", g_strerror(errno));
            }
            thread->childMemFileIsDirty = false;
        }
        // Allow child to start executing.
        if (ptrace(PTRACE_SYSCALL, thread->childPID, 0,
                   thread->signalToDeliver) < 0) {
            error("ptrace %d: %s", thread->childPID, g_strerror(errno));
            return;
        }
        thread->signalToDeliver = 0;
        _threadptrace_nextChildState(thread);
    }
}

gboolean threadptrace_isRunning(Thread* base) {
    ThreadPtrace* thread = _threadToThreadPtrace(base);
    switch (thread->childState) {
        case THREAD_PTRACE_CHILD_STATE_TRACE_ME:
        case THREAD_PTRACE_CHILD_STATE_SYSCALL_PRE:
        case THREAD_PTRACE_CHILD_STATE_SYSCALL_POST:
        case THREAD_PTRACE_CHILD_STATE_SIGNALLED:
        case THREAD_PTRACE_CHILD_STATE_EXECVE: return true;
        case THREAD_PTRACE_CHILD_STATE_NONE:
        case THREAD_PTRACE_CHILD_STATE_EXITED: return false;
    }
    utility_assert(false);
    return false;
}

void threadptrace_terminate(Thread* base) {
    ThreadPtrace* thread = _threadToThreadPtrace(base);

    if (!threadptrace_isRunning(base)) {
        return;
    }

    int status = 0;

    utility_assert(thread->childPID > 0);

    pid_t rc = waitpid(thread->childPID, &status, WNOHANG);
    utility_assert(rc != -1);

    if (rc == 0) { // child is running, request a stop
        debug("sending SIGTERM to %d", thread->childPID);
        kill(thread->childPID, SIGTERM);
        _threadptrace_nextChildState(thread);
        utility_assert(!threadptrace_isRunning(base));
    }
}

int threadptrace_getReturnCode(Thread* base) {
    ThreadPtrace* thread = _threadToThreadPtrace(base);
    utility_assert(thread->childState == THREAD_PTRACE_CHILD_STATE_EXITED);
    return thread->returnCode;
}

void threadptrace_setSysCallResult(Thread* base, SysCallReg retval) {
    ThreadPtrace* thread = _threadToThreadPtrace(base);
    utility_assert(thread->childState == THREAD_PTRACE_CHILD_STATE_SYSCALL_PRE);
    thread->syscall.sysCallReturn =
        (SysCallReturn){.state = SYSCALL_RETURN_DONE, .retval = retval};
}

void threadptrace_free(Thread* base) {
    ThreadPtrace* thread = _threadToThreadPtrace(base);

    if (thread->sys) {
        syscallhandler_unref(thread->sys);
    }

    MAGIC_CLEAR(base);
    g_free(thread);
}

static void _threadptrace_memcpyToShadow(ThreadPtrace* thread, void* shadow_dst,
                                         PluginPtr plugin_src, size_t n) {
    clearerr(thread->childMemFile);
    if (fseek(thread->childMemFile, plugin_src.val, SEEK_SET) < 0) {
        error("fseek %p: %s", (void*)plugin_src.val, g_strerror(errno));
        return;
    }
    size_t count = fread(shadow_dst, 1, n, thread->childMemFile);
    if (count != n) {
        if (feof(thread->childMemFile)) {
            error("EOF");
            return;
        }
        error("fread %u -> %u: %s", n, count, g_strerror(errno));
    }
    return;
}

static void _threadptrace_memcpyToPlugin(ThreadPtrace* thread,
                                         PluginPtr plugin_dst, void* shadow_src,
                                         size_t n) {
    if (fseek(thread->childMemFile, plugin_dst.val, SEEK_SET) < 0) {
        error("fseek %p: %s", (void*)plugin_dst.val, g_strerror(errno));
        return;
    }
    size_t count = fwrite(shadow_src, 1, n, thread->childMemFile);
    if (count != n) {
        error("fwrite %u -> %u: %s", n, count, g_strerror(errno));
    }
    thread->childMemFileIsDirty = true;
    return;
}

void* threadptrace_clonePluginPtr(Thread* base, PluginPtr plugin_src,
                                  size_t n) {
    ThreadPtrace* thread = _threadToThreadPtrace(base);
    void* rv = g_new(void, n);
    _threadptrace_memcpyToShadow(thread, rv, plugin_src, n);
    return rv;
}

void threadptrace_releaseClonedPtr(Thread* base, void* p) { g_free(p); }

const void* threadptrace_readPluginPtr(Thread* base, PluginPtr plugin_src,
                                       size_t n) {
    // TODO: Use mmap instead.

    ThreadPtrace* thread = _threadToThreadPtrace(base);
    void* rv = g_new(void, n);
    g_array_append_val(thread->readPointers, rv);
    _threadptrace_memcpyToShadow(thread, rv, plugin_src, n);
    return rv;
}

void* threadptrace_writePluginPtr(Thread* base, PluginPtr plugin_src,
                                  size_t n) {
    // TODO: Use mmap instead.

    ThreadPtrace* thread = _threadToThreadPtrace(base);
    void* rv = g_new(void, n);
    PendingWrite pendingWrite = {.pluginPtr = plugin_src, .ptr = rv, .n = n};
    g_array_append_val(thread->pendingWrites, pendingWrite);
    return rv;
}

Thread* threadptrace_new(gint threadID, SysCallHandler* sys) {
    ThreadPtrace* thread = g_new(ThreadPtrace, 1);
    *thread = (ThreadPtrace){
        .base = (Thread){.run = threadptrace_run,
                         .resume = threadptrace_resume,
                         .terminate = threadptrace_terminate,
                         .setSysCallResult = threadptrace_setSysCallResult,
                         .getReturnCode = threadptrace_getReturnCode,
                         .isRunning = threadptrace_isRunning,
                         .free = threadptrace_free,
                         .clonePluginPtr = threadptrace_clonePluginPtr,
                         .releaseClonedPtr = threadptrace_releaseClonedPtr,
                         .readPluginPtr = threadptrace_readPluginPtr,
                         .writePluginPtr = threadptrace_writePluginPtr,

                         .type_id = THREADPTRACE_TYPE_ID,
                         .referenceCount = 1},
        .sys = sys,
        // FIXME: This should the emulated CPU's frequency
        .tsc = {.cyclesPerSecond = 2000000000UL},
        .threadID = threadID,
        .childState = THREAD_PTRACE_CHILD_STATE_NONE};

    thread->pendingWrites = g_array_new(FALSE, FALSE, sizeof(PendingWrite));
    thread->readPointers = g_array_new(FALSE, FALSE, sizeof(void*));

    syscallhandler_ref(sys);
    MAGIC_INIT(&thread->base);

    return _threadPtraceToThread(thread);
}