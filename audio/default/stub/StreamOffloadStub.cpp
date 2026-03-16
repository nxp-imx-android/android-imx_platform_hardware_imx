/*
 * Copyright (C) 2025 The Android Open Source Project
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

#define LOG_TAG "AHAL_OffloadStream"
#include <Log.h>
#include <audio_utils/clock.h>
#include <error/Result.h>
#include <utils/SystemClock.h>

#include "ApeHeader.h"
#include "MpegAudioHeader.h"
#include "core-impl/StreamOffloadStub.h"

using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::media::audio::common::AudioDevice;
using aidl::android::media::audio::common::AudioFormatType;
using aidl::android::media::audio::common::AudioOffloadInfo;
using aidl::android::media::audio::common::FlushFromFrameAccuracy;
using aidl::android::media::audio::common::MicrophoneInfo;

namespace aidl::android::hardware::audio::core {

namespace offload {

static constexpr int32_t SAFE_FLUSH_FROM_MARGIN_IN_MS = 100;

std::string DspSimulatorLogic::init() {
    return "";
}

DspSimulatorLogic::Status DspSimulatorLogic::cycle() {
    using DrainMode = StreamDescriptor::DrainMode;
    std::vector<std::pair<int64_t, bool>> clipNotifies;
    DrainMode emptyDrainNotify = DrainMode::DRAIN_UNSPECIFIED;
    // Simulate playback.
    const int64_t timeBeginNs = ::android::uptimeNanos();
    usleep(1000);
    const int64_t clipFramesPlayed =
            (::android::uptimeNanos() - timeBeginNs) * mSharedState.sampleRate / NANOS_PER_SECOND;
    const int64_t bufferFramesConsumed =
            mSharedState.format.type == AudioFormatType::PCM
                    ? clipFramesPlayed       // For PCM data, the data is not compressed
                    : clipFramesPlayed / 2;  // assume 1:2 compression ratio
    int64_t bufferFramesLeft = 0, bufferNotifyFrames = DspSimulatorState::kSkipBufferNotifyFrames;
    {
        std::lock_guard l(mSharedState.lock);
        if (mSharedState.clips.empty()) {
            emptyDrainNotify = mSharedState.draining;
        }
        mSharedState.draining = DrainMode::DRAIN_UNSPECIFIED;
        mSharedState.bufferFramesLeft =
                mSharedState.bufferFramesLeft > bufferFramesConsumed
                        ? mSharedState.bufferFramesLeft - bufferFramesConsumed
                        : 0;
        int64_t framesPlayed = clipFramesPlayed;
        while (framesPlayed > 0 && !mSharedState.clips.empty()) {
            LOG(VERBOSE) << __func__ << ": " << mSharedState.clips.log();
            const bool hasNextClip = mSharedState.clips.hasNext();
            if (mSharedState.clips.currentFrames() > framesPlayed) {
                mSharedState.clips.updateCurrentFrames(-framesPlayed);
                mSharedState.mTotalFramesPlayed += framesPlayed;
                framesPlayed = 0;
                if (auto clipFramesLeft = mSharedState.clips.currentFrames();
                    clipFramesLeft <= mSharedState.earlyNotifyFrames) {
                    clipNotifies.emplace_back(clipFramesLeft, hasNextClip);
                }
            } else {
                mSharedState.mTotalFramesPlayed += mSharedState.clips.currentFrames();
                if (mSharedState.format.type == AudioFormatType::PCM) {
                    // There is not enough data to be full played, set `framesPlayed` to 0 so that
                    // it can exit the while loop.
                    framesPlayed = 0;
                    mSharedState.clips.trimCurrentFrames(0);
                    clipNotifies.emplace_back(0 /*clipFramesLeft*/, false /*hasNextClip*/);
                } else {
                    clipNotifies.emplace_back(0 /*clipFramesLeft*/, hasNextClip);
                    framesPlayed -= mSharedState.clips.removeCurrent();
                }
                if (!hasNextClip) {
                    // Since it's a simulation, the buffer consumption rate it not real,
                    // thus 'bufferFramesLeft' might still have something, need to erase it.
                    mSharedState.bufferFramesLeft = 0;
                }
            }
        }
        bufferFramesLeft = mSharedState.bufferFramesLeft;
        bufferNotifyFrames = mSharedState.bufferNotifyFrames;
        if (bufferFramesLeft <= bufferNotifyFrames) {
            // Suppress further notifications.
            mSharedState.bufferNotifyFrames = DspSimulatorState::kSkipBufferNotifyFrames;
        }
    }
    if (bufferFramesLeft <= bufferNotifyFrames) {
        LOG(DEBUG) << __func__ << ": sending onBufferStateChange: " << bufferFramesLeft;
        mSharedState.callback->onBufferStateChange(bufferFramesLeft);
    }
    if (!clipNotifies.empty()) {
        for (const auto& notify : clipNotifies) {
            LOG(DEBUG) << __func__ << ": sending onClipStateChange: " << notify.first << ", "
                       << notify.second;
            mSharedState.callback->onClipStateChange(notify.first, notify.second);
        }
    } else if (emptyDrainNotify != DrainMode::DRAIN_UNSPECIFIED) {
        LOG(DEBUG) << __func__ << ": sending onClipStateChange with no clips for "
                   << toString(emptyDrainNotify);
        if (emptyDrainNotify == DrainMode::DRAIN_EARLY_NOTIFY) {
            mSharedState.callback->onClipStateChange(1 /*clipFramesLeft*/, false /*hasNextClip*/);
        }
        mSharedState.callback->onClipStateChange(0 /*clipFramesLeft*/, false /*hasNextClip*/);
    }
    return Status::CONTINUE;
}

}  // namespace offload

using offload::DspSimulatorState;

DriverOffloadStubImpl::DriverOffloadStubImpl(const StreamContext& context)
    : DriverStubImpl(context, 0 /*asyncSleepTimeUs*/),
      mBufferNotifyFrames(static_cast<int64_t>(context.getBufferSizeInFrames()) / 2),
      mSafeMarginForFlushFromFrames(offload::SAFE_FLUSH_FROM_MARGIN_IN_MS *
                                    context.getSampleRate() / MILLIS_PER_SECOND),
      mState{context.getFormat(), context.getSampleRate(),
             250 /*earlyNotifyMs*/ * context.getSampleRate() / MILLIS_PER_SECOND},
      mDspWorker(mState) {
    LOG_IF(FATAL, !mIsAsynchronous) << "The steam must be used in asynchronous mode";

    if (mState.format.encoding == "audio/x-ape") {
        mTransferHandler = &DriverOffloadStubImpl::handleApeTransfer;
    } else if (mState.format.encoding == "audio/mpeg") {
        mTransferHandler = &DriverOffloadStubImpl::handleMpegTransfer;
    } else if (mState.format.type == AudioFormatType::PCM) {
        mTransferHandler = &DriverOffloadStubImpl::handlePcmTransfer;
        // For PCM offload, there is no way for the HAL to know where is the end of a clip.
        // In that case, it assumes there is only one clip.
        LOG_IF(FATAL, !mState.clips.add(0)) << "Cannot initialize for PCM offload";
    }
}

::android::status_t DriverOffloadStubImpl::init(DriverCallbackInterface* callback) {
    RETURN_STATUS_IF_ERROR(DriverStubImpl::init(callback));
    if (!StreamOffloadStub::getSupportedEncodings().count(mState.format.encoding) &&
        mState.format.type != AudioFormatType::PCM) {
        LOG(ERROR) << __func__ << ": encoded format \"" << mState.format.toString()
                   << "\" is not supported";
        return ::android::NO_INIT;
    }
    mState.callback = callback;
    return ::android::OK;
}

::android::status_t DriverOffloadStubImpl::drain(StreamDescriptor::DrainMode drainMode) {
    RETURN_STATUS_IF_ERROR(DriverStubImpl::drain(drainMode));
    std::lock_guard l(mState.lock);
    if (!mState.clips.empty()) {
        mState.clips.trimCurrentFrames(mState.earlyNotifyFrames * 2);
        if (drainMode == StreamDescriptor::DrainMode::DRAIN_ALL) {
            mState.clips.eraseAllNext();
        }
    }
    mState.bufferNotifyFrames = DspSimulatorState::kSkipBufferNotifyFrames;
    mState.draining = drainMode;
    return ::android::OK;
}

::android::status_t DriverOffloadStubImpl::flush() {
    RETURN_STATUS_IF_ERROR(DriverStubImpl::flush());
    mDspWorker.pause();
    {
        std::lock_guard l(mState.lock);
        mState.clips.erase();
        mState.bufferFramesLeft = 0;
        mState.bufferNotifyFrames = DspSimulatorState::kSkipBufferNotifyFrames;
    }
    return ::android::OK;
}

::android::status_t DriverOffloadStubImpl::pause() {
    RETURN_STATUS_IF_ERROR(DriverStubImpl::pause());
    mDspWorker.pause();
    {
        std::lock_guard l(mState.lock);
        mState.bufferNotifyFrames = DspSimulatorState::kSkipBufferNotifyFrames;
    }
    return ::android::OK;
}

::android::status_t DriverOffloadStubImpl::start() {
    RETURN_STATUS_IF_ERROR(DriverStubImpl::start());
    RETURN_STATUS_IF_ERROR(startWorkerIfNeeded());
    bool hasClips;  // Can be start after paused draining.
    {
        std::lock_guard l(mState.lock);
        hasClips = !mState.clips.empty();
        LOG(DEBUG) << __func__ << ": " << mState.clips.log();
        mState.bufferNotifyFrames = DspSimulatorState::kSkipBufferNotifyFrames;
    }
    if (hasClips) {
        mDspWorker.resume();
    }
    return ::android::OK;
}

::android::status_t DriverOffloadStubImpl::transfer(void* buffer, size_t frameCount,
                                                    size_t* actualFrameCount, int32_t* latencyMs) {
    RETURN_STATUS_IF_ERROR(
            DriverStubImpl::transfer(buffer, frameCount, actualFrameCount, latencyMs));
    RETURN_STATUS_IF_ERROR(startWorkerIfNeeded());

    if (mTransferHandler != nullptr) {
        const ::android::status_t status =
                (this->*mTransferHandler)(buffer, frameCount, actualFrameCount);
        if (status != ::android::OK) {
            return status;
        }
    } else {
        LOG(FATAL) << __func__ << ": unsupported format for offload: " << mState.format.toString();
        *actualFrameCount = 0;
    }

    {
        std::lock_guard l(mState.lock);
        mState.bufferFramesLeft = *actualFrameCount;
        mState.bufferNotifyFrames = mBufferNotifyFrames;
    }
    mDspWorker.resume();
    return ::android::OK;
}

::android::status_t DriverOffloadStubImpl::flushFromFrame(FlushFromFrameAccuracy accuracy,
                                                          int32_t position,
                                                          int32_t* flushFromPosition) {
    LOG(DEBUG) << __func__ << ": accuracy=" << toString(accuracy) << ", position=" << position;
    if ((accuracy != FlushFromFrameAccuracy::BEST_EFFORT &&
         accuracy != FlushFromFrameAccuracy::EXACT) ||
        position < 0 || flushFromPosition == nullptr) {
        *flushFromPosition = position;
        LOG(ERROR) << __func__ << ": invalid parameters, accuracy= " << toString(accuracy)
                   << ", position=" << position
                   << ", suggestedPosition is null:" << (flushFromPosition == nullptr);
        return ::android::BAD_VALUE;
    }
    if (mState.format.type != media::audio::common::AudioFormatType::PCM) {
        // Currently only support flushFromFrame for PCM offload.
        LOG(ERROR) << __func__ << ": invalid as format is " << mState.format.toString();
        return ::android::INVALID_OPERATION;
    }
    {
        std::lock_guard l(mState.lock);
        const int64_t requestedPosition = position + mState.mLastReportedFrames;
        const int64_t safeFlushedPosition =
                mState.mTotalFramesPlayed +
                std::min((int64_t)mSafeMarginForFlushFromFrames, mState.clips.currentFrames());
        const int64_t totalWrittenFrames = mState.mTotalFramesPlayed + mState.clips.currentFrames();
        if (accuracy == FlushFromFrameAccuracy::BEST_EFFORT) {
            if (requestedPosition < safeFlushedPosition) {
                *flushFromPosition = safeFlushedPosition - mState.mLastReportedFrames;
                mState.clips.trimCurrentFrames(safeFlushedPosition - mState.mTotalFramesPlayed);
            } else if (requestedPosition > totalWrittenFrames) {
                *flushFromPosition = totalWrittenFrames - mState.mLastReportedFrames;
            } else {
                *flushFromPosition = requestedPosition - mState.mLastReportedFrames;
                mState.clips.trimCurrentFrames(requestedPosition - mState.mTotalFramesPlayed);
            }
        } else {  // accuracy == FlushFromFrameAccuracy::EXACT
            if (requestedPosition < safeFlushedPosition ||
                requestedPosition > mState.mTotalFramesPlayed + mState.clips.currentFrames()) {
                *flushFromPosition = safeFlushedPosition - mState.mLastReportedFrames;
                return ::android::BAD_VALUE;
            } else {
                const int64_t positionToTrim = requestedPosition - mState.mTotalFramesPlayed;
                *flushFromPosition = requestedPosition - mState.mLastReportedFrames;
                mState.clips.trimCurrentFrames(positionToTrim);
            }
        }
    }
    return ::android::OK;
}

::android::status_t DriverOffloadStubImpl::handleApeTransfer(void* buffer, size_t frameCount,
                                                             size_t* actualFrameCount) {
    // Scan the buffer for clip headers.
    *actualFrameCount = frameCount;
    while (buffer != nullptr && frameCount > 0) {
        ApeHeader* apeHeader = nullptr;
        void* prevBuffer = buffer;
        buffer = findApeHeader(prevBuffer, frameCount * mFrameSizeBytes, &apeHeader);
        if (buffer != nullptr && apeHeader != nullptr) {
            // Frame count does not include the size of the header data.
            const size_t headerSizeFrames =
                    (static_cast<uint8_t*>(buffer) - static_cast<uint8_t*>(prevBuffer)) /
                    mFrameSizeBytes;
            frameCount -= headerSizeFrames;
            *actualFrameCount = frameCount;
            // Stage the clip duration into the DSP worker's queue.
            const int64_t clipDurationFrames = getApeClipDurationFrames(apeHeader);
            const int32_t clipSampleRate = apeHeader->sampleRate;
            LOG(DEBUG) << __func__ << ": found APE clip of " << clipDurationFrames << " frames, "
                       << "sample rate: " << clipSampleRate;
            if (clipSampleRate == mState.sampleRate) {
                std::lock_guard l(mState.lock);
                if (!mState.clips.add(clipDurationFrames)) {
                    LOG(ERROR) << __func__
                               << ": not ready for the next clip, clips: " << mState.clips.log();
                    return ::android::INVALID_OPERATION;
                }
            } else {
                LOG(ERROR) << __func__ << ": clip sample rate " << clipSampleRate
                           << " does not match stream sample rate " << mState.sampleRate;
                return ::android::BAD_VALUE;
            }
        } else {
            frameCount = 0;
        }
    }
    return ::android::OK;
}

::android::status_t DriverOffloadStubImpl::handleMpegTransfer(void* buffer, size_t frameCount,
                                                              size_t* actualFrameCount) {
    const size_t bufferSizeBytes = frameCount * mFrameSizeBytes;
    const uint8_t* currentPtr = static_cast<const uint8_t*>(buffer);
    const uint8_t* const endPtr = currentPtr + bufferSizeBytes;
    const uint8_t* const beginPtr = currentPtr;
    if (mMpegFrameState.bytesPending > 0) {
        size_t processBytes =
                std::min(mMpegFrameState.bytesPending, static_cast<size_t>(endPtr - currentPtr));
        currentPtr += processBytes;
        mMpegFrameState.bytesPending -= processBytes;
    }
    while (currentPtr < endPtr) {
        const uint8_t* const frameBeginning = currentPtr;
        std::optional<MpegFrame> frameOpt = findMpegFrame(&currentPtr, endPtr);
        if (!frameOpt.has_value()) {
            // Could not find a header in the input buffer.
            break;
        }

        const MpegFrame& frame = frameOpt.value();
        LOG(DEBUG) << __func__ << ": Found at offset " << frameBeginning - beginPtr << " " << frame;
        if (frame.isID3v1) {
            mMpegFrameState.clipEnded = true;
        } else {
            if (frame.sampleRate != mState.sampleRate) {
                LOG(ERROR) << __func__ << ": clip sample rate " << frame.sampleRate
                           << " does not match stream sample rate " << mState.sampleRate;
                return ::android::BAD_VALUE;
            }
            std::lock_guard l(mState.lock);
            if (frame.isID3v2 || mState.clips.empty() || mMpegFrameState.clipEnded) {
                if (!mState.clips.add(0)) {
                    LOG(ERROR) << __func__
                               << ": not ready for the next clip, clips: " << mState.clips.log();
                    return ::android::INVALID_OPERATION;
                }
                mMpegFrameState.clipEnded = false;
            }
            mState.clips.updateLastFrames(frame.frameSize);
        }
        if (currentPtr == endPtr) {
            mMpegFrameState.bytesPending = frame.bytesPending;
            break;
        }
    }
    *actualFrameCount = static_cast<size_t>(currentPtr - beginPtr) / mFrameSizeBytes;
    return ::android::OK;
}

::android::status_t DriverOffloadStubImpl::handlePcmTransfer(void* /*buffer*/, size_t frameCount,
                                                             size_t* actualFrameCount) {
    *actualFrameCount = frameCount;
    std::lock_guard l(mState.lock);
    mState.clips.updateLastFrames(frameCount);
    return ::android::OK;
}

void DriverOffloadStubImpl::shutdown() {
    LOG(DEBUG) << __func__ << ": stopping the DSP simulator worker";
    mDspWorker.stop();
    DriverStubImpl::shutdown();
}

::android::status_t DriverOffloadStubImpl::startWorkerIfNeeded() {
    if (!mDspWorkerStarted) {
        // This is an "audio service thread," must have elevated priority.
        if (!mDspWorker.start("dsp_sim", ANDROID_PRIORITY_URGENT_AUDIO)) {
            return ::android::NO_INIT;
        }
        mDspWorkerStarted = true;
    }
    return ::android::OK;
}

::android::status_t DriverOffloadStubImpl::refinePosition(StreamDescriptor::Position* position) {
    if (mState.format.type != AudioFormatType::PCM) {
        return DriverStubImpl::refinePosition(position);
    }
    std::lock_guard l(mState.lock);
    position->frames = mState.mTotalFramesPlayed;
    return ::android::OK;
}

// static
const std::set<std::string>& StreamOffloadStub::getSupportedEncodings() {
    static const std::set<std::string> kSupportedEncodings = {
            "audio/x-ape",
            "audio/mpeg",
    };
    return kSupportedEncodings;
}

StreamOffloadStub::StreamOffloadStub(StreamContext* context, const Metadata& metadata)
    : StreamCommonImpl(context, metadata), DriverOffloadStubImpl(getContext()) {}

StreamOffloadStub::~StreamOffloadStub() {
    cleanupWorker();
}

StreamOutOffloadStub::StreamOutOffloadStub(StreamContext&& context,
                                           const SourceMetadata& sourceMetadata,
                                           const std::optional<AudioOffloadInfo>& offloadInfo)
    : StreamOut(std::move(context), offloadInfo),
      StreamOffloadStub(&mContextInstance, sourceMetadata) {}

}  // namespace aidl::android::hardware::audio::core
