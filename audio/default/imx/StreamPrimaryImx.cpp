/*
 * Copyright (C) 2023 The Android Open Source Project
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

#define LOG_TAG "AHAL_StreamPrimaryImx"

#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <audio_utils/clock.h>
#include <error/Result.h>
#include <error/expected_utils.h>

#include "imx/StreamPrimaryImx.h"

#include <audio_utils/primitives.h>
#include <cutils/properties.h>
#include <fstream>
#include "core-impl/AudioCardManager.h"
extern "C" {
#include "alsa_device_profile.h"
}

#define DEFAULT_PERIOD_SIZE 1024
#define DEFAULT_PERIOD_COUNT 4
#define DEFAULT_INPUT_RATE 48000
#define DEFAULT_INPUT_CHANNELS 2
#define LPA_PERIOD_MS 500
#define LPA_BUFFER_SECOND 20

using aidl::android::hardware::audio::common::isBitPositionFlagSet;
using aidl::android::media::audio::common::AudioIoFlags;
using aidl::android::media::audio::common::AudioOutputFlags;

using aidl::android::hardware::audio::common::SinkMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::media::audio::common::AudioDevice;
using aidl::android::media::audio::common::AudioDeviceAddress;
using aidl::android::media::audio::common::AudioDeviceDescription;
using aidl::android::media::audio::common::AudioDeviceType;
using aidl::android::media::audio::common::AudioOffloadInfo;
using aidl::android::media::audio::common::MicrophoneInfo;
using android::base::GetBoolProperty;

namespace aidl::android::hardware::audio::core {

StreamPrimaryImx::StreamPrimaryImx(StreamContext* context, const Metadata& metadata)
    : StreamPrimary(context, metadata) {
    mSavedConfig = mConfig;
    mDump = property_get_bool("persist.vendor.audio.dump", false);
    property_get("ro.boot.soc_type", soc_name, NULL);
    auto flags = getContext().getFlags();
    if (flags.getTag() == AudioIoFlags::Tag::output) {
        if (isBitPositionFlagSet(flags.template get<AudioIoFlags::Tag::output>(),
                               AudioOutputFlags::DIRECT)) {
            mPrimaryOutput = false;
            mDirectOutput = true;
        } else {
            mPrimaryOutput = true;
            mDirectOutput = false;
        }
    }
    if (!mDirectOutput)
        context->startStreamDataProcessor();
    ALOGD("%s: mPrimaryOutput: %d, mDirectOutput: %d", __func__, mPrimaryOutput, mDirectOutput);
    if (mDump) {
        std::ofstream ifile(kDumpPrimaryInputFile, std::ios::trunc);
        std::ofstream ofile(kDumpPrimaryOutputFile, std::ios::trunc);
    }
}

void StreamPrimaryImx::dump(const void* buffer, size_t bytes, const char* name) {
    if ((buffer == NULL) || (bytes == 0) || (name == NULL))
        return;

    int fdDump = open(name, O_CREAT | O_APPEND | O_WRONLY, S_IRWXU | S_IRWXG);
    if (fdDump < 0) {
        ALOGW("%s: file open error, srcFile: %s, fd %d", __func__, name, fdDump);
        return;
    }

    write(fdDump, buffer, bytes);
    ::close(fdDump);
}

::android::status_t StreamPrimaryImx::flush() {
    return isStubStreamOnWorker() ? mStubDriver.flush() : StreamAlsa::flush();
}

::android::status_t StreamPrimaryImx::pause() {
    if (mHardwarePause && mStarted) {
        proxy_pause(mAlsaDeviceProxies[0].get());
    }
    return StreamPrimary::pause();
}

void StreamPrimaryImx::tryStart(){
    auto status = StreamAlsa::start();
    if (status != ::android::OK) {
        mStarted = false;
    } else {
        mStarted = true;
        mFirstTransfer = true;
    }
}

void StreamPrimaryImx::stop() {
    if (mCard) {
        std::unique_lock lock(mCard->mLock);
        if (mIsInput)
            mCard->inOwner = OWNER_NONE;
        else if (mPrimaryOutput && mCard->outOwner == OWNER_PRIMARY)
            mCard->outOwner = OWNER_NONE;
        else if (mDirectOutput && mCard->outOwner == OWNER_DIRECT)
            mCard->outOwner = OWNER_NONE;
    }
    mStarted = false;
    if (mResampler) {
        release_resampler(mResampler);
        mResampler = NULL;
        if (mResamplerBuffer) {
            free(mResamplerBuffer);
            mResamplerBuffer = NULL;
        }
        LOG(DEBUG) << __func__ << ": Release resampler.";
    }
    mConfig = mSavedConfig;
}

::android::status_t StreamPrimaryImx::standby() {
    stop();
    return StreamPrimary::standby();
}

::android::status_t StreamPrimaryImx::start() {
    bool isStub = true, shutdownAlsaStream = false;
    {
        std::lock_guard l(mLock);
        isStub = mAlsaDeviceId == kStubDeviceId;
        shutdownAlsaStream =
                mCurrAlsaDeviceId != mAlsaDeviceId && mCurrAlsaDeviceId != kStubDeviceId;
        mCurrAlsaDeviceId = mAlsaDeviceId;
    }
    if (shutdownAlsaStream) {
        StreamAlsa::shutdown();
    }
    if (isStub) {
        return mStubDriver.start();
    }

    if (!mAlsaDeviceProxies.empty()) {
        // This is a resume after a pause.
        if (mHardwarePause && mStarted) {
            proxy_resume(mAlsaDeviceProxies[0].get());
        }
        return ::android::OK;
    }
    mCard = AudioCardManager::getCardForDevice(getConnectedDevices().at(0));
    if (!mCard) {
        return ::android::NO_INIT;
    }
    bool toStart = false;
    {
    std::unique_lock lock(mCard->mLock);
    if (mPrimaryOutput) {
        /* output priority: DIRECT(3) > PRIMARY(2) > HFP(1) > NONE(0) */
        if (mCard->outOwner < OWNER_DIRECT) {
            mCard->outOwner = OWNER_PRIMARY;
            toStart = true;
        }
    } else if (mDirectOutput) {
        mCard->outOwner = OWNER_DIRECT;
        toStart = true;
    } else if (mIsInput) {
        /* input priority: HFP(1) > PRIMARY(2) > NONE(0) */
        if (mCard->inOwner != OWNER_HFP) {
            mCard->inOwner = OWNER_PRIMARY;
            toStart = true;
        }
    } else {
        toStart = true;
    }
    }
    if (toStart)
        tryStart();

    if (mIsInput && !mStarted && mConfig->rate != DEFAULT_INPUT_RATE) {
        auto requested_rate = mConfig->rate;
        mConfig->rate = DEFAULT_INPUT_RATE;
        mConfig->channels = DEFAULT_INPUT_CHANNELS;
        tryStart();
        if (mStarted) {
            int ret = create_resampler(
                    DEFAULT_INPUT_RATE, requested_rate, mConfig->channels,
                    RESAMPLER_QUALITY_MAX - 1,
                    NULL,
                    &mResampler);
            if (ret) {
                LOG(ERROR) << "Resampler initialization failed! Error code " << ret;
                return ::android::NO_INIT;
            }
            mResamplerBuffer = (int16_t *)malloc(mBufferSizeFrames * mFrameSizeBytes * DEFAULT_INPUT_CHANNELS);
            if (!mResamplerBuffer) {
                LOG(ERROR) << "Resampler buffer initialization failed!";
                if (mResampler) {
                    release_resampler(mResampler);
                    mResampler = NULL;
                }
                return ::android::NO_INIT;
            }
            LOG(DEBUG) << __func__ << ": Create resampler from "
                << DEFAULT_INPUT_RATE << " to " << requested_rate
                << ", buffer frames " << mBufferSizeFrames
                << ", frame size " << mFrameSizeBytes;
        } else {
            mConfig->rate = requested_rate;
            LOG(DEBUG) << __func__ << ": The default input rate " << DEFAULT_INPUT_RATE << " is not supported.";
            return ::android::NO_INIT;
        }
    }
    mStartTimeNs = ::android::uptimeNanos();
    mStartRetryCount = 0;
    mFramesSinceStart = 0;
    mSkipNextTransfer = false;
    return ::android::OK;
}

::android::status_t StreamPrimaryImx::transfer(void* buffer, size_t frameCount,
                                            size_t* actualFrameCount, int32_t* latencyMs) {
    LOG(VERBOSE) << __func__ << ": start transfer: " << frameCount;
    if (isStubStreamOnWorker()) {
        return mStubDriver.transfer(buffer, frameCount, actualFrameCount, latencyMs);
    }

    bool toStandby = false, toStart = false;
    {
    std::unique_lock lock(mCard->mLock);
    if (mPrimaryOutput) {
        /* output priority: DIRECT(3) > PRIMARY(2) > HFP(1) > NONE(0) */
        if (mCard->outOwner > OWNER_PRIMARY) {
            if (mStarted) {
                LOG(DEBUG) << __func__ << ": standby the primary output for direct";
                toStandby = true;
            }
        } else if (mCard->outOwner == OWNER_PRIMARY) {
            if (!mStarted) {
                toStart = true;
            }
        } else {
            mCard->outOwner = OWNER_PRIMARY;
        }
    } else if (mDirectOutput) {
        if (mCard->outOwner == OWNER_DIRECT) {
            if (!mStarted && mStartRetryCount < kMaxStartRetryCount) {
                toStart = true;
            }
        } else {
            mCard->outOwner = OWNER_DIRECT;
        }
    } else if (mIsInput) {
        /* input priority: HFP(1) > PRIMARY(2) > NONE(0) */
        if (mCard->inOwner == OWNER_HFP) {
            if (mStarted) {
                LOG(DEBUG) << __func__ << ": standby the primary input for hfp";
                toStandby = true;
            }
        } else if (mCard->inOwner == OWNER_PRIMARY) {
            if (!mStarted) {
                toStart = true;
            }
        } else {
            mCard->inOwner = OWNER_PRIMARY;
        }
    }
    }
    if (toStart)
        tryStart();
    if (toStandby)
        standby();

    if (mDirectOutput && mCard->outOwner == OWNER_DIRECT) {
        if (mStarted)
            mStartRetryCount = 0;
        else {
            mStartRetryCount ++;
            if (mStartRetryCount == kMaxStartRetryCount)
                LOG(DEBUG) << __func__ << ": stop trying to start after " << mStartRetryCount << " times";
        }
    }

    mFramesSinceStart += frameCount;
    if (!mStarted) {
        *actualFrameCount = frameCount;
        const long bufferDurationUs =
                (*actualFrameCount) * MICROS_PER_SECOND / mContext.getSampleRate();
        const auto totalDurationUs =
                (::android::uptimeNanos() - mStartTimeNs) / NANOS_PER_MICROSECOND;
        const long totalOffsetUs =
                mFramesSinceStart * MICROS_PER_SECOND / mContext.getSampleRate() - totalDurationUs;
        if (totalOffsetUs > 0) {
            const long sleepTimeUs = std::min(totalOffsetUs, bufferDurationUs);
            if (sleepTimeUs > 500000) {
                LOG(WARNING) << __func__ << ": sleeping for " << sleepTimeUs << " us";
            }
            usleep(sleepTimeUs);
        } else {
            LOG(WARNING) << __func__ << ": Wrong sleep time: totalOffsetUs " << totalOffsetUs
                << ", bufferDurationUs " << bufferDurationUs
                << ", totalDurationUs " << totalDurationUs
                << ", mFramesSinceStart " << mFramesSinceStart
                << ", actualFrameCount " << *actualFrameCount;
        }
        return ::android::OK;
    }

    if (!mIsInput && mConvertChannel &&
            (mConfig->channels == 4 || mConfig->channels == 6 || mConfig->channels == 8)) {
        if (mConfig->format == PCM_FORMAT_S16_LE) {
            AudioCardManager::convertChannelS16(buffer, frameCount * mFrameSizeBytes, mConfig->channels);
        } else if (mConfig->format == PCM_FORMAT_S32_LE) {
            AudioCardManager::convertChannelS32(buffer, frameCount * mFrameSizeBytes, mConfig->channels);
        }
    }

    if (mDump && !mIsInput)
        dump(buffer, frameCount * mFrameSizeBytes, kDumpPrimaryOutputFile);

    if (mFirstTransfer)
        LOG(DEBUG) << __func__ << ": Start first transfer " << frameCount << " frames.";

    if (mIsStereoToMono) {
        if (mIsInput) {
            auto dst = static_cast<int16_t*>(buffer);
            std::unique_ptr<int16_t[]> src{new int16_t[frameCount]};

            RETURN_STATUS_IF_ERROR(
                    StreamAlsa::transfer(src.get(), frameCount / 2, actualFrameCount, latencyMs));
            upmix_to_stereo_i16_from_mono_i16(dst, src.get(), frameCount);
        } else {
            auto src = static_cast<const int16_t*>(buffer);
            std::unique_ptr<int16_t[]> dst{new int16_t[frameCount]};

            downmix_to_mono_i16_from_stereo_i16(dst.get(), src, frameCount);
            RETURN_STATUS_IF_ERROR(
                    StreamAlsa::transfer(dst.get(), frameCount / 2, actualFrameCount, latencyMs));
        }
        *actualFrameCount *= 2;

        goto done;
    }

    if (mIsS32ToS16) {
        auto channels = aidl::android::hardware::audio::common::getChannelCount(getContext().getChannelLayout());
        auto dst = static_cast<int16_t*>(buffer);
        std::unique_ptr<int32_t[]> src{new int32_t[frameCount * channels]};

        RETURN_STATUS_IF_ERROR(
                StreamAlsa::transfer(src.get(), frameCount * 2, actualFrameCount, latencyMs));
        memcpy_to_i16_from_i32(dst, src.get(), frameCount * channels);

        *actualFrameCount /= 2;

        goto done;
    }

    if (mIsS16ToS24) {
        auto channels = aidl::android::hardware::audio::common::getChannelCount(getContext().getChannelLayout());
        auto src = static_cast<int16_t*>(buffer);
        std::unique_ptr<int32_t[]> dst{new int32_t[frameCount * channels]};

        memcpy_to_q8_23_from_i16(dst.get(), src, frameCount * channels);
        RETURN_STATUS_IF_ERROR(
                StreamAlsa::transfer(dst.get(), frameCount * channels, actualFrameCount, latencyMs));

        *actualFrameCount /= 2;

        goto done;
    }

    if (mResampler) {
        int ratio = 1;
        if (mSavedConfig->channels == 1)
            ratio = 2;
        StreamAlsa::transfer(mResamplerBuffer, frameCount * ratio, actualFrameCount, latencyMs);
        *actualFrameCount /= ratio;
        size_t in_frame_count = *actualFrameCount;
        size_t out_frame_count = *actualFrameCount;
        mResampler->resample_from_input(mResampler,
                (int16_t *)mResamplerBuffer, &in_frame_count,
                (int16_t *)buffer, &out_frame_count);
        *actualFrameCount = out_frame_count;
        if (mSavedConfig->channels == 1) {
            downmix_to_mono_i16_from_stereo_i16((int16_t*)buffer, (const int16_t*)buffer, out_frame_count);
        }
        goto done;
    }

    RETURN_STATUS_IF_ERROR(
            StreamAlsa::transfer(buffer, frameCount, actualFrameCount, latencyMs));

done:
    if (mFirstTransfer) {
        mFirstTransfer = false;
        LOG(DEBUG) << __func__ << ": End first transfer " << *actualFrameCount << " frames.";
    }

    if (mDump && mIsInput)
        dump(buffer, frameCount * mFrameSizeBytes, kDumpPrimaryInputFile);
    LOG(VERBOSE) << __func__ << ": end transfer: " << *actualFrameCount;
    return ::android::OK;
}

::android::status_t StreamPrimaryImx::refinePosition(StreamDescriptor::Position* position) {
    if ((property_get_int32("vendor.audio.lpa.enable", 0) && mDirectOutput) ||
            (getContext().getFormat().encoding == "audio/vnd.sony.dsd")) {
        return StreamAlsa::refinePosition(position);
    }
    // Stub path (e.g. imx8mq input, no codec): no hw clock, mStarted never set.
    // Keep OK above the !mStarted check so the framework's frame counter drives
    // the observable position (else it stays -1, VTS position never increases).
    if (isStubStreamOnWorker()) {
        return ::android::OK;
    }
    if (!mStarted) {
        // ALSA proxy not open yet. NO_INIT (not OK) skips the poll (no warning)
        // and makes the framework drop this sample, else VTS position goes backward.
        return ::android::NO_INIT;
    }
    if (mIsInput) {
        // Use ALSA hw timestamps and clamp to monotonic max to avoid retrograde position.
        const int64_t savedFrames = position->frames;
        const int64_t savedTimeNs = position->timeNs;
        if (StreamAlsa::refinePosition(position) != ::android::OK) {
            position->frames = savedFrames;
            position->timeNs = savedTimeNs;
        }
        if (position->frames < mLastCapturePosition) {
            position->frames = mLastCapturePosition;
        } else {
            mLastCapturePosition = position->frames;
        }
    } else {
        if (StreamAlsa::refinePosition(position) != ::android::OK) {
            int64_t buffer_size = static_cast<int64_t>(mConfig->period_size) * mConfig->period_count;
            position->frames = position->frames > buffer_size ?
                (position->frames - buffer_size) : 0;
        }
    }

    return ::android::OK;
}

void StreamPrimaryImx::shutdown() {
    stop();
    StreamPrimary::shutdown();
}

ndk::ScopedAStatus StreamPrimaryImx::setConnectedDevices(const ConnectedDevices& devices) {
    auto status = StreamPrimary::setConnectedDevices(devices);

    static bool kSimulateInput = false;
    if (0 == strcmp(soc_name, "imx8mq")) {
        kSimulateInput = GetBoolProperty("ro.boot.audio.tinyalsa.simulate_input", true);
    } else {
        kSimulateInput = GetBoolProperty("ro.boot.audio.tinyalsa.simulate_input", false);
    }

    if (!devices.empty() && devices[0].type.type == AudioDeviceType::IN_HEADSET &&
            devices[0].type.connection == AudioDeviceDescription::CONNECTION_BT_SCO)
        kSimulateInput = false;

    if (devices.empty())
        kSimulateInput = true;

    if (kSimulateInput && mIsInput) {
        std::lock_guard l(mLock);
        mAlsaDeviceId = kStubDeviceId;
    }
    return status;
}

std::vector<alsa::DeviceProfile> StreamPrimaryImx::getDeviceProfiles() {
    std::vector<alsa::DeviceProfile> deviceProfile{
        alsa::DeviceProfile{.card = 0,
            .device = 0,
            .direction = mIsInput ? PCM_IN : PCM_OUT,
            .isExternal = false}};
    const ConnectedDevices& connectedDevices = getConnectedDevices();
    if (connectedDevices.size() > 1)
        LOG(WARNING) << __func__ << ": size of ConnectedDevices is larger than 1";

    struct audio_card *card = AudioCardManager::getCardForDevice(connectedDevices[0]);
    if (card) {
        deviceProfile[0].card = card->card;

        if (strstr(card->driver_name, "sco-audio")) {
            if (mSavedConfig.has_value() && mSavedConfig->channels == 2) {
                mConfig->channels = 1;
                mIsStereoToMono = true;
                LOG(INFO) << __func__ << ": Force set mono channel for bt_sco";
            }
        } else if (strstr(card->driver_name, "micfil") && !card->support_s16) {
            mConfig->format = PCM_FORMAT_S32_LE;
            mIsS32ToS16 = true;
            LOG(INFO) << __func__ << ": Force set S32 format for micfil";
        } else if (mIsStereoToMono || mIsS32ToS16) {
            mIsStereoToMono = false;
            mIsS32ToS16 = false;
            mConfig = mSavedConfig;
        }

        if (strstr(card->driver_name, "cs42888")) {
            mConvertChannel = true;
            LOG(INFO) << __func__ << ": Convert channels.";
        }

        if ((property_get_int32("vendor.persist.audio.pass.through", 0) == 2000) &&
                ((0 == strcmp(soc_name, "imx8mp")) || (0 == strcmp(soc_name, "imx8ulp")))) {
            mIsS16ToS24 = true;
            LOG(INFO) << __func__ << ": Force set S24 format for passthrough on imx8mp/imx8ulp";
            mConfig->format = PCM_FORMAT_S24_LE;
        } else if (mIsS16ToS24) {
            mIsS16ToS24 = false;
            mConfig = mSavedConfig;
        }

        mConfig->period_size = mBufferSizeFrames;
        mConfig->period_count = DEFAULT_PERIOD_COUNT;

        if (card->out_period_size) {
            mConfig->period_size = card->out_period_size;
        }
        if (card->out_period_count) {
            mConfig->period_count = card->out_period_count;
        }

        if (!mIsInput) {
            struct pcm_params *params = pcm_params_get(card->card, 0, PCM_OUT);
            if (params) {
                unsigned int max_period_size = pcm_params_get_max(params, PCM_PARAM_PERIOD_SIZE);
                if (mConfig->period_size * mConfig->channels > max_period_size) {
                    mConfig->period_size = DEFAULT_PERIOD_SIZE;
                    LOG(INFO) << __func__ << ": Force set period size from " <<
                        mBufferSizeFrames << " to " << DEFAULT_PERIOD_SIZE <<
                        ", the max is " << max_period_size;
                }
                pcm_params_free(params);
            }
        }

        if (property_get_int32("vendor.audio.lpa.enable", 0) && mDirectOutput) {
            mConfig->period_size = mConfig->rate * LPA_PERIOD_MS / 1000;
            mConfig->period_count = LPA_BUFFER_SECOND * 1000 / LPA_PERIOD_MS;
            mHardwarePause = true;
            LOG(INFO) << __func__ << ": Force set period size as " << LPA_PERIOD_MS << "ms for LPA";
        }
    }

    return deviceProfile;
}

StreamInPrimaryImx::StreamInPrimaryImx(StreamContext&& context, const SinkMetadata& sinkMetadata,
                                 const std::vector<MicrophoneInfo>& microphones)
    : StreamIn(std::move(context), microphones),
      StreamPrimaryImx(&mContextInstance, sinkMetadata),
      StreamInHwGainHelper(&mContextInstance) {}

StreamOutPrimaryImx::StreamOutPrimaryImx(StreamContext&& context, const SourceMetadata& sourceMetadata,
                                   const std::optional<AudioOffloadInfo>& offloadInfo)
    : StreamOut(std::move(context), offloadInfo),
      StreamPrimaryImx(&mContextInstance, sourceMetadata),
      StreamOutHwVolumeHelper(&mContextInstance) {}

}  // namespace aidl::android::hardware::audio::core
