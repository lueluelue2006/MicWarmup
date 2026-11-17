#include "warmup.h"

#include <assert.h>
#include <stdio.h>

typedef struct {
    MWInput input;
    uint64_t now, cancelAt, disconnectAt;
    MWInterrupt requested;
    const MWDeviceState *readings;
    size_t readingCount, checks;
    uint64_t checkedAt[5], reportedAt;
    MWInterrupt interruptOnLastRead, interruptDuringReport;
    int startStatus, stopStatus, starts, stops, reports, probes;
    bool stopDuringProbe, stopDuringStart;
} Mock;

static Mock NewMock(void) {
    return (Mock){ .input = { MWInputOK, 17, MWBeforeRunning }, .now = MW_SECOND,
                   .cancelAt = UINT64_MAX, .disconnectAt = UINT64_MAX };
}

static MWInput Probe(void *ctx) {
    Mock *mock = ctx;
    mock->probes++;
    if (mock->stopDuringProbe) mock->requested = MWCancel;
    return mock->input;
}
static int32_t Start(void *ctx, MWDeviceID id) {
    Mock *mock = ctx;
    assert(id == mock->input.id && mock->starts == 0 && mock->stops == 0);
    mock->starts++;
    if (mock->stopDuringStart) mock->requested = MWCancel;
    return mock->startStatus;
}
static int32_t Stop(void *ctx, MWDeviceID id) {
    Mock *mock = ctx;
    assert(id == mock->input.id && mock->starts == 1 && mock->startStatus == 0 && mock->stops == 0);
    mock->stops++;
    return mock->stopStatus;
}
static MWDeviceState DeviceState(void *ctx, MWDeviceID id) {
    Mock *mock = ctx;
    assert(id == mock->input.id);
    size_t index = mock->checks++;
    if (index < sizeof(mock->checkedAt) / sizeof(mock->checkedAt[0])) mock->checkedAt[index] = mock->now;
    if (index < mock->readingCount) {
        if (index + 1 == mock->readingCount) mock->requested = mock->interruptOnLastRead;
        return mock->readings[index];
    }
    return mock->now < mock->disconnectAt ? MWDeviceAvailable : MWDeviceUnavailable;
}
static bool Report(void *ctx, MWBefore before) {
    Mock *mock = ctx;
    assert(mock->starts == 1 && mock->startStatus == 0 && mock->stops == 0);
    assert(mock->reports == 0 && before == mock->input.before);
    mock->reports++;
    mock->reportedAt = mock->now;
    mock->requested = mock->interruptDuringReport;
    return mock->requested == MWContinue;
}
static MWInterrupt Interrupt(void *ctx) {
    Mock *mock = ctx;
    return mock->now >= mock->cancelAt ? MWCancel : mock->requested;
}
static uint64_t Now(void *ctx) { return ((Mock *)ctx)->now; }
static void Pause(void *ctx, uint64_t duration) { ((Mock *)ctx)->now += duration; }

static MWOutcome Run(Mock *mock, uint64_t duration) {
    MWOps ops = { mock, Probe, Start, Stop, DeviceState, Report, Interrupt, Now, Pause };
    return MWRun(&ops, mock->now + duration);
}

static void TestSelection(void) {
    const MWCandidate devices[] = {
        { 1, false, true, true }, { 2, true, true, false }, { 3, true, false, true },
        { 4, true, true, true }, { 5, true, true, true },
    };
    MWInput none = MWSelect(devices, 3);
    assert(none.selection == MWInputNone && !none.id);
    MWInput one = MWSelect(devices, 4);
    assert(one.selection == MWInputOK && one.id == devices[3].id);
    MWInput ambiguous = MWSelect(devices, 5);
    assert(ambiguous.selection == MWInputAmbiguous && !ambiguous.id);
    Mock mock = NewMock();
    mock.input = none;
    assert(Run(&mock, MW_SECOND) == MWNoDevice && !mock.starts);
    mock.input = ambiguous;
    assert(Run(&mock, MW_SECOND) == MWMultipleDevices && !mock.starts);
    Mock unknown = NewMock();
    unknown.input.selection = MWInputQueryFailed;
    assert(Run(&unknown, MW_SECOND) == MWQueryFailed);
    assert(unknown.probes == 1 && !unknown.starts && !unknown.stops && !unknown.checks);
}

static void TestRepeatedStart(void) {
    MWPhase phase = MWIdle;
    assert(MWBegin(&phase) && !MWBegin(&phase));
    assert(MWMarkActive(&phase) && !MWBegin(&phase));
    assert(MWEnd(&phase) && !MWBegin(&phase));
    assert(!MWMarkActive(&phase) && !MWEnd(&phase));
    phase = MWIdle;
    assert(MWBegin(&phase) && MWEnd(&phase) && !MWMarkActive(&phase));
}

static void TestPairedStop(void) {
    Mock stop = NewMock();
    stop.cancelAt = stop.now + MW_SECOND;
    assert(Run(&stop, 2 * MW_SECOND) == MWStopped);
    assert(stop.starts == 1 && stop.stops == 1 && stop.reports == 1);

    Mock timeout = NewMock();
    uint64_t expire = timeout.now + MW_LIMIT;
    assert(Run(&timeout, MW_LIMIT) == MWExpired);
    assert(timeout.now == expire && timeout.starts == 1 && timeout.stops == 1 && timeout.reports == 1);

    Mock unplug = NewMock();
    unplug.disconnectAt = unplug.now + MW_SECOND / 2;
    assert(Run(&unplug, 2 * MW_SECOND) == MWDeviceChanged);
    assert(unplug.now == unplug.disconnectAt && unplug.checks == 2);
    assert(unplug.starts == 1 && unplug.stops == 1 && unplug.reports == 1);

    Mock cancelWhileStarting = NewMock();
    cancelWhileStarting.stopDuringStart = true;
    assert(Run(&cancelWhileStarting, MW_SECOND) == MWStopped);
    assert(cancelWhileStarting.starts == 1 && cancelWhileStarting.stops == 1 && !cancelWhileStarting.reports);

    Mock cancelBeforeStart = NewMock();
    cancelBeforeStart.stopDuringProbe = true;
    assert(Run(&cancelBeforeStart, MW_SECOND) == MWStopped);
    assert(!cancelBeforeStart.starts && !cancelBeforeStart.stops && !cancelBeforeStart.reports);
}

static void TestErrors(void) {
    Mock start = NewMock();
    start.startStatus = -1;
    assert(Run(&start, MW_SECOND) == MWStartFailed);
    assert(start.starts == 1 && !start.stops && !start.reports);

    Mock stop = NewMock();
    stop.stopStatus = -1;
    assert(Run(&stop, MW_SECOND) == MWStopFailed);
    assert(stop.starts == 1 && stop.stops == 1 && stop.reports == 1);
}

static Mock WithReadings(const MWDeviceState *readings, size_t count) {
    Mock mock = NewMock();
    mock.readings = readings;
    mock.readingCount = count;
    return mock;
}

static void TestRecoverableReads(void) {
    const uint64_t retry = MW_SECOND / 20, interval = MW_SECOND / 2;
    const MWDeviceState firstRead[] = { MWDeviceUnknown, MWDeviceAvailable };
    Mock startup = WithReadings(firstRead, 2);
    startup.cancelAt = startup.now + interval;
    assert(Run(&startup, MW_SECOND) == MWStopped);
    assert(startup.checks == 2 && startup.checkedAt[1] - startup.checkedAt[0] == retry);
    assert(startup.reportedAt == startup.checkedAt[1]);
    assert(startup.starts == 1 && startup.stops == 1 && startup.reports == 1);

    const MWDeviceState intermittent[] = { MWDeviceAvailable, MWDeviceUnknown, MWDeviceAvailable,
                                           MWDeviceUnknown, MWDeviceAvailable };
    Mock active = WithReadings(intermittent, 5);
    active.cancelAt = active.now + 3 * interval;
    assert(Run(&active, 2 * MW_SECOND) == MWStopped);
    assert(active.checks == 5 && active.starts == 1 && active.stops == 1 && active.reports == 1);
    assert(active.checkedAt[1] - active.checkedAt[0] == interval);
    assert(active.checkedAt[2] - active.checkedAt[1] == retry);
    assert(active.checkedAt[3] - active.checkedAt[2] == interval);
    assert(active.checkedAt[4] - active.checkedAt[3] == retry);
}

static void TestUnconfirmedOrChanged(void) {
    const MWDeviceState unknown[] = { MWDeviceAvailable, MWDeviceUnknown, MWDeviceUnknown };
    Mock active = WithReadings(unknown, 3);
    assert(Run(&active, 2 * MW_SECOND) == MWSessionQueryFailed);
    assert(active.checks == 3 && active.reports == 1 && active.starts == 1 && active.stops == 1);
    assert(active.now == active.checkedAt[2] && active.checkedAt[2] - active.checkedAt[1] == MW_SECOND / 20);

    Mock startup = WithReadings(unknown + 1, 2);
    assert(Run(&startup, MW_SECOND) == MWSessionQueryFailed);
    assert(startup.checks == 2 && !startup.reports && startup.starts == 1 && startup.stops == 1);

    Mock failedStop = WithReadings(unknown, 3);
    failedStop.stopStatus = -1;
    assert(Run(&failedStop, MW_SECOND) == MWStopFailed && failedStop.stops == 1);

    const MWDeviceState changed[] = { MWDeviceUnknown, MWDeviceUnavailable };
    Mock replaced = WithReadings(changed, 2);
    assert(Run(&replaced, MW_SECOND) == MWDeviceChanged);
    assert(replaced.checks == 2 && !replaced.reports && replaced.stops == 1);

    Mock unavailable = WithReadings(changed + 1, 1);
    assert(Run(&unavailable, MW_SECOND) == MWDeviceChanged);
    assert(unavailable.now == unavailable.checkedAt[0] && unavailable.checks == 1);
    assert(!unavailable.reports && unavailable.stops == 1);
}

static void TestInterruptionDuringRecheck(void) {
    const MWDeviceState unreadable[] = { MWDeviceUnknown, MWDeviceUnknown };
    Mock cancelled = WithReadings(unreadable, 2);
    cancelled.cancelAt = cancelled.now + MW_SECOND / 20;
    assert(Run(&cancelled, MW_SECOND) == MWStopped);
    assert(cancelled.checks == 1 && cancelled.stops == 1 && !cancelled.reports);

    Mock deadline = WithReadings(unreadable, 2);
    assert(Run(&deadline, MW_SECOND / 20) == MWExpired);
    assert(deadline.checks == 1 && deadline.stops == 1 && !deadline.reports);

    Mock duringRead = WithReadings(unreadable, 2);
    duringRead.interruptOnLastRead = MWTimeUp;
    assert(Run(&duringRead, MW_SECOND) == MWExpired);
    assert(duringRead.checks == 2 && duringRead.stops == 1 && !duringRead.reports);

    Mock duringReport = NewMock();
    duringReport.interruptDuringReport = MWControlError;
    assert(Run(&duringReport, MW_SECOND) == MWControlFailed && duringReport.stops == 1);
}

int main(void) {
    TestSelection();
    TestRepeatedStart();
    TestPairedStop();
    TestErrors();
    TestRecoverableReads();
    TestUnconfirmedOrChanged();
    TestInterruptionDuringRecheck();
    puts("Mock tests passed: device eligibility, balanced stops, and bounded session rechecks.");
    return 0;
}
