#ifndef MIC_WARMUP_HAL_PROBE_H
#define MIC_WARMUP_HAL_PROBE_H

#include "warmup.h"

MWInput MWProbeBuiltInInput(bool includeRunningStatus);
// Check whether the session's original device is still the unique eligible input.
MWDeviceState MWCheckBuiltInInput(MWDeviceID id);

#endif
