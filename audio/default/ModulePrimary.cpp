/*
 * Copyright (C) 2023 The Android Open Source Project
 * Copyright 2024-2025 NXP
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

#include <vector>

#define LOG_TAG "AHAL_ModulePrimary"
#include <Utils.h>
#include <android-base/logging.h>

#include "core-impl/ModulePrimary.h"
#include "core-impl/StreamMmapStub.h"
#include "core-impl/StreamOffloadStub.h"
#include "core-impl/StreamPrimary.h"
#include "core-impl/Telephony.h"

#include <media/stagefright/foundation/MediaDefs.h>
#include "core-impl/AudioCardManager.h"
#include "core-impl/StreamCompress.h"
#include "core-impl/StreamMmap.h"

using aidl::android::hardware::audio::common::areAllBitPositionFlagsSet;
using aidl::android::hardware::audio::common::hasMmapFlag;
using aidl::android::hardware::audio::common::SinkMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::hardware::audio::core::StreamDescriptor;
using aidl::android::media::audio::common::AudioInputFlags;
using aidl::android::media::audio::common::AudioIoFlags;
using aidl::android::media::audio::common::AudioOffloadInfo;
using aidl::android::media::audio::common::AudioOutputFlags;
using aidl::android::media::audio::common::AudioPort;
using aidl::android::media::audio::common::AudioPortConfig;
using aidl::android::media::audio::common::AudioPortExt;
using aidl::android::media::audio::common::MicrophoneInfo;

namespace aidl::android::hardware::audio::core {

ModulePrimary::ModulePrimary(std::unique_ptr<Configuration>&& config)
    : Module(Type::DEFAULT, std::move(config)) {
    AudioCardManager::init();
}

ModulePrimary::~ModulePrimary() {
    AudioCardManager::release();
}

ndk::ScopedAStatus ModulePrimary::getTelephony(std::shared_ptr<ITelephony>* _aidl_return) {
    if (!mTelephony) {
        mTelephony = ndk::SharedRefBase::make<Telephony>();
    }
    *_aidl_return = mTelephony.getInstance();
    LOG(DEBUG) << __func__
               << ": returning instance of ITelephony: " << _aidl_return->get()->asBinder().get();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ModulePrimary::calculateBufferSizeFrames(
        const ::aidl::android::media::audio::common::AudioFormatDescription& format,
        int32_t latencyMs, int32_t sampleRateHz, int32_t* bufferSizeFrames) {
    if (format.type != ::aidl::android::media::audio::common::AudioFormatType::PCM &&
        format.encoding == ::android::MEDIA_MIMETYPE_AUDIO_MPEG) {
        *bufferSizeFrames = COMPRESS_OFFLOAD_BUFFER_SIZE;
        return ndk::ScopedAStatus::ok();
    }

    if (format.type != ::aidl::android::media::audio::common::AudioFormatType::PCM &&
        format.encoding == "audio/vnd.sony.dsd") {
        *bufferSizeFrames = DSD_BUFFER_SIZE;
        return ndk::ScopedAStatus::ok();
    }

    if (format.type != ::aidl::android::media::audio::common::AudioFormatType::PCM &&
        StreamOffloadStub::getSupportedEncodings().count(format.encoding)) {
        *bufferSizeFrames = sampleRateHz / 2;  // 1/2 of a second.
        return ndk::ScopedAStatus::ok();
    }
    return Module::calculateBufferSizeFrames(format, latencyMs, sampleRateHz, bufferSizeFrames);
}

ndk::ScopedAStatus ModulePrimary::createInputStream(StreamContext&& context,
                                                    const SinkMetadata& sinkMetadata,
                                                    const std::vector<MicrophoneInfo>& microphones,
                                                    std::shared_ptr<StreamIn>* result) {
    if (context.isMmap()) {
        return createStreamInstance<StreamInMmap>(result, std::move(context), sinkMetadata,
                                                      microphones);
    }
    return createStreamInstance<StreamInPrimary>(result, std::move(context), sinkMetadata,
                                                 microphones);
}

ndk::ScopedAStatus ModulePrimary::createOutputStream(
        StreamContext&& context, const SourceMetadata& sourceMetadata,
        const std::optional<AudioOffloadInfo>& offloadInfo, std::shared_ptr<StreamOut>* result) {
    if (context.isMmap()) {
        return createStreamInstance<StreamOutMmap>(result, std::move(context), sourceMetadata,
                                                       offloadInfo);
    } else if (areAllBitPositionFlagsSet(
                       context.getFlags().get<AudioIoFlags::output>(),
                       {AudioOutputFlags::COMPRESS_OFFLOAD, AudioOutputFlags::NON_BLOCKING})) {
        // "Stub" is used because there is no actual decoder. The stream just
        // extracts the clip duration from the media file header and simulates
        // playback over time.
        return createStreamInstance<StreamOutOffloadStub>(result, std::move(context),
                                                          sourceMetadata, offloadInfo);
    }

    if (context.getFormat().encoding == ::android::MEDIA_MIMETYPE_AUDIO_MPEG) {
        const auto& c = AudioCardManager::getCardForDevice(AUDIO_DEVICE_OUT_LINE);
        if (c && strstr(c->card_name, "sof")) {
            return createStreamInstance<StreamOutCompress>(result, std::move(context), sourceMetadata, offloadInfo);
        } else {
            LOG(INFO) << "reject creating compress offload stream.";
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }
    }

    return createStreamInstance<StreamOutPrimary>(result, std::move(context), sourceMetadata,
                                                  offloadInfo);
}

ndk::ScopedAStatus ModulePrimary::createMmapBuffer(const AudioPortConfig& portConfig,
                                                   int32_t bufferSizeFrames, int32_t frameSizeBytes,
                                                   MmapBufferDescriptor* desc) {
    bufferSizeFrames = MMAP_BUFFER_SIZE;
    const size_t bufferSizeBytes = static_cast<size_t>(bufferSizeFrames) * frameSizeBytes;
    // The actual mmap buffer for I/O is created after the stream exits standby, via
    // 'IStreamCommon.createMmapBuffer'. But we must return a valid file descriptor here because
    // 'MmapBufferDescriptor' can not contain a "null" fd.
    const std::string regionName =
            std::string("mmap-sim-o-") +
            std::to_string(portConfig.ext.get<AudioPortExt::Tag::mix>().handle);
    int fd = ashmem_create_region(regionName.c_str(), bufferSizeBytes);
    if (fd < 0) {
        PLOG(ERROR) << __func__ << ": failed to create shared memory region of " << bufferSizeBytes
                    << " bytes";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    desc->sharedMemory.fd = ndk::ScopedFileDescriptor(fd);
    desc->sharedMemory.size = bufferSizeBytes;
    desc->burstSizeFrames = bufferSizeFrames / 4;
    desc->flags = 1 << MmapBufferDescriptor::FLAG_INDEX_APPLICATION_SHAREABLE;
    LOG(DEBUG) << __func__ << ": " << desc->toString();
    return ndk::ScopedAStatus::ok();
}

int32_t ModulePrimary::getNominalLatencyMs(const AudioPortConfig& portConfig) {
    static constexpr int32_t kLowLatencyMs = MMAP_BUFFER_MS;
    static constexpr int32_t kStandardLatencyMs = 16;
    return hasMmapFlag(portConfig.flags.value()) ? kLowLatencyMs : kStandardLatencyMs;
}

ndk::ScopedAStatus ModulePrimary::populateConnectedDevicePort(
        ::aidl::android::media::audio::common::AudioPort* audioPort, int32_t nextPortId) {
    LOG(INFO) << __func__ << ": " << audioPort->name << ", id: " << nextPortId;
    auto& audioDevice = audioPort->ext.get<aidl::android::media::audio::common::AudioPortExt::Tag::device>().device;
    const auto& c = AudioCardManager::getCardForDevice(audioDevice);
    if (!c)
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);

    if (audioDevice.type.type == ::aidl::android::media::audio::common::AudioDeviceType::OUT_DEVICE &&
            audioDevice.type.connection == ::aidl::android::media::audio::common::AudioDeviceDescription::CONNECTION_HDMI) {
        struct mixer_ctl *ctl = NULL;
        struct mixer *mixer;

        mixer = mixer_open(c->card);
        if (mixer) {
            int retry = 0;
            while (retry++ <= 10) {
                ctl = mixer_get_ctl_by_name(mixer, "HDMI Jack");
                if (ctl) {
                    /* If HDMI is connected, return ok */
                    if (mixer_ctl_get_value(ctl, 0) == 1) {
                        mixer_close(mixer);
                        return ndk::ScopedAStatus::ok();
                    }
                } else {
                    /* evk_8ulp hdmi driver imx-spdif doesn't support HDMI Jack */
                    LOG(INFO) << __func__ << ": HDMI Jack doesn't support";
                    mixer_close(mixer);
                    return ndk::ScopedAStatus::ok();
                }
                usleep(200000);
                LOG(INFO) << __func__ << ": detect HDMI connection, retry " << retry;
            }
            mixer_close(mixer);
        }
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::audio::core
