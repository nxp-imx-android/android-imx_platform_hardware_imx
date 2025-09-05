/*
 * Copyright (C) 2025 The Android Open Source Project
 * Copyright 2025 NXP
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
#include <string>

#include "core-impl/Stream.h"

#include <tinyalsa/asoundlib.h>
#include <core-impl/AudioCardManager.h>

namespace aidl::android::hardware::audio::core {

#define MMAP_SAMPLE_RATE 48000

/* period size : period ms : buffer ms
         512   :   10.7    :   42.7
         256   :    5.3    :   21.3
*/
#define MMAP_PERIOD_SIZE 512
#define MMAP_PERIOD_MS (MMAP_PERIOD_SIZE * 1000 / MMAP_SAMPLE_RATE)
#define MMAP_PERIOD_COUNT 4
#define MMAP_BUFFER_MS (MMAP_PERIOD_MS * MMAP_PERIOD_COUNT)
#define MMAP_BUFFER_SIZE (MMAP_PERIOD_SIZE * MMAP_PERIOD_COUNT)

class StreamMmap : public StreamCommonImpl {
  public:
    static const std::string kCreateMmapBufferName;

    StreamMmap(StreamContext* context, const Metadata& metadata);
    ~StreamMmap();

    // Methods of 'DriverInterface'.
    ::android::status_t init(DriverCallbackInterface*) override;
    ::android::status_t drain(StreamDescriptor::DrainMode) override;
    ::android::status_t flush() override;
    ::android::status_t pause() override;
    ::android::status_t standby() override;
    ::android::status_t start() override;
    ::android::status_t transfer(void* buffer, size_t frameCount, size_t* actualFrameCount,
                                 int32_t* latencyMs) override;

    void shutdown() override;
    void stop();

    ndk::ScopedAStatus getVendorParameters(const std::vector<std::string>& in_ids,
                                           std::vector<VendorParameter>* _aidl_return) override;
    ndk::ScopedAStatus setVendorParameters(const std::vector<VendorParameter>& in_parameters,
                                           bool in_async) override;

    ::android::status_t refinePosition(StreamDescriptor::Position* position) override;
    ::android::status_t getMmapPositionAndLatency(StreamDescriptor::Position* position,
                                                  int32_t* latency) override;

  private:
    ndk::ScopedAStatus createMmapBuffer(MmapBufferDescriptor* desc);

  private:
    bool mIsInput;
    bool mIsStarted = false;
    struct pcm *mPcm = nullptr;
    struct pcm* openPcm();
    void closePcm();
    pthread_t mThreadId = 0;
    bool mThreadRun = false;
    static void* _threadLoop(void*);
    void* threadLoop();
    StreamDescriptor::Position mPosition;
    int64_t mTotalFrames = 0;
};

class StreamInMmap final : public StreamIn, public StreamMmap {
  public:
    friend class ndk::SharedRefBase;
    StreamInMmap(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SinkMetadata& sinkMetadata,
            const std::vector<::aidl::android::media::audio::common::MicrophoneInfo>& microphones);

  private:
    void onClose(StreamDescriptor::State) override { defaultOnClose(); }
};

class StreamOutMmap final : public StreamOut, public StreamMmap {
  public:
    friend class ndk::SharedRefBase;
    StreamOutMmap(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SourceMetadata& sourceMetadata,
            const std::optional<::aidl::android::media::audio::common::AudioOffloadInfo>&
                    offloadInfo);

  private:
    void onClose(StreamDescriptor::State) override { defaultOnClose(); }
};

}  // namespace aidl::android::hardware::audio::core
