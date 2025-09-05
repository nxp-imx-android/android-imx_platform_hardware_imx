/*
 * Copyright (C) 2022 The Android Open Source Project
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

#define LOG_TAG "AHAL_Bluetooth"
#include <android-base/logging.h>
#include <audio_utils/primitives.h>

#include "core-impl/Bluetooth.h"

using aidl::android::hardware::audio::core::VendorParameter;
using aidl::android::media::audio::common::Boolean;
using aidl::android::media::audio::common::Float;
using aidl::android::media::audio::common::Int;

namespace aidl::android::hardware::audio::core {

Bluetooth::Bluetooth() {
    mScoConfig.isEnabled = Boolean{false};
    mScoConfig.isNrecEnabled = Boolean{false};
    mScoConfig.mode = ScoConfig::Mode::SCO;
    mHfpConfig.isEnabled = Boolean{false};
    mHfpConfig.sampleRate = Int{8000};
    mHfpConfig.volume = Float{HfpConfig::VOLUME_MAX};

    /* speaker and mic shares the same pcm_config: 48000, 2 channel, 16 bit */
    #define PCM_CONFIG_SPEAKER_PERIOD_BYTES 4
    pcm_config_speaker = {
        .channels = 2,
        .rate = 48000,
        .period_size = 192,
        .period_count = 4,
        .format = PCM_FORMAT_S16_LE,
        .start_threshold = 0,
        .avail_min = 0,
    };

    pcm_config_sco = {
        .channels = 1,
        .rate = 16000,
        .period_size = 64,
        .period_count = 4,
        .format = PCM_FORMAT_S16_LE,
        .start_threshold = 0,
        .avail_min = 0,
    };
}

ndk::ScopedAStatus Bluetooth::setScoConfig(const ScoConfig& in_config, ScoConfig* _aidl_return) {
    if (in_config.isEnabled.has_value()) {
        mScoConfig.isEnabled = in_config.isEnabled;
    }
    if (in_config.isNrecEnabled.has_value()) {
        mScoConfig.isNrecEnabled = in_config.isNrecEnabled;
    }
    if (in_config.mode != ScoConfig::Mode::UNSPECIFIED) {
        mScoConfig.mode = in_config.mode;
    }
    if (in_config.debugName.has_value()) {
        mScoConfig.debugName = in_config.debugName;
    }
    *_aidl_return = mScoConfig;
    LOG(DEBUG) << __func__ << ": received " << in_config.toString() << ", returning "
               << _aidl_return->toString();
    return ndk::ScopedAStatus::ok();
}

int Bluetooth::openPcmForDevice(const audio_devices_t& audioDevice, unsigned int flags,
        struct pcm_config *config, struct pcm** pcm)
{
    if (!config || !pcm) {
        return -EINVAL;
    }
    auto card = AudioCardManager::getCardForDevice(audioDevice);
    if (!card) {
        LOG(ERROR) << "card is not found";
        return -EINVAL;
    }

    *pcm = pcm_open(card->card, 0, flags, config);
    if (!*pcm) {
        LOG(ERROR) << "pcm_open() failed: " << pcm_get_error(*pcm);
        return -EBUSY;
    }
    if (!pcm_is_ready(*pcm)) {
        LOG(ERROR) << "pcm_is_ready() failed: " << pcm_get_error(*pcm);
        pcm_close(*pcm);
        *pcm = NULL;
        return -EBUSY;
    }

    LOG(INFO) << "  channels: " << config->channels;
    LOG(INFO) << "  rate: " << config->rate;
    LOG(INFO) << "  period_size: " << config->period_size;
    LOG(INFO) << "  period_count: " << config->period_count;
    LOG(INFO) << "  format: " << config->format;
    return 0;
}

void Bluetooth::stopHfp() {
    LOG(DEBUG) << __func__;
    if (tid_uplink) {
        uplink_running = false;
        pthread_join(tid_uplink, NULL);
        tid_uplink = 0;
    }

    if (tid_downlink) {
        downlink_running = false;
        pthread_join(tid_downlink, NULL);
        tid_downlink = 0;
    }

    if (pcm_speaker_out) {
        pcm_close(pcm_speaker_out);
        pcm_speaker_out = NULL;
    }

    if (pcm_mic_in) {
        pcm_close(pcm_mic_in);
        pcm_mic_in = NULL;
    }

    if (pcm_sco_out) {
        pcm_close(pcm_sco_out);
        pcm_sco_out = NULL;
    }

    if (pcm_sco_in) {
        pcm_close(pcm_sco_in);
        pcm_sco_in = NULL;
    }

    if (mDownlinkResampler) {
        release_resampler(mDownlinkResampler);
        mDownlinkResampler = NULL;
        if (mDownlinkResamplerBuffer) {
            free(mDownlinkResamplerBuffer);
            mDownlinkResamplerBuffer = NULL;
        }
    }

    if (mUplinkResampler) {
        release_resampler(mUplinkResampler);
        mUplinkResampler = NULL;
        if (mUplinkResamplerBuffer) {
            free(mUplinkResamplerBuffer);
            mUplinkResamplerBuffer = NULL;
        }
    }
}

void* Bluetooth::uplink_task(void* arg) {
    Bluetooth* instance = static_cast<Bluetooth*>(arg);
    instance->uplink_task_impl();
    return nullptr;
}

void *Bluetooth::uplink_task_impl()
{
    int ret = 0;
    size_t frameCount = pcm_config_speaker.period_size;
    size_t bytesMic = pcm_config_speaker.period_size * PCM_CONFIG_SPEAKER_PERIOD_BYTES;
    size_t bytesSco = pcm_frames_to_bytes(pcm_sco_out, pcm_config_sco.period_size);
    size_t inFrameCount = 0;
    size_t outFrameCount = 0;

    auto card = AudioCardManager::getCardForDevice(AUDIO_DEVICE_IN_BUILTIN_MIC);
    if (!card) {
        LOG(ERROR) << "card is not found";
        return NULL;
    }

    void *bufferMic = malloc(bytesMic);
    if (!bufferMic) {
        LOG(ERROR) << __func__ << "Failed to alloc " << bytesMic << " bytes";
        return NULL;
    }

    void *bufferSco = malloc(bytesSco);
    if (!bufferSco) {
        LOG(ERROR) << __func__ << "Failed to alloc " << bytesSco << " bytes";
        free(bufferMic);
        return NULL;
    }

    LOG(INFO) << __func__ << " start";
    while (uplink_running) {
        /* input priority: HFP(1) > PRIMARY(2) > NONE(0) */
        if (card->inOwner == OWNER_HFP) {
            if (!pcm_mic_in) {
                ret = openPcmForDevice(AUDIO_DEVICE_IN_BUILTIN_MIC, PCM_IN,
                        &pcm_config_speaker, &pcm_mic_in);
                if (ret) {
                    usleep(5000);
                    continue;
                }
            }
        } else {
            std::unique_lock lock(card->mLock);
            card->inOwner = OWNER_HFP;
            continue;
        }

        ret = pcm_read(pcm_mic_in, bufferMic, bytesMic);
        if (ret) {
            LOG(ERROR) << __func__ << " pcm read failed: ret: " << ret << " " << pcm_get_error(pcm_mic_in);
            usleep(5000);
            continue;
        }

        inFrameCount = frameCount;
        outFrameCount = frameCount;
        if (pcm_config_sco.rate != pcm_config_speaker.rate && mUplinkResampler) {
            mUplinkResampler->resample_from_input(mUplinkResampler,
                    (int16_t *)bufferMic, &inFrameCount,
                    (int16_t *)mUplinkResamplerBuffer, &outFrameCount);
            if (inFrameCount != frameCount) {
                LOG(ERROR) << __func__ << " resampler does not consume all input data: in "
                    << inFrameCount << " out " << outFrameCount;
            }
            downmix_to_mono_i16_from_stereo_i16((int16_t*)bufferSco, (const int16_t*)mUplinkResamplerBuffer, outFrameCount);
        } else {
            downmix_to_mono_i16_from_stereo_i16((int16_t*)bufferSco, (const int16_t*)bufferMic, frameCount);
        }

        ret = pcm_write(pcm_sco_out, bufferSco, pcm_frames_to_bytes(pcm_sco_out, outFrameCount));
        if (ret) {
            LOG(ERROR) << __func__ << " pcm write failed: ret: " << ret << " " << pcm_get_error(pcm_sco_out);
            usleep(5000);
            continue;
        }
    }

    free(bufferMic);
    free(bufferSco);
    if (card->inOwner == OWNER_HFP) {
        card->inOwner = OWNER_NONE;
    }
    LOG(INFO) << __func__ << " stop";
    return NULL;
}

void* Bluetooth::downlink_task(void* arg) {
    Bluetooth* instance = static_cast<Bluetooth*>(arg);
    instance->downlink_task_impl();
    return nullptr;
}

void *Bluetooth::downlink_task_impl()
{
    int ret = 0;
    size_t frameCount = pcm_config_sco.period_size;
    size_t bytesSpeaker = pcm_config_speaker.period_size * PCM_CONFIG_SPEAKER_PERIOD_BYTES;
    size_t bytesSco = pcm_frames_to_bytes(pcm_sco_in, pcm_config_sco.period_size);
    size_t inFrameCount = 0;
    size_t outFrameCount = 0;

    auto card = AudioCardManager::getCardForDevice(AUDIO_DEVICE_OUT_SPEAKER);
    if (!card) {
        LOG(ERROR) << "card is not found";
        return NULL;
    }

    void *bufferSpeaker = malloc(bytesSpeaker);
    if (!bufferSpeaker) {
        LOG(ERROR) << __func__ << "Failed to alloc " << bytesSpeaker << " bytes";
        return NULL;
    }

    void *bufferSco = malloc(bytesSco);
    if (!bufferSco) {
        LOG(ERROR) << __func__ << "Failed to alloc " << bytesSco << " bytes";
        free(bufferSpeaker);
        return NULL;
    }

    LOG(INFO) << __func__ << " start";
    while (downlink_running) {
        /* output priority: DIRECT(3) > PRIMARY(2) > HFP(1) > NONE(0) */
        if (card->outOwner > OWNER_HFP) {
            if (pcm_speaker_out) {
                pcm_close(pcm_speaker_out);
                pcm_speaker_out = NULL;
                LOG(ERROR) << __func__ << " standby hfp speaker";
            }
            usleep(20000);
            continue;
        } else if (card->outOwner == OWNER_HFP) {
            if (!pcm_speaker_out) {
                ret = openPcmForDevice(AUDIO_DEVICE_OUT_SPEAKER, PCM_OUT,
                        &pcm_config_speaker, &pcm_speaker_out);
                if (ret) {
                    usleep(5000);
                    continue;
                }
            }
        } else {
            std::unique_lock lock(card->mLock);
            card->outOwner = OWNER_HFP;
            continue;
        }
        ret = pcm_read(pcm_sco_in, bufferSco, bytesSco);
        if (ret) {
            LOG(ERROR) << __func__ << " pcm read failed: ret: " << ret << " " << pcm_get_error(pcm_sco_in);
            usleep(5000);
            continue;
        }

        inFrameCount = frameCount;
        outFrameCount = frameCount * (pcm_config_speaker.rate / pcm_config_sco.rate);
        if (pcm_config_sco.rate != pcm_config_speaker.rate && mDownlinkResampler) {
            mDownlinkResampler->resample_from_input(mDownlinkResampler,
                    (int16_t *)bufferSco, &inFrameCount,
                    (int16_t *)mDownlinkResamplerBuffer, &outFrameCount);
            if (inFrameCount != frameCount) {
                LOG(ERROR) << __func__ << " resampler does not consume all input data: in "
                    << inFrameCount << " out " << outFrameCount;
            }
            upmix_to_stereo_i16_from_mono_i16((int16_t*)bufferSpeaker, (const int16_t*)mDownlinkResamplerBuffer, outFrameCount);
        } else {
            upmix_to_stereo_i16_from_mono_i16((int16_t*)bufferSpeaker, (const int16_t*)bufferSco, frameCount);
        }

        ret = pcm_write(pcm_speaker_out, bufferSpeaker, pcm_frames_to_bytes(pcm_speaker_out, outFrameCount));
        if (ret) {
            LOG(ERROR) << __func__ << " pcm write failed: ret: " << ret << " "  << pcm_get_error(pcm_speaker_out);
            usleep(5000);
            continue;
        }
    }

    free(bufferSpeaker);
    free(bufferSco);
    if (card->outOwner == OWNER_HFP) {
        card->outOwner = OWNER_NONE;
    }
    LOG(INFO) << __func__ << " stop";
    return NULL;
}

void Bluetooth::startHfp() {
    LOG(DEBUG) << __func__;
    int ret = 0;

    int ratio = pcm_config_speaker.rate / pcm_config_sco.rate;
    if (!ratio) {
        LOG(ERROR) << __func__ << " wrong rates, speaker: " <<
            pcm_config_speaker.rate << ", sco: " << pcm_config_sco.rate;
        return;
    }
    pcm_config_sco.period_size = pcm_config_speaker.period_size / ratio;

    ret = openPcmForDevice(AUDIO_DEVICE_OUT_BLUETOOTH_SCO, PCM_OUT,
                &pcm_config_sco, &pcm_sco_out);
    if (ret)
        goto error;

    ret = openPcmForDevice(AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET, PCM_IN,
                &pcm_config_sco, &pcm_sco_in);
    if (ret)
        goto error;

    if (pcm_config_sco.rate != pcm_config_speaker.rate) {
        ret = create_resampler(
                pcm_config_sco.rate, pcm_config_speaker.rate, pcm_config_sco.channels,
                RESAMPLER_QUALITY_DEFAULT,
                NULL,                      /* resampler_buffer_provider */
                &mDownlinkResampler);
        if (ret) {
            LOG(ERROR) << "Resampler initialization failed! Error code " << ret;
            goto error;
        }
        size_t size = pcm_frames_to_bytes(pcm_sco_in, pcm_config_sco.period_size * ratio);
        mDownlinkResamplerBuffer = (int16_t *)malloc(size);
        if (!mDownlinkResamplerBuffer) {
            LOG(ERROR) << "Resampler buffer initialization failed!";
            goto error;
        }

        LOG(INFO) << __func__ << ": created downlink resampler from " <<
            pcm_config_sco.rate << " to " << pcm_config_speaker.rate <<
            ", channel " << pcm_config_sco.channels << ", buffer size " << size;

        ret = create_resampler(
                pcm_config_speaker.rate, pcm_config_sco.rate, pcm_config_speaker.channels,
                RESAMPLER_QUALITY_DEFAULT,
                NULL,                      /* resampler_buffer_provider */
                &mUplinkResampler);
        if (ret) {
            LOG(ERROR) << "Resampler initialization failed! Error code " << ret;
            goto error;
        }
        size = pcm_config_speaker.period_size / ratio * PCM_CONFIG_SPEAKER_PERIOD_BYTES;
        mUplinkResamplerBuffer = (int16_t *)malloc(size);
        if (!mUplinkResamplerBuffer) {
            LOG(ERROR) << "Resampler buffer initialization failed!";
            goto error;
        }

        LOG(INFO) << __func__ << ": created uplink resampler from " <<
            pcm_config_speaker.rate << " to " << pcm_config_sco.rate <<
            ", channel " << pcm_config_speaker.channels << ", buffer size " << size;
    }

    pthread_attr_t attr;
    struct sched_param schParam;

    pthread_attr_init(&attr);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    schParam.sched_priority = 3;
    pthread_attr_setschedparam(&attr, &schParam);

    uplink_running = true;
    ret = pthread_create(&tid_uplink, &attr, &uplink_task, this);
    if (ret) {
        goto error;
    }

    downlink_running = true;
    ret = pthread_create(&tid_downlink, &attr, &downlink_task, this);
    if (ret) {
        goto error;
    }

    return;
error:
    stopHfp();
    return;
}

ndk::ScopedAStatus Bluetooth::setHfpConfig(const HfpConfig& in_config, HfpConfig* _aidl_return) {
    if (in_config.sampleRate.has_value() && in_config.sampleRate.value().value <= 0) {
        LOG(ERROR) << __func__ << ": invalid sample rate: " << in_config.sampleRate.value().value;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (in_config.volume.has_value() && (in_config.volume.value().value < HfpConfig::VOLUME_MIN ||
                                         in_config.volume.value().value > HfpConfig::VOLUME_MAX)) {
        LOG(ERROR) << __func__ << ": invalid volume: " << in_config.volume.value().value;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    if (in_config.isEnabled.has_value()) {
        mHfpConfig.isEnabled = in_config.isEnabled;
        if (mHfpConfig.isEnabled == Boolean{true})
            startHfp();
        else
            stopHfp();

    }
    if (in_config.sampleRate.has_value()) {
        mHfpConfig.sampleRate = in_config.sampleRate;
        pcm_config_sco.rate = mHfpConfig.sampleRate.value().value;
    }
    if (in_config.volume.has_value()) {
        mHfpConfig.volume = in_config.volume;
    }
    *_aidl_return = mHfpConfig;
    LOG(DEBUG) << __func__ << ": received " << in_config.toString() << ", returning "
               << _aidl_return->toString();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothA2dp::isEnabled(bool* _aidl_return) {
    *_aidl_return = mEnabled;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothA2dp::setEnabled(bool in_enabled) {
    mEnabled = in_enabled;
    LOG(DEBUG) << __func__ << ": " << mEnabled;
    if (mHandler) return mHandler();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothA2dp::supportsOffloadReconfiguration(bool* _aidl_return) {
    *_aidl_return = false;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothA2dp::reconfigureOffload(
        const std::vector<::aidl::android::hardware::audio::core::VendorParameter>& in_parameters
                __unused) {
    LOG(DEBUG) << __func__ << ": " << ::android::internal::ToString(in_parameters);
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus BluetoothLe::isEnabled(bool* _aidl_return) {
    *_aidl_return = mEnabled;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothLe::setEnabled(bool in_enabled) {
    mEnabled = in_enabled;
    if (mHandler) return mHandler();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothLe::supportsOffloadReconfiguration(bool* _aidl_return) {
    *_aidl_return = false;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothLe::reconfigureOffload(
        const std::vector<::aidl::android::hardware::audio::core::VendorParameter>& in_parameters
                __unused) {
    LOG(DEBUG) << __func__ << ": " << ::android::internal::ToString(in_parameters);
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

}  // namespace aidl::android::hardware::audio::core
