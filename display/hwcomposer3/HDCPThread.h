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
    HDCPThread(Display* display);
    virtual ~HDCPThread();

    HDCPThread(const HDCPThread&) = delete;
    HDCPThread& operator=(const HDCPThread&) = delete;

    HDCPThread(HDCPThread&&) = delete;
    HDCPThread& operator=(HDCPThread&&) = delete;

    HWC3::Error start();

    using HdcpThreadCallback = std::function<void (Display*)>;

    HWC3::Error setCallbacks(const HdcpThreadCallback& callback);

    HWC3::Error setHdcpThreadEnabled(bool enabled);

    HWC3::Error getHdcpLevels(HdcpLevels& levels);

    void updateHdcpLevels(std::string hdcp_cap_result,
                          std::string hdcp_ver_result);

    using HdcpChangedCallback = std::function<void(long /* displayId */,
                                                   bool state,
                                                   HdcpLevels /* levels */)>;
    HWC3::Error setHdcpChangedCallback(const HdcpChangedCallback& callback);
    void setHdcpState(bool state, bool isPrimary);

private:
    HWC3::Error stop();

    void threadLoop();

    const int64_t mHwcId;

    Display* mDisplay = nullptr;

    std::thread mThread;

    std::mutex mStateMutex;

    std::atomic<bool> mShuttingDown{false};

    std::optional<HdcpThreadCallback> mCallbacks;
    std::optional<HdcpChangedCallback> mHdcpChangedCallbacks;

    bool mStarted = false;
    bool mThreadEnabled = false;
    std::string mHdcpStatusPath;
    std::string mHdcpCapPath;
    std::string mHdcpVersionPath;
    std::regex mPattern;
    bool mHdcpState = false;
    HdcpLevels mLevels = {.connectedLevel = HdcpLevel::HDCP_NONE,
                          .maxLevel = HdcpLevel::HDCP_NONE};

    enum KHdcp_Version: uint8_t {
        HDCP_TX_2 = 0,
        HDCP_TX_1,
        HDCP_TX_BOTH,
    };

    std::chrono::time_point<std::chrono::system_clock> mHdcpStartTime;

};

} // namespace aidl::android::hardware::graphics::composer3::impl

#endif
