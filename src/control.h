#ifndef MIC_WARMUP_CONTROL_H
#define MIC_WARMUP_CONTROL_H

#include "warmup.h"

#include <stdatomic.h>

enum { MW_CONTROL_FRAME_SIZE = 9, MW_CONTROL_RENEW = 'R', MW_CONTROL_ACK = 'A',
       MW_CONTROL_CANCEL = 'C' };

#define MW_START_LIMIT (15 * MW_SECOND)

static inline void MWControlEncode(unsigned char out[MW_CONTROL_FRAME_SIZE],
                                   unsigned char tag, uint64_t deadline) {
    out[0] = tag;
    for (size_t i = 1; i < MW_CONTROL_FRAME_SIZE; i++, deadline >>= 8) {
        out[i] = (unsigned char)deadline;
    }
}

static inline uint64_t MWControlDecode(const unsigned char frame[MW_CONTROL_FRAME_SIZE]) {
    uint64_t deadline = 0;
    for (size_t i = 1; i < MW_CONTROL_FRAME_SIZE; i++) {
        deadline |= (uint64_t)frame[i] << (8 * (i - 1));
    }
    return deadline;
}

typedef struct {
    // Only the supervisor changes the lease. Both threads can commit a stop.
    atomic_int state;
    uint64_t deadline;
    uint64_t startupAt; // Set once before the audio worker starts.
    uint64_t watchdogAt;
    uint64_t audioStopAt;
    unsigned char partial[MW_CONTROL_FRAME_SIZE];
    size_t used;
} MWControl;

typedef bool (*MWControlAck)(void *ctx, uint64_t deadline);

// Only the supervisor may initialize, advance, or inspect the next check time.
bool MWControlInit(MWControl *control, uint64_t now, uint64_t deadline);
bool MWControlBeginStartup(MWControl *control, uint64_t now);
uint64_t MWControlNextCheck(const MWControl *control);
// Present all immediately available bytes together, with any EOF, owner or signal stop.
// Returns zero to keep supervising, or the required helper exit code.
int MWControlAdvance(MWControl *control, uint64_t now, MWInterrupt outside,
                     const unsigned char *bytes, size_t size, MWControlAck ack, void *ctx);
// After joining the worker, retain the reason for a confirmed clean stop.
MWOutcome MWControlCompletedOutcome(const MWControl *control, MWOutcome outcome);

// These operations may run on the audio thread. The audio thread never reads the pipe.
MWInterrupt MWControlInterruption(const MWControl *control);
MWInterrupt MWControlAudioInterruption(MWControl *control, uint64_t now);
// Disarm the startup watchdog only after the active status byte was written.
bool MWControlAudioReady(MWControl *control, uint64_t now);
void MWControlAudioStopping(MWControl *control, uint64_t now);

#endif
