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

#pragma once

#include <aidl/android/hardware/audio/core/BnBluetooth.h>
#include <aidl/android/hardware/audio/core/BnBluetoothA2dp.h>
#include <aidl/android/hardware/audio/core/BnBluetoothLe.h>
#include <core-impl/AudioCardManager.h>
#include <tinyalsa/asoundlib.h>
#include <audio_utils/resampler.h>

namespace aidl::android::hardware::audio::core {

class ParamChangeHandler {
  public:
    ParamChangeHandler() = default;
    void registerHandler(std::function<ndk::ScopedAStatus()> handler) { mHandler = handler; }

  protected:
    std::function<ndk::ScopedAStatus()> mHandler = nullptr;
};

class Bluetooth : public BnBluetooth {
  public:
    Bluetooth();

  private:
    ndk::ScopedAStatus setScoConfig(const ScoConfig& in_config, ScoConfig* _aidl_return) override;
    ndk::ScopedAStatus setHfpConfig(const HfpConfig& in_config, HfpConfig* _aidl_return) override;

    ScoConfig mScoConfig;
    HfpConfig mHfpConfig;

  private:
    struct pcm_config pcm_config_sco;
    struct pcm_config pcm_config_speaker;
    struct pcm *pcm_speaker_out = NULL;
    struct pcm *pcm_mic_in = NULL;
    struct pcm *pcm_sco_out = NULL;
    struct pcm *pcm_sco_in = NULL;

    struct resampler_itfe *mUplinkResampler = NULL;
    int16_t *mUplinkResamplerBuffer = NULL;

    struct resampler_itfe *mDownlinkResampler = NULL;
    int16_t *mDownlinkResamplerBuffer = NULL;

    pthread_t tid_uplink = 0;
    pthread_t tid_downlink = 0;
    bool uplink_running = false;
    bool downlink_running = false;

    void startHfp();
    void stopHfp();
    static void *uplink_task(void *arg);
    void *uplink_task_impl();
    static void *downlink_task(void *arg);
    void *downlink_task_impl();
    int openPcmForDevice(const audio_devices_t& audioDevice, unsigned int flags,
        struct pcm_config *config, struct pcm** pcm);
};

class BluetoothA2dp : public BnBluetoothA2dp, public ParamChangeHandler {
  public:
    BluetoothA2dp() = default;
    ndk::ScopedAStatus isEnabled(bool* _aidl_return) override;

  private:
    ndk::ScopedAStatus setEnabled(bool in_enabled) override;
    ndk::ScopedAStatus supportsOffloadReconfiguration(bool* _aidl_return) override;
    ndk::ScopedAStatus reconfigureOffload(
            const std::vector<::aidl::android::hardware::audio::core::VendorParameter>&
                    in_parameters) override;

    bool mEnabled = false;
};

class BluetoothLe : public BnBluetoothLe, public ParamChangeHandler {
  public:
    BluetoothLe() = default;
    ndk::ScopedAStatus isEnabled(bool* _aidl_return) override;

  private:
    ndk::ScopedAStatus setEnabled(bool in_enabled) override;
    ndk::ScopedAStatus supportsOffloadReconfiguration(bool* _aidl_return) override;
    ndk::ScopedAStatus reconfigureOffload(
            const std::vector<::aidl::android::hardware::audio::core::VendorParameter>&
                    in_parameters) override;

    bool mEnabled = false;
};

}  // namespace aidl::android::hardware::audio::core
