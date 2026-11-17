#include "hal_probe.h"

#include <CoreAudio/CoreAudio.h>

static bool ReadUInt32(AudioObjectID object, AudioObjectPropertySelector selector, UInt32 *value) {
    AudioObjectPropertyAddress property = { selector, kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain };
    UInt32 size = sizeof(*value);
    return AudioObjectGetPropertyData(object, &property, 0, NULL, &size, value) == noErr &&
           size == sizeof(*value);
}

MWInput MWProbeBuiltInInput(bool includeRunningStatus) {
    const MWInput failed = { MWInputQueryFailed, 0, MWBeforeUnknown };
    const AudioObjectPropertyAddress devices = { kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    const AudioObjectPropertyAddress streams = { kAudioDevicePropertyStreams,
        kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMain };
    AudioObjectID ids[256];
    UInt32 bytes;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devices, 0, NULL, &bytes) != noErr ||
        bytes > sizeof(ids) || bytes % sizeof(ids[0]) != 0) return failed;
    if (bytes == 0) return MWSelect(NULL, 0);
    UInt32 returned = bytes;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &devices, 0, NULL, &returned, ids) != noErr ||
        returned != bytes) return failed;
    size_t count = bytes / sizeof(ids[0]);
    MWCandidate candidates[256] = {0};
    for (size_t i = 0; i < count; i++) {
        AudioObjectID id = ids[i];
        if (id == kAudioObjectUnknown) return failed;
        candidates[i].id = id;
        UInt32 transport;
        if (!ReadUInt32(id, kAudioDevicePropertyTransportType, &transport)) return failed;
        if (transport != kAudioDeviceTransportTypeBuiltIn) continue;
        candidates[i].builtIn = true;
        UInt32 alive;
        if (!ReadUInt32(id, kAudioDevicePropertyDeviceIsAlive, &alive)) return failed;
        if (!alive) continue;
        candidates[i].alive = true;
        UInt32 streamSize;
        if (AudioObjectGetPropertyDataSize(id, &streams, 0, NULL, &streamSize) != noErr ||
            streamSize % sizeof(AudioStreamID) != 0) return failed;
        candidates[i].hasInput = streamSize > 0;
    }
    MWInput result = MWSelect(candidates, count);
    UInt32 running;
    if (result.selection == MWInputOK && includeRunningStatus &&
        ReadUInt32(result.id, kAudioDevicePropertyDeviceIsRunningSomewhere, &running) && running <= 1) {
        result.before = running ? MWBeforeRunning : MWBeforeStopped;
    }
    return result;
}
