#ifndef MIC_WARMUP_CONTROL_H
#define MIC_WARMUP_CONTROL_H

#include "warmup.h"

#include <stdatomic.h>

enum { MW_CONTROL_FRAME_SIZE = 9, MW_CONTROL_RENEW = 'R', MW_CONTROL_ACK = 'A',
       MW_CONTROL_CANCEL = 'C' };

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
    // The active deadline and committed stop reason share one atomic value.
    _Atomic(uint64_t) state;
    uint64_t watchdogAt;
    uint64_t audioStopAt;
    unsigned char partial[MW_CONTROL_FRAME_SIZE];
    size_t used;
} MWControl;

typedef bool (*MWControlAck)(void *ctx, uint64_t deadline);

// Only the supervisor may initialize, advance, or inspect the next check time.
bool MWControlInit(MWControl *control, uint64_t now, uint64_t deadline);
uint64_t MWControlNextCheck(const MWControl *control);
// Present all immediately available bytes together, with any EOF, owner or signal stop.
// Returns false if the watchdog requires a process exit.
bool MWControlAdvance(MWControl *control, uint64_t now, MWInterrupt outside,
                      const unsigned char *bytes, size_t size, MWControlAck ack, void *ctx);

// These operations may run on the audio thread. The audio thread never reads the pipe.
MWInterrupt MWControlInterruption(const MWControl *control);
void MWControlAudioStopping(MWControl *control, uint64_t now);

#endif
