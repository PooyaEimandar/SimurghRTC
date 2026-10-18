// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "modules/audio_device/include/audio_device_default.h"

/** No hardware, capture thread or audio callbacks. Generated PCM/sinks are a separate SR-004 probe. */
class FSimurghRTCHeadlessAudioDevice : public webrtc::webrtc_impl::AudioDeviceModuleDefault<webrtc::AudioDeviceModule>
{
public:
    int32_t Init() override { bInitialized = true; return 0; }
    int32_t Terminate() override { bInitialized = false; return 0; }
    bool Initialized() const override { return bInitialized; }
    int32_t ActiveAudioLayer(AudioLayer* Layer) const override { *Layer = kDummyAudio; return 0; }
    int32_t PlayoutIsAvailable(bool* Value) override { *Value = false; return 0; }
    int32_t RecordingIsAvailable(bool* Value) override { *Value = false; return 0; }
    int32_t SpeakerVolumeIsAvailable(bool* Value) override { *Value = false; return 0; }
    int32_t MicrophoneVolumeIsAvailable(bool* Value) override { *Value = false; return 0; }
    int32_t SpeakerMuteIsAvailable(bool* Value) override { *Value = false; return 0; }
    int32_t MicrophoneMuteIsAvailable(bool* Value) override { *Value = false; return 0; }
    int32_t SpeakerMute(bool* Value) const override { *Value = true; return 0; }
    int32_t MicrophoneMute(bool* Value) const override { *Value = true; return 0; }
    int32_t StereoPlayout(bool* Value) const override { *Value = false; return 0; }
    int32_t StereoRecording(bool* Value) const override { *Value = false; return 0; }
    int32_t SpeakerVolume(uint32_t* Value) const override { *Value = 0; return 0; }
    int32_t MaxSpeakerVolume(uint32_t* Value) const override { *Value = 0; return 0; }
    int32_t MinSpeakerVolume(uint32_t* Value) const override { *Value = 0; return 0; }
    int32_t MicrophoneVolume(uint32_t* Value) const override { *Value = 0; return 0; }
    int32_t MaxMicrophoneVolume(uint32_t* Value) const override { *Value = 0; return 0; }
    int32_t MinMicrophoneVolume(uint32_t* Value) const override { *Value = 0; return 0; }
    int32_t PlayoutDeviceName(uint16_t, char* Name, char* Guid) override
    { Name[0] = 0; if (Guid) Guid[0] = 0; return -1; }
    int32_t RecordingDeviceName(uint16_t, char* Name, char* Guid) override
    { Name[0] = 0; if (Guid) Guid[0] = 0; return -1; }
private:
    bool bInitialized = false;
};
