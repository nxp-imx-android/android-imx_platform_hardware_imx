/*
 * Copyright 2022 The Android Open Source Project
 * Copyright 2024 NXP
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

#ifndef ANDROID_HWC_HDCPTHREAD_H
#define ANDROID_HWC_HDCPTHREAD_H

#include <android/hardware/graphics/common/1.0/types.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <thread>

#include "Common.h"
#include <regex>
#include <android-base/unique_fd.h>
#include <android-base/file.h>
#include <condition_variable>
#include <chrono>

// It is same as kernel space
#define HDCP_CONFIG_NONE    (0)
#define HDCP_CONFIG_1_4     (1)
#define HDCP_CONFIG_2_2     (2)

using aidl::android::hardware::drm::HdcpLevel;
using aidl::android::hardware::drm::HdcpLevels;

namespace aidl::android::hardware::graphics::composer3::impl {

class Display;
class HDCPThread {
public:
    HDCPThread(uint32_t displayId);
    virtual ~HDCPThread();

    HDCPThread(const HDCPThread&) = delete;
    HDCPThread& operator=(const HDCPThread&) = delete;

    HDCPThread(HDCPThread&&) = delete;
    HDCPThread& operator=(HDCPThread&&) = delete;

    HWC3::Error start();

    HWC3::Error setCallbacks(const std::function<void()>& callback);

    HWC3::Error setHdcpThreadEnabled(bool enabled);

    HWC3::Error getHdcpLevels(HdcpLevels& levels);

    void updateHdcpLevels(std::string hdcp_cap_result,
                          std::string hdcp_ver_result);

    HWC3::Error stop();

private:
    uint32_t mDisplayId;

    void threadLoop();

    std::thread mThread;

    std::mutex mStateMutex;

    std::atomic<bool> mShuttingDown{false};

    std::optional<std::function<void()>> mCallbacks;

    bool mStarted = false;
    bool mThreadEnabled = false;
    std::string mHdcpStatusPath;
    std::string mHdcpCapPath;
    std::string mHdcpVersionPath;
    std::regex mPattern;

    enum KHdcp_Version: uint8_t {
        HDCP_TX_2 = 0,
        HDCP_TX_1,
        HDCP_TX_BOTH,
    };
    HdcpLevels mLevels = {.connectedLevel = HdcpLevel::HDCP_NONE, .maxLevel = HdcpLevel::HDCP_NONE};
    std::chrono::time_point<std::chrono::system_clock> mHdcpStartTime;

};

} // namespace aidl::android::hardware::graphics::composer3::impl

#endif
