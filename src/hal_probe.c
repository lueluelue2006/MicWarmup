#include "hal_probe.h"

#include <CoreAudio/CoreAudio.h>

static OSStatus ReadUInt32(AudioObjectID object, AudioObjectPropertySelector selector, UInt32 *value) {
    AudioObjectPropertyAddress property = { selector, kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain };
    UInt32 size = sizeof(*value);
    OSStatus status = AudioObjectGetPropertyData(object, &property, 0, NULL, &size, value);
    if (status != noErr) return status;
    return size == sizeof(*value) ? noErr : kAudioHardwareBadPropertySizeError;
}

static bool MissingDevice(OSStatus status) {
    return status == kAudioHardwareBadDeviceError || status == kAudioHardwareBadObjectError;
}

enum { DeviceCapacity = 256 };
typedef struct {
    AudioObjectID ids[DeviceCapacity];
    size_t count;
} DeviceList;

static bool ReadDevices(DeviceList *list) {
    const AudioObjectPropertyAddress devices = { kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 bytes;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devices, 0, NULL, &bytes) != noErr ||
        bytes > sizeof(list->ids) || bytes % sizeof(list->ids[0]) != 0) return false;
    list->count = bytes / sizeof(list->ids[0]);
    if (!bytes) return true;
    UInt32 returned = bytes;
    return AudioObjectGetPropertyData(kAudioObjectSystemObject, &devices, 0, NULL, &returned,
                                      list->ids) == noErr && returned == bytes;
}

static MWDeviceState ReadCandidate(AudioObjectID id, MWCandidate *candidate, bool beforeStart) {
    *candidate = (MWCandidate){ .id = id };
    if (id == kAudioObjectUnknown) return MWDeviceUnknown;
    UInt32 transport;
    OSStatus status = ReadUInt32(id, kAudioDevicePropertyTransportType, &transport);
    if (beforeStart && status != noErr) return MWDeviceUnknown;
    if (MissingDevice(status)) return MWDeviceUnavailable;
    bool transportKnown = status == noErr && transport != kAudioDeviceTransportTypeUnknown;
    candidate->builtIn = transportKnown && transport == kAudioDeviceTransportTypeBuiltIn;
    if (transportKnown && !candidate->builtIn) return MWDeviceUnavailable;
    // A dead or input-less device can be ruled out even when its transport is unspecified.
    UInt32 alive;
    status = ReadUInt32(id, kAudioDevicePropertyDeviceIsAlive, &alive);
    bool aliveKnown = status == noErr && alive <= 1;
    if (!aliveKnown && beforeStart) return MWDeviceUnknown;
    if (MissingDevice(status)) return MWDeviceUnavailable;
    candidate->alive = aliveKnown && alive;
    if (aliveKnown && !candidate->alive) return MWDeviceUnavailable;

    const AudioObjectPropertyAddress streams = { kAudioDevicePropertyStreams,
        kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMain };
    UInt32 streamSize;
    status = AudioObjectGetPropertyDataSize(id, &streams, 0, NULL, &streamSize);
    if (!beforeStart && MissingDevice(status)) return MWDeviceUnavailable;
    if (status != noErr || streamSize % sizeof(AudioStreamID)) return MWDeviceUnknown;
    candidate->hasInput = streamSize > 0;
    if (!candidate->hasInput) return MWDeviceUnavailable;
    return transportKnown && aliveKnown ? MWDeviceAvailable : MWDeviceUnknown;
}

MWInput MWProbeBuiltInInput(bool includeRunningStatus) {
    const MWInput failed = { MWInputQueryFailed, 0, MWBeforeUnknown };
    DeviceList list;
    if (!ReadDevices(&list)) return failed;
    MWCandidate candidates[DeviceCapacity];
    for (size_t i = 0; i < list.count; i++) {
        if (ReadCandidate(list.ids[i], &candidates[i], true) == MWDeviceUnknown) return failed;
    }
    MWInput result = MWSelect(candidates, list.count);
    UInt32 running;
    if (result.selection == MWInputOK && includeRunningStatus &&
        ReadUInt32(result.id, kAudioDevicePropertyDeviceIsRunningSomewhere, &running) == noErr && running <= 1) {
        result.before = running ? MWBeforeRunning : MWBeforeStopped;
    }
    return result;
}

MWDeviceState MWCheckBuiltInInput(MWDeviceID id) {
    DeviceList list;
    if (!ReadDevices(&list)) return MWDeviceUnknown;
    bool listed = false, missingID = false;
    for (size_t i = 0; i < list.count; i++) {
        if (list.ids[i] == id) listed = true;
        if (list.ids[i] == kAudioObjectUnknown) missingID = true;
    }
    if (!listed && !missingID) return MWDeviceUnavailable;

    MWCandidate candidate;
    MWDeviceState state = listed ? ReadCandidate(id, &candidate, false) : MWDeviceUnknown;
    if (state == MWDeviceUnavailable) return state;
    for (size_t i = 0; i < list.count; i++) {
        if (list.ids[i] == id) continue;
        MWDeviceState other = ReadCandidate(list.ids[i], &candidate, false);
        // A different eligible input rules out the original as the unique choice.
        if (other == MWDeviceAvailable) return MWDeviceUnavailable;
        if (other == MWDeviceUnknown) state = MWDeviceUnknown;
    }
    return state;
}
