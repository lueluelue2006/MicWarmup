#include "control.h"
#include "hal_probe.h"

#include <CoreAudio/CoreAudio.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    MWControl control;
    atomic_bool finished;
    MWOutcome outcome;
} Session;

static volatile sig_atomic_t signaled;

static void CancelOnSignal(int number) { (void)number; signaled = 1; }

static MWInterrupt OwnerState(pid_t owner) {
    return signaled || getppid() != owner ? MWCancel : MWContinue;
}

static MWInput Probe(void *ctx) { (void)ctx; return MWProbeBuiltInInput(true); }
static int32_t Start(void *ctx, MWDeviceID id) { (void)ctx; return AudioDeviceStart(id, NULL); }
static void AudioStopping(Session *session) { MWControlAudioStopping(&session->control, MWNow()); }
static int32_t Stop(void *ctx, MWDeviceID id) {
    AudioStopping(ctx);
    return AudioDeviceStop(id, NULL);
}
static MWDeviceState DeviceState(void *ctx, MWDeviceID id) {
    (void)ctx;
    return MWCheckBuiltInInput(id);
}
static bool ReportActive(void *ctx, MWBefore before) {
    Session *session = ctx;
    if (MWControlAudioInterruption(&session->control, MWNow()) != MWContinue) return false;
    const char byte = (char)before;
    if (write(STDOUT_FILENO, &byte, 1) == 1) return MWControlAudioReady(&session->control, MWNow());
    AudioStopping(ctx);
    return false;
}
static MWInterrupt Interruption(void *ctx) {
    Session *session = ctx;
    return MWControlAudioInterruption(&session->control, MWNow());
}
static uint64_t Now(void *ctx) { (void)ctx; return MWNow(); }
static void Pause(void *ctx, uint64_t duration) {
    (void)ctx;
    struct timespec time = { (time_t)(duration / MW_SECOND), (long)(duration % MW_SECOND) };
    nanosleep(&time, NULL);
}

static void *RunAudio(void *ctx) {
    Session *session = ctx;
    MWOps ops = { ctx, Probe, Start, Stop, DeviceState, ReportActive, Interruption, Now, Pause };
    session->outcome = MWRunSupervised(&ops);
    AudioStopping(session);
    atomic_store(&session->finished, true);
    return NULL;
}

static int CheckOnly(void) {
    MWInput input = MWProbeBuiltInInput(true);
    switch (input.selection) {
        case MWInputOK:
            puts("built_in_input=unique");
            printf("hardware_running=%s\n", input.before == MWBeforeRunning ? "yes" :
                   input.before == MWBeforeStopped ? "no" : "unknown");
            return 0;
        case MWInputNone: puts("built_in_input=none"); return MWNoDevice;
        case MWInputAmbiguous: puts("built_in_input=ambiguous"); return MWMultipleDevices;
        case MWInputQueryFailed: puts("built_in_input=unavailable"); return MWQueryFailed;
    }
    return MWQueryFailed;
}

static bool IsPipe(int fd) {
    struct stat info;
    return fstat(fd, &info) == 0 && S_ISFIFO(info.st_mode);
}

static bool MakeNonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

enum { SupervisorIntervalMs = 25, CommandBudget = 4096 };
static const uint64_t Millisecond = MW_SECOND / 1000;

static int PollInterval(uint64_t now, uint64_t next) {
    if (!now || now >= next) return 0;
    uint64_t remaining = next - now;
    if (remaining >= SupervisorIntervalMs * Millisecond) return SupervisorIntervalMs;
    return (int)((remaining + Millisecond - 1) / Millisecond);
}

static MWInterrupt WaitForPipe(pid_t owner, int waitMs, bool *readable) {
    MWInterrupt outside = OwnerState(owner);
    if (outside != MWContinue) return outside;
    struct pollfd pipe = { .fd = STDIN_FILENO, .events = POLLIN };
    int events = poll(&pipe, 1, waitMs);
    int pollError = errno;
    outside = OwnerState(owner);
    if (outside != MWContinue) return outside;
    if (pipe.revents & POLLHUP) return MWCancel;
    if ((events < 0 && pollError != EINTR) || (pipe.revents & (POLLERR | POLLNVAL))) {
        return MWControlError;
    }
    if (readable) *readable = (pipe.revents & POLLIN) != 0;
    return MWContinue;
}

static MWInterrupt AwaitStart(pid_t owner, uint64_t now, uint64_t deadline) {
    uint64_t startLimit = deadline - now < 2 * MW_SECOND ? deadline : now + 2 * MW_SECOND;
    while ((now = MWNow()) && now < startLimit) {
        bool readable;
        MWInterrupt outside = WaitForPipe(owner, PollInterval(now, startLimit), &readable);
        if (outside != MWContinue) return outside;
        if (!readable) continue;
        unsigned char start;
        ssize_t count = read(STDIN_FILENO, &start, 1);
        if (count == 0) return MWCancel;
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count != 1 || start != 'S') return MWControlError;
        return OwnerState(owner);
    }
    if (OwnerState(owner) != MWContinue) return MWCancel;
    return now && now >= deadline ? MWTimeUp : MWControlError;
}

static MWInterrupt ReadCommands(pid_t owner, int waitMs, unsigned char bytes[CommandBudget], size_t *size) {
    MWInterrupt outside = WaitForPipe(owner, waitMs, NULL);
    if (outside != MWContinue) return outside;

    // Drain even on poll timeout so a frame at the soft deadline is considered first.
    // Do not apply any frames from a batch that also contains EOF or a control failure.
    while (*size < CommandBudget) {
        ssize_t count = read(STDIN_FILENO, bytes + *size, CommandBudget - *size);
        int readError = errno;
        outside = OwnerState(owner);
        if (outside != MWContinue) return outside;
        if (count > 0) *size += (size_t)count;
        else if (count == 0) return MWCancel;
        else if (readError == EAGAIN || readError == EWOULDBLOCK) return MWContinue;
        else if (readError != EINTR) return MWControlError;
    }
    // A writer cannot starve time and owner checks by feeding commands continuously.
    return MWControlError;
}

static bool Ack(void *ctx, uint64_t deadline) {
    (void)ctx;
    unsigned char frame[MW_CONTROL_FRAME_SIZE];
    MWControlEncode(frame, MW_CONTROL_ACK, deadline);
    // One write below PIPE_BUF keeps the acknowledgment whole next to the audio status byte.
    return write(STDOUT_FILENO, frame, sizeof(frame)) == (ssize_t)sizeof(frame);
}

static int Advance(Session *session, pid_t owner, int waitMs) {
    unsigned char bytes[CommandBudget];
    size_t size = 0;
    MWInterrupt outside = MWContinue;
    if (MWControlInterruption(&session->control) == MWContinue) outside = ReadCommands(owner, waitMs, bytes, &size);
    else if (waitMs) Pause(NULL, (uint64_t)waitMs * Millisecond);
    uint64_t now = MWNow();
    if (OwnerState(owner) != MWContinue) outside = MWCancel;
    return MWControlAdvance(&session->control, now, outside, bytes, size, Ack, NULL);
}

static uint64_t ReadNumber(const char *text) {
    if (!text[0] || text[strspn(text, "0123456789")]) return 0;
    errno = 0;
    uintmax_t number = strtoumax(text, NULL, 10);
    return errno || number > UINT64_MAX ? 0 : (uint64_t)number;
}

static int Supervise(pid_t owner, uint64_t deadline) {
    uint64_t now = MWNow();
    Session session = {0};
    if (owner <= 1 || getppid() != owner || !IsPipe(STDIN_FILENO) || !IsPipe(STDOUT_FILENO)) {
        return MWControlFailed;
    }
    if (!MWControlInit(&session.control, now, deadline)) {
        // A valid request can expire while the child is waiting to be scheduled.
        if (deadline && deadline <= now && deadline <= UINT64_MAX - MW_STOP_GRACE) return MWExpired;
        return MWControlFailed;
    }
    atomic_init(&session.finished, false);

    struct sigaction signalAction = { .sa_handler = CancelOnSignal };
    sigemptyset(&signalAction.sa_mask);
    if (sigaction(SIGTERM, &signalAction, NULL) == -1 ||
        sigaction(SIGINT, &signalAction, NULL) == -1 || signal(SIGPIPE, SIG_IGN) == SIG_ERR ||
        !MakeNonblocking(STDIN_FILENO) || !MakeNonblocking(STDOUT_FILENO)) {
        return MWControlFailed;
    }

    MWInterrupt initial = AwaitStart(owner, now, deadline);
    if (initial == MWContinue) {
        int forcedExit = Advance(&session, owner, 0);
        if (forcedExit) return forcedExit;
        initial = MWControlInterruption(&session.control);
    }
    if (initial == MWCancel) return MWStopped;
    if (initial == MWTimeUp) return MWExpired;
    if (initial != MWContinue) return MWControlFailed;

    // Keep signals on the supervisor thread so the worker can't miss a stop request.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    if (!MWControlBeginStartup(&session.control, MWNow())) return MWControlFailed;
    if (pthread_sigmask(SIG_BLOCK, &signals, NULL) != 0) return MWControlFailed;
    pthread_t audio;
    if (pthread_create(&audio, NULL, RunAudio, &session) != 0) return MWControlFailed;
    if (pthread_sigmask(SIG_UNBLOCK, &signals, NULL) != 0) _exit(MWWatchdog);

    while (!atomic_load(&session.finished)) {
        now = MWNow();
        int waitMs = PollInterval(now, MWControlNextCheck(&session.control));
        int forcedExit = Advance(&session, owner, waitMs);
        if (forcedExit && !atomic_load(&session.finished)) {
            _exit(forcedExit);
        }
    }
    pthread_join(audio, NULL);
    return MWControlCompletedOutcome(&session.control, session.outcome);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--check") == 0) return CheckOnly();
    if (argc == 4 && strcmp(argv[1], "--ui-session") == 0) {
        uint64_t owner = ReadNumber(argv[2]);
        if (owner > INT32_MAX) return MWControlFailed;
        return Supervise((pid_t)owner, ReadNumber(argv[3]));
    }
    fputs("Use the app to request a session, or --check for a read-only check.\n", stderr);
    return MWControlFailed;
}
