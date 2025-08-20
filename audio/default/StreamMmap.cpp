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

#include <unistd.h>
#include <cstdlib>

#define LOG_TAG "AHAL_MmapStream"
#include <android-base/logging.h>
#include <audio_utils/clock.h>
#include <error/Result.h>
#include <utils/SystemClock.h>

#include "core-impl/StreamMmap.h"

using aidl::android::hardware::audio::common::SinkMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::media::audio::common::AudioOffloadInfo;
using aidl::android::media::audio::common::MicrophoneInfo;

namespace aidl::android::hardware::audio::core {

void* StreamMmap::_threadLoop(void *arg) {
    StreamMmap* instance = static_cast<StreamMmap*>(arg);
    instance->threadLoop();
    return nullptr;
}

void* StreamMmap::threadLoop() {
    LOG(DEBUG) << __func__;
    while (mThreadRun) {
        if (mPcm) {
            if (pcm_mmap_avail(mPcm) >= MMAP_PERIOD_SIZE) {
                pcm_mmap_commit(mPcm, 0, MMAP_PERIOD_SIZE);
            }
        }
        usleep(MMAP_PERIOD_MS * 1000 / 2);
    }
    LOG(DEBUG) << __func__ << " end";
    return nullptr;
}

::android::status_t StreamMmap::init(DriverCallbackInterface* callback) {
    LOG(DEBUG) << __func__;
    return ::android::OK;
}

::android::status_t StreamMmap::drain(StreamDescriptor::DrainMode drainMode) {
    LOG(DEBUG) << __func__;
    stop();
    return ::android::OK;
}

::android::status_t StreamMmap::flush() {
    LOG(DEBUG) << __func__;
    stop();
    return ::android::OK;
}

::android::status_t StreamMmap::pause() {
    LOG(DEBUG) << __func__;
    stop();
    return ::android::OK;
}

::android::status_t StreamMmap::standby() {
    LOG(DEBUG) << __func__;
    stop();
    return ::android::OK;
}

void StreamMmap::stop() {
    if (!mIsStarted) {
        LOG(DEBUG) << __func__ << " already stopped.";
        return;
    }

    if (mThreadRun) {
        mThreadRun = false;
        pthread_join(mThreadId, nullptr);
    }

    if (mPcm) {
        pcm_stop(mPcm);
    }

    mTotalFrames += mPosition.frames;
    mIsStarted = false;
    return;
}

::android::status_t StreamMmap::start() {
    LOG(DEBUG) << __func__;

    if (mIsStarted) {
        LOG(DEBUG) << __func__ << ": already started.";
        return ::android::OK;
    }

    if (!openPcm()) {
        LOG(DEBUG) << __func__ << ": failed to open pcm.";
        return ::android::NO_INIT;
    }

    if (pcm_prepare(mPcm)) {
        LOG(DEBUG) << __func__ << ": prepare error: " << pcm_get_error(mPcm);
        return ::android::NO_INIT;
    }

    if (!mIsInput) {
        if (pcm_mmap_commit(mPcm, 0, MMAP_BUFFER_SIZE) < 0) {
            LOG(DEBUG) << __func__ << ": commit error: " << pcm_get_error(mPcm);
            return ::android::NO_INIT;
        }
    }

    if (pcm_start(mPcm)) {
        LOG(DEBUG) << __func__ << ": start error: " << pcm_get_error(mPcm);
        return ::android::NO_INIT;
    }

    struct sched_param schParam;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    schParam.sched_priority = 3;
    pthread_attr_setschedparam(&attr, &schParam);
    mThreadRun = true;
    pthread_create(&mThreadId, &attr, _threadLoop, this);
    pthread_attr_destroy(&attr);

    mIsStarted = true;

    return ::android::OK;
}

::android::status_t StreamMmap::transfer(void* buffer, size_t frameCount,
                                            size_t* actualFrameCount, int32_t* latencyMs) {
    return ::android::OK;
}

void StreamMmap::shutdown() {
    LOG(DEBUG) << __func__;
    stop();
    closePcm();
}

::android::status_t StreamMmap::refinePosition(StreamDescriptor::Position* position) {
    struct timespec ts = { 0, 0 };
    unsigned int frames = 0;

    position->frames = position->timeNs = StreamDescriptor::Position::UNKNOWN;

    if (!mIsStarted || !mPcm) {
        LOG(DEBUG) << __func__ << ": not started or no pcm.";
        return ::android::OK;
    }

    if (pcm_mmap_get_hw_ptr(mPcm, &frames, &ts)) {
        LOG(ERROR) << __func__ << " " << pcm_get_error(mPcm);
        return ::android::OK;
    }

    mPosition.frames = frames;
    mPosition.timeNs = audio_utils_ns_from_timespec(&ts);

    position->frames = mTotalFrames + mPosition.frames;
    position->timeNs = mPosition.timeNs;

    LOG(VERBOSE) << __func__ << ": position " << position->toString();
    return ::android::OK;
}

::android::status_t StreamMmap::getMmapPositionAndLatency(
        StreamDescriptor::Position* position, int32_t* latencyMs) {
    if (!mIsStarted) {
        position->frames = position->timeNs = StreamDescriptor::Position::UNKNOWN;
        LOG(DEBUG) << __func__ << ": not started";
        return ::android::OK;
    }

    position->frames = mTotalFrames + mPosition.frames;
    position->timeNs = mPosition.timeNs;
    *latencyMs = MMAP_PERIOD_MS;

    LOG(VERBOSE) << __func__ << ": position " << position->toString() << " latency " << *latencyMs;
    return ::android::OK;
}

const std::string StreamMmap::kCreateMmapBufferName = "aosp.createMmapBuffer";

StreamMmap::StreamMmap(StreamContext* context, const Metadata& metadata)
    : StreamCommonImpl(context, metadata),
      mIsInput(isInput(metadata)) {
    LOG(DEBUG) << __func__ << (mIsInput ? ": input" : ": output");
}

StreamMmap::~StreamMmap() {
    LOG(DEBUG) << __func__;
}

ndk::ScopedAStatus StreamMmap::getVendorParameters(const std::vector<std::string>& in_ids,
                                                       std::vector<VendorParameter>* _aidl_return) {
    std::vector<std::string> unprocessedIds;
    for (const auto& id : in_ids) {
        if (id == kCreateMmapBufferName) {
            LOG(DEBUG) << __func__ << ": " << id;
            MmapBufferDescriptor mmapDesc;
            RETURN_STATUS_IF_ERROR(createMmapBuffer(&mmapDesc));
            VendorParameter createMmapBuffer{.id = id};
            createMmapBuffer.ext.setParcelable(mmapDesc);
            LOG(DEBUG) << __func__ << ": returning " << mmapDesc.toString();
            _aidl_return->push_back(std::move(createMmapBuffer));
        } else {
            unprocessedIds.push_back(id);
        }
    }
    if (!unprocessedIds.empty()) {
        return StreamCommonImpl::getVendorParameters(unprocessedIds, _aidl_return);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus StreamMmap::setVendorParameters(
        const std::vector<VendorParameter>& in_parameters, bool in_async) {
    std::vector<VendorParameter> unprocessedParameters;
    for (const auto& param : in_parameters) {
        if (param.id == kCreateMmapBufferName) {
            LOG(DEBUG) << __func__ << ": " << param.id;
            // The value is irrelevant. The fact that this parameter can be "set" is an
            // indication that the method can be used by the client via 'getVendorParameters'.
        } else {
            unprocessedParameters.push_back(param);
        }
    }
    if (!unprocessedParameters.empty()) {
        return StreamCommonImpl::setVendorParameters(unprocessedParameters, in_async);
    }
    return ndk::ScopedAStatus::ok();
}

struct pcm* StreamMmap::openPcm() {
    struct pcm_config config;
    if (mPcm)
        return mPcm;

    const ConnectedDevices& connectedDevices = getConnectedDevices();
    const auto& c = AudioCardManager::getCardForDevice(connectedDevices[0]);
    if (!c) {
        LOG(ERROR) << __func__ << ": no connected devices";
        return nullptr;
    }

    config.channels = aidl::android::hardware::audio::common::getChannelCount(mContext.getChannelLayout());
    config.rate = MMAP_SAMPLE_RATE;
    config.format = MMAP_FORMAT;
    config.period_size = MMAP_PERIOD_SIZE;
    config.period_count = MMAP_PERIOD_COUNT;
    AudioCardManager::printPcmConfig(&config);

    if (mIsInput)
        mPcm = pcm_open(c->card, 0, PCM_IN | PCM_MMAP | PCM_MONOTONIC, &config);
    else
        mPcm = pcm_open(c->card, 0, PCM_OUT | PCM_MMAP | PCM_MONOTONIC, &config);

    if (!mPcm) return nullptr;

    if (!pcm_is_ready(mPcm)) {
        LOG(ERROR) << __func__ << ": fail to open pcm: " << pcm_get_error(mPcm);
        closePcm();
        return nullptr;
    }

    return mPcm;
}

void StreamMmap::closePcm() {
    if (mPcm) {
        pcm_close(mPcm);
        mPcm = nullptr;
    }
}

ndk::ScopedAStatus StreamMmap::createMmapBuffer(MmapBufferDescriptor* desc) {
    if (!openPcm())
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    desc->sharedMemory.fd = ndk::ScopedFileDescriptor(dup(pcm_get_poll_fd(mPcm)));
    desc->sharedMemory.size = static_cast<size_t>(MMAP_BUFFER_SIZE) * mContext.getFrameSize();
    desc->burstSizeFrames = MMAP_PERIOD_SIZE;
    desc->flags = 0;
    return ndk::ScopedAStatus::ok();
}

StreamInMmap::StreamInMmap(StreamContext&& context, const SinkMetadata& sinkMetadata,
                                   const std::vector<MicrophoneInfo>& microphones)
    : StreamIn(std::move(context), microphones), StreamMmap(&mContextInstance, sinkMetadata) {}

StreamOutMmap::StreamOutMmap(StreamContext&& context, const SourceMetadata& sourceMetadata,
                                     const std::optional<AudioOffloadInfo>& offloadInfo)
    : StreamOut(std::move(context), offloadInfo),
      StreamMmap(&mContextInstance, sourceMetadata) {}

}  // namespace aidl::android::hardware::audio::core
