#include "warmup.h"

#include <assert.h>
#include <stdio.h>

typedef struct {
    MWInput input;
    uint64_t now, cancelAt, disconnectAt;
    MWInterrupt requested;
    int startStatus, stopStatus, starts, stops, reports;
    bool stopDuringProbe, stopDuringStart;
} Mock;

static Mock NewMock(void) {
    return (Mock){ .input = { MWInputOK, 17, MWBeforeRunning }, .now = MW_SECOND,
                   .cancelAt = UINT64_MAX, .disconnectAt = UINT64_MAX };
}

static MWInput Probe(void *ctx) {
    Mock *mock = ctx;
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
static bool StillAvailable(void *ctx, MWDeviceID id) {
    Mock *mock = ctx;
    return id == mock->input.id && mock->now < mock->disconnectAt;
}
static bool Report(void *ctx, MWBefore before) {
    Mock *mock = ctx;
    assert(mock->starts == 1 && mock->startStatus == 0 && mock->stops == 0);
    assert(mock->reports == 0 && before == mock->input.before);
    mock->reports++;
    return true;
}
static MWInterrupt Interrupt(void *ctx) {
    Mock *mock = ctx;
    return mock->now >= mock->cancelAt ? MWCancel : mock->requested;
}
static uint64_t Now(void *ctx) { return ((Mock *)ctx)->now; }
static void Pause(void *ctx, uint64_t duration) { ((Mock *)ctx)->now += duration; }

static MWOutcome Run(Mock *mock, uint64_t duration) {
    MWOps ops = { mock, Probe, Start, Stop, StillAvailable, Report, Interrupt, Now, Pause };
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
    mock.input.selection = MWInputQueryFailed;
    assert(Run(&mock, MW_SECOND) == MWQueryFailed && !mock.starts);
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

int main(void) {
    TestSelection();
    TestRepeatedStart();
    TestPairedStop();
    TestErrors();
    puts("Mock tests passed: device eligibility, duplicate starts, balanced stops, and failures.");
    return 0;
}
