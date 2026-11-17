#include "hal_probe.h"

#include <CoreAudio/CoreAudio.h>
#include <assert.h>
#include <stdio.h>

enum { Mic = 17, Second = 23 };
typedef enum { ValidList, SizeError, DataError, Misaligned, Oversized, SizeChanged } ListRead;
typedef struct {
    AudioObjectID id;
    UInt32 transport, alive, inputBytes;
    AudioObjectPropertySelector unreadable, shortRead;
} Device;
static Device devices[3];
static size_t count;
static ListRead listRead;
static OSStatus propertyError;

static Device BuiltIn(AudioObjectID id) {
    return (Device){ .id = id, .transport = kAudioDeviceTransportTypeBuiltIn, .alive = 1,
                     .inputBytes = sizeof(AudioStreamID) };
}

static void Reset(void) {
    count = 1;
    devices[0] = BuiltIn(Mic);
    listRead = ValidList;
    propertyError = kAudioHardwareUnspecifiedError;
}

static Device *Lookup(AudioObjectID id) {
    for (size_t i = 0; i < count; i++) {
        if (devices[i].id == id) return &devices[i];
    }
    return NULL;
}

OSStatus AudioObjectGetPropertyDataSize(AudioObjectID id, const AudioObjectPropertyAddress *address,
                                        UInt32 qualifierSize, const void *qualifier, UInt32 *size) {
    assert(!qualifierSize && !qualifier);
    if (id == kAudioObjectSystemObject) {
        assert(address->mSelector == kAudioHardwarePropertyDevices);
        if (listRead == SizeError) return kAudioHardwareUnspecifiedError;
        *size = listRead == Oversized ? UINT32_MAX - 3 : (UInt32)(count * sizeof(AudioObjectID));
        if (listRead == Misaligned) (*size)++;
        return noErr;
    }
    assert(address->mSelector == kAudioDevicePropertyStreams &&
           address->mScope == kAudioDevicePropertyScopeInput);
    Device *device = Lookup(id);
    if (!device || device->unreadable == address->mSelector) return propertyError;
    *size = device->inputBytes;
    return noErr;
}

OSStatus AudioObjectGetPropertyData(AudioObjectID id, const AudioObjectPropertyAddress *address,
                                    UInt32 qualifierSize, const void *qualifier, UInt32 *size, void *out) {
    assert(!qualifierSize && !qualifier);
    if (id == kAudioObjectSystemObject) {
        assert(address->mSelector == kAudioHardwarePropertyDevices && *size == count * sizeof(AudioObjectID));
        if (listRead == DataError) return kAudioHardwareUnspecifiedError;
        for (size_t i = 0; i < count; i++) ((AudioObjectID *)out)[i] = devices[i].id;
        if (listRead == SizeChanged) *size -= sizeof(AudioObjectID);
        return noErr;
    }
    assert(*size == sizeof(UInt32));
    Device *device = Lookup(id);
    if (!device || device->unreadable == address->mSelector) return propertyError;
    switch (address->mSelector) {
        case kAudioDevicePropertyTransportType: *(UInt32 *)out = device->transport; break;
        case kAudioDevicePropertyDeviceIsAlive: *(UInt32 *)out = device->alive; break;
        case kAudioDevicePropertyDeviceIsRunningSomewhere: *(UInt32 *)out = 1; break;
        default: assert(false);
    }
    if (device->shortRead == address->mSelector) (*size)--;
    return noErr;
}

static void TestListReads(void) {
    Reset();
    assert(MWProbeBuiltInInput(true).before == MWBeforeRunning && MWCheckBuiltInInput(Mic) == MWDeviceAvailable);
    const ListRead failures[] = { SizeError, DataError, Misaligned, Oversized, SizeChanged };
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        listRead = failures[i];
        assert(MWProbeBuiltInInput(true).selection == MWInputQueryFailed);
        assert(MWCheckBuiltInInput(Mic) == MWDeviceUnknown);
    }
    listRead = ValidList;
    devices[0].id = kAudioObjectUnknown;
    assert(MWCheckBuiltInInput(Mic) == MWDeviceUnknown);
    count = 0;
    assert(MWProbeBuiltInInput(true).selection == MWInputNone);
    assert(MWCheckBuiltInInput(Mic) == MWDeviceUnavailable);
    devices[count++] = BuiltIn(Second);
    devices[0].unreadable = kAudioDevicePropertyTransportType;
    assert(MWCheckBuiltInInput(Mic) == MWDeviceUnavailable); // Original ID is definitely gone.
}

static void TestEligibility(void) {
    const AudioObjectPropertySelector required[] = { kAudioDevicePropertyTransportType,
        kAudioDevicePropertyDeviceIsAlive, kAudioDevicePropertyStreams };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        Reset();
        devices[0].unreadable = required[i];
        assert(MWProbeBuiltInInput(true).selection == MWInputQueryFailed);
        assert(MWCheckBuiltInInput(Mic) == MWDeviceUnknown);
        propertyError = kAudioHardwareBadDeviceError;
        assert(MWProbeBuiltInInput(true).selection == MWInputQueryFailed);
        assert(MWCheckBuiltInInput(Mic) == MWDeviceUnavailable);
        propertyError = kAudioHardwareBadObjectError;
        assert(MWCheckBuiltInInput(Mic) == MWDeviceUnavailable);
    }
    Reset();
    devices[0].shortRead = kAudioDevicePropertyTransportType;
    assert(MWCheckBuiltInInput(Mic) == MWDeviceUnknown);
    devices[0].alive = 0;
    assert(MWCheckBuiltInInput(Mic) == MWDeviceUnavailable);
    Reset();
    devices[0].alive = 2;
    assert(MWCheckBuiltInInput(Mic) == MWDeviceUnknown);
    devices[0].inputBytes = 0;
    assert(MWCheckBuiltInInput(Mic) == MWDeviceUnavailable);
    Reset();
    devices[0].inputBytes = 1;
    assert(MWCheckBuiltInInput(Mic) == MWDeviceUnknown);
    Reset();
    devices[0].transport = kAudioDeviceTransportTypeUSB;
    assert(MWCheckBuiltInInput(Mic) == MWDeviceUnavailable);
    devices[0].transport = kAudioDeviceTransportTypeUnknown;
    assert(MWCheckBuiltInInput(Mic) == MWDeviceUnknown);
    devices[0].inputBytes = 0;
    assert(MWCheckBuiltInInput(Mic) == MWDeviceUnavailable);
    Reset();
    devices[0].unreadable = kAudioDevicePropertyDeviceIsRunningSomewhere;
    MWInput initial = MWProbeBuiltInInput(true);
    assert(initial.selection == MWInputOK && initial.before == MWBeforeUnknown);
    assert(MWCheckBuiltInInput(Mic) == MWDeviceAvailable);
}

static void TestOtherDevices(void) {
    Reset();
    devices[count++] = BuiltIn(Second);
    assert(MWProbeBuiltInInput(false).selection == MWInputAmbiguous);
    assert(MWCheckBuiltInInput(Mic) == MWDeviceUnavailable);
    devices[1].transport = kAudioDeviceTransportTypeBluetooth;
    assert(MWCheckBuiltInInput(Mic) == MWDeviceAvailable);
    devices[1].transport = kAudioDeviceTransportTypeUnknown;
    assert(MWCheckBuiltInInput(Mic) == MWDeviceUnknown);
    devices[1].inputBytes = 0;
    assert(MWCheckBuiltInInput(Mic) == MWDeviceAvailable);
    devices[1].inputBytes = sizeof(AudioStreamID);
    devices[count++] = BuiltIn(42);
    assert(MWCheckBuiltInInput(Mic) == MWDeviceUnavailable); // Two confirmed built-ins plus an unknown.
    devices[0].unreadable = kAudioDevicePropertyDeviceIsAlive;
    assert(MWCheckBuiltInInput(Mic) == MWDeviceUnavailable); // A different choice is definitely eligible.
    devices[0].transport = kAudioDeviceTransportTypeUSB;
    count = 2;
    assert(MWCheckBuiltInInput(Mic) == MWDeviceUnavailable); // An unreadable peer cannot hide this change.
}

int main(void) {
    TestListReads();
    TestEligibility();
    TestOtherDevices();
    puts("Fake HAL tests passed: list failures, indeterminate properties, and confirmed device changes.");
    return 0;
}
