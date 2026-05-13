/*
 * Copyright 2024-2026 NXP
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <mutex>
#include <vector>

#include <android-base/thread_annotations.h>

#include "core-impl/StreamPrimary.h"
#include "core-impl/AudioCardManager.h"
#include <audio_utils/resampler.h>
#include <cutils/properties.h>

namespace aidl::android::hardware::audio::core {

#define DSD_BUFFER_SIZE 262144

class StreamPrimaryImx : public StreamPrimary {
  public:
    StreamPrimaryImx(StreamContext* context, const Metadata& metadata);

    ::android::status_t flush() override;
    ::android::status_t pause() override;
    ::android::status_t standby() override;
    ::android::status_t start() override;
    ::android::status_t transfer(void* buffer, size_t frameCount, size_t* actualFrameCount,
                                 int32_t* latencyMs) override;
    ::android::status_t refinePosition(StreamDescriptor::Position* position) override;
    void shutdown() override;

    // Overridden methods of 'StreamCommonImpl', called on a Binder thread.
    ndk::ScopedAStatus setConnectedDevices(const ConnectedDevices& devices) override;

  protected:
    std::vector<alsa::DeviceProfile> getDeviceProfiles() override;

    void tryStart();
    void stop();

    char soc_name[PROPERTY_VALUE_MAX];
    int16_t mStartRetryCount = 0;
    const int16_t kMaxStartRetryCount = 8;
    bool mIsStereoToMono = false;
    bool mIsS32ToS16 = false;
    bool mIsS16ToS24 = false;
    bool mHardwarePause = false;
    bool mStarted = false;
    bool mPrimaryOutput = false;
    bool mDirectOutput = false;
    bool mFirstTransfer = false;
    bool mConvertChannel = false;
    struct audio_card *mCard = NULL;
    struct resampler_itfe *mResampler = NULL;
    int16_t *mResamplerBuffer = NULL;
    std::optional<struct pcm_config> mSavedConfig;
};

class StreamInPrimaryImx final : public StreamIn, public StreamPrimaryImx, public StreamInHwGainHelper {
  public:
    friend class ndk::SharedRefBase;
    StreamInPrimaryImx(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SinkMetadata& sinkMetadata,
            const std::vector<::aidl::android::media::audio::common::MicrophoneInfo>& microphones);

  private:
    void onClose(StreamDescriptor::State) override { defaultOnClose(); }
};

class StreamOutPrimaryImx final : public StreamOut,
                               public StreamPrimaryImx,
                               public StreamOutHwVolumeHelper {
  public:
    friend class ndk::SharedRefBase;
    StreamOutPrimaryImx(StreamContext&& context,
                     const ::aidl::android::hardware::audio::common::SourceMetadata& sourceMetadata,
                     const std::optional<::aidl::android::media::audio::common::AudioOffloadInfo>&
                             offloadInfo);

  private:
    void onClose(StreamDescriptor::State) override { defaultOnClose(); }
};

}  // namespace aidl::android::hardware::audio::core
