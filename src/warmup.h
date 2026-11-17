#ifndef MIC_WARMUP_H
#define MIC_WARMUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MW_SECOND UINT64_C(1000000000)
#define MW_LIMIT (120 * MW_SECOND)
#define MW_STOP_GRACE MW_SECOND

typedef uint32_t MWDeviceID;
typedef enum { MWInputOK, MWInputNone, MWInputAmbiguous, MWInputQueryFailed } MWSelection;
typedef enum { MWDeviceUnknown, MWDeviceAvailable, MWDeviceUnavailable } MWDeviceState;
typedef enum { MWBeforeUnknown = 'U', MWBeforeStopped = 'I', MWBeforeRunning = 'R' } MWBefore;

typedef struct {
    MWDeviceID id;
    bool builtIn, alive, hasInput;
} MWCandidate;

typedef struct {
    MWSelection selection;
    MWDeviceID id;
    MWBefore before;
} MWInput;

MWInput MWSelect(const MWCandidate *candidates, size_t count);
uint64_t MWNow(void);

typedef enum { MWIdle, MWStarting, MWActive, MWStopping } MWPhase;
bool MWBegin(MWPhase *phase);
bool MWMarkActive(MWPhase *phase);
bool MWEnd(MWPhase *phase);

typedef enum {
    MWStopped = 0, MWExpired = 20, MWDeviceChanged = 21, MWNoDevice = 22,
    MWMultipleDevices = 23, MWQueryFailed = 24, MWStartFailed = 25,
    MWStopFailed = 26, MWWatchdog = 27, MWControlFailed = 28,
    MWSessionQueryFailed = 29, MWStartTimedOut = 30, MWStartTimedOutStopped = 31,
} MWOutcome;

typedef enum { MWContinue, MWCancel, MWTimeUp, MWControlError } MWInterrupt;

typedef struct {
    void *ctx;
    MWInput (*probe)(void *ctx);
    int32_t (*start)(void *ctx, MWDeviceID id);
    int32_t (*stop)(void *ctx, MWDeviceID id);
    MWDeviceState (*deviceState)(void *ctx, MWDeviceID id);
    bool (*reportActive)(void *ctx, MWBefore before);
    MWInterrupt (*interruption)(void *ctx);
    uint64_t (*now)(void *ctx);
    void (*pause)(void *ctx, uint64_t duration);
} MWOps;

MWOutcome MWRun(const MWOps *ops, uint64_t stopAt);
MWOutcome MWRunSupervised(const MWOps *ops);

#endif
