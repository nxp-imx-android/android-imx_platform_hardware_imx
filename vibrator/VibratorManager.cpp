/*
 * Copyright (C) 2020 The Android Open Source Project
 * Copyright 2026 NXP
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

#include "vibrator-impl/VibratorManager.h"
#include "vibrator-impl/VibrationSession.h"

#include <aidl/android/hardware/vibrator/BnVibratorCallback.h>
#include <aidl/android/media/audio/common/AudioChannelLayout.h>
#include <aidl/android/media/audio/common/AudioConfigBase.h>
#include <aidl/android/media/audio/common/AudioFormatDescription.h>
#include <aidl/android/media/audio/common/AudioFormatType.h>
#include <aidl/android/media/audio/common/PcmType.h>
#include <android-base/logging.h>
#include <chrono>
#include <thread>

using namespace ::std::chrono_literals;
using ::aidl::android::media::audio::common::AudioChannelLayout;
using ::aidl::android::media::audio::common::AudioConfigBase;
using ::aidl::android::media::audio::common::AudioFormatDescription;
using ::aidl::android::media::audio::common::AudioFormatType;
using ::aidl::android::media::audio::common::PcmType;

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

void clearHapticGeneratorSessions(
        std::vector<std::unique_ptr<HapticGeneratorSessionState>>& hgSessions);

static constexpr int32_t kDefaultVibratorId = 1;
static constexpr size_t kFmqCommandQueueSize = 16;
static constexpr size_t kFmqEffectQueueSize = 16;
static constexpr size_t kFmqReplyQueueSize = 16;
static constexpr size_t kFmqPcmQueueSize = 1024;
static constexpr size_t kSimulatedEffectSize = 8192;

const auto kDefaultFmqTimeoutNanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(100ms).count();

class VibratorCallback : public BnVibratorCallback {
  public:
    VibratorCallback(int32_t delayMs, std::shared_ptr<IVibrationSession> session,
                     std::shared_ptr<VibratorManager> manager)
        : mDelayMs(delayMs), mSession(std::move(session)), mManager(std::move(manager)) {}
    ndk::ScopedAStatus onComplete() override {
        LOG(VERBOSE) << "Closing session after vibrator became idle";
        std::this_thread::sleep_for(std::chrono::milliseconds(mDelayMs));
        if (mManager) {
            mManager->clearSession(mSession);
        }
        return ndk::ScopedAStatus::ok();
    }

  private:
    const int32_t mDelayMs;
    std::shared_ptr<IVibrationSession> mSession;
    std::shared_ptr<VibratorManager> mManager;
};

ndk::ScopedAStatus VibratorManager::getCapabilities(int32_t* _aidl_return) {
    LOG(VERBOSE) << "Vibrator manager reporting capabilities";
    std::lock_guard lock(mMutex);
    if (mCapabilities == 0) {
        int32_t version;
        if (!getInterfaceVersion(&version).isOk()) {
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_STATE));
        }
        mCapabilities = IVibratorManager::CAP_SYNC | IVibratorManager::CAP_PREPARE_ON |
                        IVibratorManager::CAP_PREPARE_PERFORM |
                        IVibratorManager::CAP_PREPARE_COMPOSE |
                        IVibratorManager::CAP_MIXED_TRIGGER_ON |
                        IVibratorManager::CAP_MIXED_TRIGGER_PERFORM |
                        IVibratorManager::CAP_MIXED_TRIGGER_COMPOSE |
                        IVibratorManager::CAP_TRIGGER_CALLBACK;

        if (version >= 3) {
            mCapabilities |= IVibratorManager::CAP_START_SESSIONS;
        }
        if (version >= 4) {
            mCapabilities |= IVibratorManager::CAP_HAPTIC_GENERATOR;
        }
    }

    *_aidl_return = mCapabilities;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::getVibratorIds(std::vector<int32_t>* _aidl_return) {
    LOG(VERBOSE) << "Vibrator manager getting vibrator ids";
    *_aidl_return = {kDefaultVibratorId};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::getVibrator(int32_t vibratorId,
                                                std::shared_ptr<IVibrator>* _aidl_return) {
    LOG(VERBOSE) << "Vibrator manager getting vibrator " << vibratorId;
    if (vibratorId == kDefaultVibratorId) {
        *_aidl_return = mDefaultVibrator;
        return ndk::ScopedAStatus::ok();
    } else {
        *_aidl_return = nullptr;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
}

ndk::ScopedAStatus VibratorManager::prepareSynced(const std::vector<int32_t>& vibratorIds) {
    LOG(VERBOSE) << "Vibrator Manager prepare synced";
    if (vibratorIds.size() != 1 || vibratorIds[0] != kDefaultVibratorId) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::lock_guard lock(mMutex);
    if (mIsPreparing) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    mIsPreparing = true;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::triggerSynced(
        const std::shared_ptr<IVibratorCallback>& callback) {
    LOG(VERBOSE) << "Vibrator Manager trigger synced";
    {
        std::lock_guard lock(mMutex);
        if (!mIsPreparing) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }
        mIsPreparing = false;
    }
    if (callback) {
        std::thread([callback] {
            LOG(VERBOSE) << "Notifying perform complete";
            callback->onComplete();
        }).detach();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::cancelSynced() {
    LOG(VERBOSE) << "Vibrator Manager cancel synced";
    std::lock_guard lock(mMutex);
    mIsPreparing = false;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::startSession(const std::vector<int32_t>& vibratorIds,
                                                 const VibrationSessionConfig&,
                                                 const std::shared_ptr<IVibratorCallback>& callback,
                                                 std::shared_ptr<IVibrationSession>* _aidl_return) {
    LOG(VERBOSE) << "Vibrator Manager start session";
    *_aidl_return = nullptr;
    int32_t capabilities = 0;
    if (!getCapabilities(&capabilities).isOk()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if ((capabilities & IVibratorManager::CAP_START_SESSIONS) == 0) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }
    if (vibratorIds.size() != 1 || vibratorIds[0] != kDefaultVibratorId) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::lock_guard lock(mMutex);
    if (mIsPreparing || mSession) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    mSessionCallback = std::shared_ptr<IVibratorCallback>(callback);  // Keep a separate reference.
    mSession = ndk::SharedRefBase::make<VibrationSession>(this->ref<VibratorManager>());
    *_aidl_return = std::shared_ptr<IVibrationSession>(mSession);  // Return a separate reference.
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::clearSessions() {
    LOG(VERBOSE) << "Vibrator Manager clear sessions";
    int32_t capabilities = 0;
    if (!getCapabilities(&capabilities).isOk()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (!(capabilities & IVibratorManager::CAP_START_SESSIONS) &&
        !(capabilities & IVibratorManager::CAP_HAPTIC_GENERATOR)) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    if (capabilities & IVibratorManager::CAP_START_SESSIONS) {
        abortSession();
    }

    if (capabilities & IVibratorManager::CAP_HAPTIC_GENERATOR) {
        // Also clear any active haptic generator sessions
        std::vector<std::unique_ptr<HapticGeneratorSessionState>> hgSessions;
        {
            std::lock_guard lock(mMutex);
            hgSessions = std::move(mHapticGeneratorSessions);
        }
        clearHapticGeneratorSessions(hgSessions);
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::startHapticGeneratorSession(
        const std::vector<int32_t>& vibratorIds, const HapticGeneratorConfig& config,
        const std::shared_ptr<IVibratorCallback>& callback, HapticGeneratorSession* _aidl_return) {
    LOG(VERBOSE) << "Vibrator Manager start haptic generator session";
    int32_t capabilities = 0;
    if (!getCapabilities(&capabilities).isOk()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if ((capabilities & IVibratorManager::CAP_HAPTIC_GENERATOR) == 0) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }
    if (vibratorIds.empty() || vibratorIds[0] != kDefaultVibratorId) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    const auto& format = config.audioFormat;
    if (format.sampleRate <= 0) {
        LOG(ERROR) << "Haptic generator session rejected: Invalid/Unspecified sample rate "
                   << format.sampleRate;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    const auto& channelMask = config.audioFormat.channelMask;
    bool isChannelMaskInvalid = false;

    switch (channelMask.getTag()) {
        case AudioChannelLayout::Tag::layoutMask:
            if (channelMask.get<AudioChannelLayout::Tag::layoutMask>() == 0) {
                isChannelMaskInvalid = true;
            }
            break;
        case AudioChannelLayout::Tag::indexMask:
            if (channelMask.get<AudioChannelLayout::Tag::indexMask>() == 0) {
                isChannelMaskInvalid = true;
            }
            break;
        case AudioChannelLayout::Tag::none:
            isChannelMaskInvalid = true;
            break;
        case AudioChannelLayout::Tag::invalid:
            isChannelMaskInvalid = true;
            break;
        default:
            // Channel mask is valid
            break;
    }

    if (isChannelMaskInvalid) {
        LOG(ERROR) << "Haptic generator session rejected: Invalid/Empty channel mask";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    auto sessionState = std::make_unique<HapticGeneratorSessionState>(callback);

    if (!sessionState->isValid()) {
        LOG(ERROR) << "Failed to create haptic generator message queues";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    HapticGeneratorQueues hgQueues;
    hgQueues.vibratorId = kDefaultVibratorId;
    hgQueues.command = sessionState->commandQueue->dupeDesc();
    hgQueues.effect = sessionState->effectQueue->dupeDesc();
    hgQueues.reply = sessionState->replyQueue->dupeDesc();
    hgQueues.pcm = sessionState->pcmQueue->dupeDesc();

    *_aidl_return = {};
    _aidl_return->queues.push_back(std::move(hgQueues));

    {
        std::lock_guard lock(mMutex);
        mHapticGeneratorSessions.push_back(std::move(sessionState));
    }

    return ndk::ScopedAStatus::ok();
}

void VibratorManager::abortSession() {
    std::shared_ptr<IVibrationSession> session;
    {
        std::lock_guard lock(mMutex);
        session = mSession;
    }
    if (session) {
        mDefaultVibrator->off();
        clearSession(session);
    }
}

void VibratorManager::closeSession(int32_t delayMs) {
    std::shared_ptr<IVibrationSession> session;
    {
        std::lock_guard lock(mMutex);
        session = mSession;
    }
    if (session) {
        auto callback = ndk::SharedRefBase::make<VibratorCallback>(delayMs, session,
                                                                   this->ref<VibratorManager>());
        mDefaultVibrator->setGlobalVibrationCallback(callback);
    }
}

void VibratorManager::clearSession(const std::shared_ptr<IVibrationSession>& session) {
    std::shared_ptr<IVibratorCallback> callback;
    {
        std::lock_guard lock(mMutex);
        if (mSession != session) {
            // Probably a delayed call from an old session that was already cleared, ignore it.
            return;
        }
        callback = std::move(mSessionCallback);
        mSession = nullptr;
        mSessionCallback = nullptr;  // make sure any delayed call will not trigger this again.
    }
    if (callback) {
        std::thread([callback] {
            LOG(VERBOSE) << "Notifying session complete";
            if (!callback->onComplete().isOk()) {
                LOG(ERROR) << "Failed to call onComplete";
            }
        }).detach();
    }
}

void clearHapticGeneratorSessions(
        std::vector<std::unique_ptr<HapticGeneratorSessionState>>& hgSessions) {
    for (auto& hgSession : hgSessions) {
        if (hgSession) {
            hgSession->close();
        }
    }
}

HapticGeneratorSessionState::HapticGeneratorSessionState(
        const std::shared_ptr<IVibratorCallback>& callback)
    : commandQueue(std::make_unique<CommandQueue>(kFmqCommandQueueSize,
                                                  true /* configureEventFlagWord */)),
      effectQueue(std::make_unique<VibrationEffectQueue>(kFmqEffectQueueSize,
                                                         true /* configureEventFlagWord */)),
      pcmQueue(std::make_unique<PcmQueue>(kFmqPcmQueueSize, true /* configureEventFlagWord */)),
      replyQueue(
              std::make_unique<ReplyQueue>(kFmqReplyQueueSize, true /* configureEventFlagWord */)),
      mCallback(callback),
      mSessionThread(&HapticGeneratorSessionState::run, this) {}

HapticGeneratorSessionState::~HapticGeneratorSessionState() {
    close();
}

bool HapticGeneratorSessionState::isValid() const {
    return commandQueue->isValid() && effectQueue->isValid() && replyQueue->isValid() &&
           pcmQueue->isValid();
}

void HapticGeneratorSessionState::close() {
    mStopSession = true;
    if (mSessionThread.joinable()) {
        LOG(VERBOSE) << "Stopping HapticGeneratorSession thread...";
        mSessionThread.join();
        LOG(VERBOSE) << "HapticGeneratorSession thread stopped.";
    }
}

void HapticGeneratorSessionState::handleStartEffect(HapticGeneratorReply* reply) {
    LOG(VERBOSE) << "HapticGenerator: Received startEffect command.";
    // Reset all state flags for a new conversion.
    mIsEffectStarted = true;
    mIsEffectComplete = false;
    mRemainingPcmBytes = 0;
    // Clear any leftover data from a previous effect by draining the effects input queue
    VibrationEffectContent effect;
    while (effectQueue->read(&effect)) {
        // Discard data
    }
    reply->status = 0;
}

void HapticGeneratorSessionState::handleBurstBytes(const HapticGeneratorCommand& command,
                                                   HapticGeneratorReply* reply) {
    if (!mIsEffectStarted) {
        LOG(ERROR) << "HapticGenerator: Received burstBytes before startEffect.";
        reply->status = STATUS_INVALID_OPERATION;
        return;
    }

    VibrationEffectContent effect;
    while (effectQueue->read(&effect)) {
        LOG(VERBOSE) << "HapticGenerator: Consumed one effect part from queue.";
        mRemainingPcmBytes += kSimulatedEffectSize;
    }

    size_t requestedBytes = command.get<HapticGeneratorCommand::Tag::burstBytes>();
    size_t bytesToSend = std::min(requestedBytes, mRemainingPcmBytes);

    if (bytesToSend > 0) {
        // This simulation only writes zeroes. A real implementation should generate PCM data.
        std::vector<int8_t> pcmBuffer(bytesToSend, 0);
        if (!pcmQueue->write(pcmBuffer.data(), bytesToSend)) {
            LOG(ERROR) << "HapticGenerator: Failed to write to PCM queue.";
            reply->status = STATUS_INVALID_OPERATION;
            return;
        }
    }

    if (bytesToSend == 0) {
        // If we are sending 0 bytes, it's only a successful state (STATUS_OK) if the
        // effect was marked as completed. Otherwise, we need more data.
        reply->status = mIsEffectComplete ? STATUS_OK : STATUS_NOT_ENOUGH_DATA;
    }

    mRemainingPcmBytes -= bytesToSend;
    reply->burstBytesReady = bytesToSend;
}

void HapticGeneratorSessionState::handleCompleteEffect(HapticGeneratorReply* reply) {
    LOG(VERBOSE) << "HapticGenerator: Received completeEffect command.";
    if (!mIsEffectStarted) {
        LOG(ERROR) << "HapticGenerator: Received completeEffect command before startEffect.";
        reply->status = STATUS_INVALID_OPERATION;
    } else {
        mIsEffectComplete = true;
        reply->status = 0;
    }
}

void HapticGeneratorSessionState::handleCancelEffect(HapticGeneratorReply* reply) {
    LOG(VERBOSE) << "HapticGenerator: Received cancelEffect command.";
    if (!mIsEffectStarted) {
        LOG(ERROR) << "HapticGenerator: Received cancelEffect command before startEffect.";
        reply->status = STATUS_INVALID_OPERATION;
    } else {
        mIsEffectStarted = false;
        mIsEffectComplete = false;
        mRemainingPcmBytes = 0;
        reply->status = 0;
        // Clear any leftover data from a previous effect by draining the effect input queue
        VibrationEffectContent effect;
        while (effectQueue->read(&effect)) {
            // Discard data
        }
    }
}

void HapticGeneratorSessionState::handleClose(HapticGeneratorReply* reply) {
    LOG(VERBOSE) << "HapticGenerator: Received close command.";
    mIsEffectStarted = false;
    mStopSession = true;
    reply->status = 0;
}

void HapticGeneratorSessionState::run() {
    LOG(INFO) << "HapticGeneratorSession thread started.";

    while (!mStopSession) {
        HapticGeneratorCommand command;
        if (!commandQueue->readBlocking(&command, 1, kDefaultFmqTimeoutNanos)) {
            LOG(ERROR) << __func__ << ": Failed to read command from FMQ";
            // If the read times out, continue the loop to re-check mStopSession.
            continue;
        }

        HapticGeneratorReply reply = {.status = STATUS_OK, .burstBytesReady = 0};

        switch (command.getTag()) {
            case HapticGeneratorCommand::Tag::effect:
                switch (command.get<HapticGeneratorCommand::Tag::effect>()) {
                    case HapticGeneratorCommand::Effect::START:
                        handleStartEffect(&reply);
                        break;
                    case HapticGeneratorCommand::Effect::COMPLETE:
                        handleCompleteEffect(&reply);
                        break;
                    case HapticGeneratorCommand::Effect::CANCEL:
                        handleCancelEffect(&reply);
                        break;
                }
                break;
            case HapticGeneratorCommand::Tag::session:
                switch (command.get<HapticGeneratorCommand::Tag::session>()) {
                    case HapticGeneratorCommand::Session::CLOSE:
                        handleClose(&reply);
                        break;
                }
                break;
            case HapticGeneratorCommand::Tag::burstBytes:
                handleBurstBytes(command, &reply);
                break;
            default:
                LOG(ERROR) << "HapticGenerator: Received invalid command.";
        }

        if (!replyQueue->writeBlocking(&reply, 1, kDefaultFmqTimeoutNanos)) {
            LOG(ERROR) << "HapticGenerator: Failed to write reply.";
        }
    }

    if (mCallback) {
        std::thread([callback = mCallback] {
            LOG(VERBOSE) << "Notifying haptic generator session complete";
            if (!callback->onComplete().isOk()) {
                LOG(ERROR) << "Failed to call onComplete for haptic generator session";
            }
        }).detach();
    }
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
