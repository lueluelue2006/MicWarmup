#include "control.h"
#include "hal_probe.h"

#include <CoreAudio/CoreAudio.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Link the production helper entry point and control pipe code against a fake HAL.
int MWHelperEntry(int argc, char **argv);

typedef enum { Healthy, HangStart, HangStop } Mode;
static Mode mode;
static int halEvents;

MWInput MWProbeBuiltInInput(bool includeRunningStatus) {
    (void)includeRunningStatus;
    return (MWInput){ MWInputOK, 17, MWBeforeStopped };
}

OSStatus AudioDeviceStart(AudioDeviceID device, AudioDeviceIOProcID proc) {
    assert(device == 17 && !proc && write(halEvents, "+", 1) == 1);
    while (mode == HangStart) pause();
    return 0;
}

OSStatus AudioDeviceStop(AudioDeviceID device, AudioDeviceIOProcID proc) {
    assert(device == 17 && !proc && write(halEvents, "-", 1) == 1);
    while (mode == HangStop) pause();
    return 0;
}

typedef struct {
    uint64_t deadline;
    int command[2], output[2], audio[2];
} Process;

static Process NewProcess(uint64_t duration) {
    Process p = { .deadline = MWNow() + duration };
    assert(pipe(p.command) == 0 && pipe(p.output) == 0 && pipe(p.audio) == 0);
    return p;
}

static void Close(int fd) { assert(close(fd) == 0); }

static pid_t Launch(Process p, Mode selected) {
    pid_t owner = getpid();
    pid_t child = fork();
    assert(child >= 0);
    if (child) return child;

    assert(dup2(p.command[0], STDIN_FILENO) == STDIN_FILENO);
    assert(dup2(p.output[1], STDOUT_FILENO) == STDOUT_FILENO);
    Close(p.command[0]); Close(p.command[1]);
    Close(p.output[0]); Close(p.output[1]);
    Close(p.audio[0]);
    halEvents = p.audio[1];
    mode = selected;
    char ownerText[32], expiryText[32];
    snprintf(ownerText, sizeof(ownerText), "%d", owner);
    snprintf(expiryText, sizeof(expiryText), "%" PRIu64, p.deadline);
    char *constArgs[] = { "test-helper", "--ui-session", ownerText, expiryText, NULL };
    _exit(MWHelperEntry(4, constArgs));
}

static void CloseChildEnds(Process p) {
    Close(p.command[0]); Close(p.output[1]); Close(p.audio[1]);
}

static void Write(int fd, const void *bytes, size_t size) {
    assert(write(fd, bytes, size) == (ssize_t)size);
}

static void Read(int fd, unsigned char *bytes, size_t size) {
    while (size) {
        struct pollfd pipe = { .fd = fd, .events = POLLIN };
        assert(poll(&pipe, 1, 3500) == 1);
        ssize_t count = read(fd, bytes, size);
        assert(count > 0);
        bytes += count;
        size -= (size_t)count;
    }
}

static void ExpectByte(int fd, unsigned char expected) {
    unsigned char byte;
    Read(fd, &byte, 1);
    assert(byte == expected);
}

static void ExpectEOF(int fd) {
    struct pollfd pipe = { .fd = fd, .events = POLLIN };
    unsigned char unexpected;
    assert(poll(&pipe, 1, 3500) == 1 && read(fd, &unexpected, 1) == 0);
    Close(fd);
}

static void ExpectExit(pid_t child, int code) {
    uint64_t limit = MWNow() + 3 * MW_SECOND;
    int status;
    pid_t reaped;
    while ((reaped = waitpid(child, &status, WNOHANG)) == 0 && MWNow() < limit) {
        const struct timespec shortWait = { .tv_nsec = 5 * (long)(MW_SECOND / 1000) };
        nanosleep(&shortWait, NULL);
    }
    if (reaped == 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    assert(reaped == child && WIFEXITED(status) && WEXITSTATUS(status) == code);
}

static void ExpectActive(Process p) {
    ExpectByte(p.audio[0], '+');
    ExpectByte(p.output[0], MWBeforeStopped);
}

static void WriteRenew(Process p, uint64_t deadline) {
    unsigned char frame[MW_CONTROL_FRAME_SIZE];
    MWControlEncode(frame, MW_CONTROL_RENEW, deadline);
    Write(p.command[1], frame, sizeof(frame));
}

static void ExpectAck(Process p, uint64_t deadline) {
    unsigned char frame[MW_CONTROL_FRAME_SIZE];
    Read(p.output[0], frame, sizeof(frame));
    assert(frame[0] == MW_CONTROL_ACK && MWControlDecode(frame) == deadline);
}

static void TestSplitAndPackedFrames(void) {
    Process p = NewProcess(10 * MW_SECOND);
    pid_t child = Launch(p, Healthy);
    CloseChildEnds(p);
    Write(p.command[1], "S", 1);
    ExpectActive(p);

    unsigned char frames[2 * MW_CONTROL_FRAME_SIZE];
    uint64_t first = p.deadline + MW_SECOND, second = first + MW_SECOND;
    MWControlEncode(frames, MW_CONTROL_RENEW, first);
    MWControlEncode(frames + MW_CONTROL_FRAME_SIZE, MW_CONTROL_RENEW, second);
    Write(p.command[1], frames, 4);
    struct pollfd output = { .fd = p.output[0], .events = POLLIN };
    assert(poll(&output, 1, 50) == 0);
    Write(p.command[1], frames + 4, sizeof(frames) - 4);
    ExpectAck(p, first);
    ExpectAck(p, second);

    Write(p.command[1], "C", 1);
    ExpectExit(child, MWStopped);
    Close(p.command[1]);
    ExpectByte(p.audio[0], '-');
    ExpectEOF(p.audio[0]);
    ExpectEOF(p.output[0]);
}

static void TestEOFBeatsQueuedRenewal(void) {
    Process p = NewProcess(10 * MW_SECOND);
    pid_t child = Launch(p, Healthy);
    CloseChildEnds(p);
    Write(p.command[1], "S", 1);
    ExpectActive(p);

    assert(kill(child, SIGSTOP) == 0);
    int status;
    assert(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));
    WriteRenew(p, p.deadline + MW_SECOND);
    Write(p.command[1], "R", 1); // Also leave an incomplete frame queued.
    Close(p.command[1]);
    assert(kill(child, SIGCONT) == 0);

    ExpectExit(child, MWStopped);
    ExpectByte(p.audio[0], '-');
    ExpectEOF(p.audio[0]);
    ExpectEOF(p.output[0]);
}

static void TestFullOutputCannotHang(void) {
    Process p = NewProcess(10 * MW_SECOND);
    pid_t child = Launch(p, Healthy);
    Close(p.command[0]); Close(p.audio[1]);
    Write(p.command[1], "S", 1);
    ExpectActive(p);

    int flags = fcntl(p.output[1], F_GETFL);
    assert(flags != -1 && (flags & O_NONBLOCK));
    const unsigned char filler[512] = {0};
    while (write(p.output[1], filler, sizeof(filler)) == (ssize_t)sizeof(filler)) {}
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    while (write(p.output[1], filler, 1) == 1) {}
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    WriteRenew(p, p.deadline + MW_SECOND);

    ExpectExit(child, MWControlFailed);
    Close(p.command[1]); Close(p.output[1]); Close(p.output[0]);
    ExpectByte(p.audio[0], '-');
    ExpectEOF(p.audio[0]);
}

static void TestStartupWatchdog(void) {
    Process p = NewProcess(10 * MW_SECOND);
    pid_t child = Launch(p, HangStart);
    CloseChildEnds(p);
    Write(p.command[1], "S", 1);
    ExpectByte(p.audio[0], '+');
    WriteRenew(p, p.deadline + MW_SECOND);
    ExpectAck(p, p.deadline + MW_SECOND);

    uint64_t cancel = MWNow();
    assert(kill(child, SIGTERM) == 0);
    ExpectExit(child, MWWatchdog);
    assert(MWNow() >= cancel + MW_STOP_GRACE);
    Close(p.command[1]);
    ExpectEOF(p.audio[0]);
    ExpectEOF(p.output[0]);
}

static void TestStopWatchdog(void) {
    Process p = NewProcess(MW_SECOND);
    pid_t child = Launch(p, HangStop);
    CloseChildEnds(p);
    Write(p.command[1], "S", 1);
    ExpectActive(p);
    uint64_t renewed = p.deadline + MW_SECOND / 2;
    WriteRenew(p, renewed);
    ExpectAck(p, renewed);

    ExpectByte(p.audio[0], '-');
    assert(MWNow() >= renewed);
    WriteRenew(p, renewed + MW_SECOND);
    ExpectExit(child, MWWatchdog);
    assert(MWNow() >= renewed + MW_STOP_GRACE);
    Close(p.command[1]);
    ExpectEOF(p.audio[0]);
    ExpectEOF(p.output[0]);
}

static void TestOwnerExit(void) {
    Process p = NewProcess(10 * MW_SECOND);
    pid_t owner = fork();
    assert(owner >= 0);
    if (owner == 0) {
        Launch(p, Healthy);
        CloseChildEnds(p);
        Write(p.command[1], "S", 1);
        ExpectByte(p.output[0], MWBeforeStopped);
        _exit(0);
    }

    CloseChildEnds(p);
    Close(p.output[0]);
    ExpectExit(owner, 0);
    ExpectByte(p.audio[0], '+');
    ExpectByte(p.audio[0], '-');
    ExpectEOF(p.audio[0]);
    Close(p.command[1]); // The writer stayed open when the owner disappeared.
}

int main(void) {
    TestSplitAndPackedFrames();
    TestEOFBeatsQueuedRenewal();
    TestFullOutputCannotHang();
    TestStartupWatchdog();
    TestStopWatchdog();
    TestOwnerExit();
    puts("Fake HAL helper tests passed: framing, EOF, backpressure, owner loss, and both watchdog paths.");
    return 0;
}
