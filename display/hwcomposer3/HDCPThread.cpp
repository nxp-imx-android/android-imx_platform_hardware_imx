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

#include "HDCPThread.h"
#include <utils/ThreadDefs.h>
#include <thread>
#include "Display.h"

using android::base::ReadFileToString;
using android::base::WriteStringToFd;

namespace aidl::android::hardware::graphics::composer3::impl {

HDCPThread::HDCPThread(uint32_t displayId) : mDisplayId(displayId), mPattern("(\\d+)\\s*:") {
    const std::string hdcpInfoPath = getHdcpInfoPath();
    mHdcpStatusPath = hdcpInfoPath + "/HDCPTX_Status";
    mHdcpCapPath = hdcpInfoPath + "/HDCPTX_Version";
    mHdcpVersionPath = hdcpInfoPath + "/HDCPTX_Curversion";
}

HDCPThread::~HDCPThread() {
    if (mStarted)
        stop();
}

HWC3::Error HDCPThread::start() {
    DEBUG_LOG("%s HDCP Thread for display:%" PRIu64, __FUNCTION__, mDisplayId);

    mThread = std::thread([this]() { threadLoop(); });

    const std::string name = "display_" + std::to_string(mDisplayId) + "_hdcp";

    int ret = pthread_setname_np(mThread.native_handle(), name.c_str());
    if (ret != 0) {
        ALOGE("%s: failed to set HDCP thread name: %s", __FUNCTION__, strerror(ret));
    }

    struct sched_param param = {
            .sched_priority = 2,
    };
    ret = pthread_setschedparam(mThread.native_handle(), SCHED_FIFO, &param);
    if (ret != 0) {
        ALOGE("%s: failed to set HDCP thread priority: %s", __FUNCTION__, strerror(ret));
    }

    mStarted = true;
    return HWC3::Error::None;
}

HWC3::Error HDCPThread::stop() {
    mShuttingDown.store(true);
    if (mThread.joinable()) {
        mThread.join();
    }

    return HWC3::Error::None;
}

HWC3::Error HDCPThread::setCallbacks(const std::function<void()>& callback) {
    DEBUG_LOG("%s HDCP Thread for display:%" PRIu64, __FUNCTION__, mDisplayId);

    std::unique_lock<std::mutex> lock(mStateMutex);
    if (!mCallbacks.has_value()) {
        mCallbacks = callback;
    }

    return HWC3::Error::None;
}

HWC3::Error HDCPThread::setHdcpThreadEnabled(bool enabled) {
    DEBUG_LOG("%s HDCP Thread for display:%" PRIu64 " enabled:%d", __FUNCTION__, mDisplayId,
              enabled);

    std::lock_guard<std::mutex> lock(mStateMutex);
    mThreadEnabled = enabled;
    mHdcpStartTime = std::chrono::system_clock::now();

    return HWC3::Error::None;
}

void HDCPThread::updateHdcpLevels(std::string hdcpCap,
                                  std::string hdcpVer) {
    DEBUG_LOG("%s HDCP Thread for display:%" PRIu64, __FUNCTION__, mDisplayId);

    std::unique_lock<std::mutex> lock(mStateMutex);
    int8_t hdcp_cap = static_cast<int8_t>(HdcpLevel::HDCP_UNKNOWN);
    int8_t hdcp_ver = static_cast<int8_t>(HdcpLevel::HDCP_UNKNOWN);
    if (!hdcpCap.empty())
        hdcp_cap = std::stoi(hdcpCap.c_str());

    if (!hdcpVer.empty())
        hdcp_ver = std::stoi(hdcpVer.c_str());

    switch (hdcp_ver) {
        case KHdcp_Version::HDCP_TX_2:
            mLevels.connectedLevel = HdcpLevel::HDCP_V2_2;
            break;
        case KHdcp_Version::HDCP_TX_1:
            mLevels.connectedLevel = HdcpLevel::HDCP_V1;
            break;
        default:
            mLevels.connectedLevel = HdcpLevel::HDCP_UNKNOWN;
            ALOGE("cur hdcp versioin is not correct");
    }

    switch (hdcp_cap) {
        case (HDCP_CONFIG_1_4 | HDCP_CONFIG_2_2):
            mLevels.maxLevel = HdcpLevel::HDCP_V2_2;
            break;
        case HDCP_CONFIG_2_2:
            mLevels.maxLevel = HdcpLevel::HDCP_V2_2;
            break;
        case HDCP_CONFIG_1_4:
            mLevels.maxLevel = HdcpLevel::HDCP_V1;
            break;
        default:
            mLevels.maxLevel = HdcpLevel::HDCP_UNKNOWN;
            ALOGE("hdcp versioin is not correct");
    }
}

HWC3::Error HDCPThread::getHdcpLevels(HdcpLevels& levels) {
    levels.connectedLevel = mLevels.connectedLevel;
    levels.maxLevel = mLevels.maxLevel;
    return HWC3::Error::None;
}

void HDCPThread::threadLoop() {
    std::string mAuthResult;
    std::smatch mMatch;
    std::string mHdcpCapResult;
    std::string mVersionResult;
    while (!mShuttingDown.load()) {
        if (mThreadEnabled) {
            if (ReadFileToString(mHdcpStatusPath, &mAuthResult)) {
                if (!mAuthResult.empty()) {
                    if (std::regex_search(mAuthResult, mMatch, mPattern)) {
                        if(std::stoi(mMatch[1].str()) == 5) {
                            /* Get the current hdcp levels from connected display */
                            ReadFileToString(mHdcpCapPath, &mHdcpCapResult);
                            ReadFileToString(mHdcpVersionPath, &mVersionResult);
                            updateHdcpLevels(mHdcpCapResult, mVersionResult);
                            if (mCallbacks) {
                                DEBUG_LOG("%s: for display:%" PRIu64 " calling hdcp", __FUNCTION__,
                                          mDisplayId);
                                (*mCallbacks)();
                            }
                        } else {
                            std::chrono::time_point<std::chrono::system_clock> end_time = std::chrono::system_clock::now();
                            auto elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(end_time - mHdcpStartTime).count();
                            // hdcp auth timeout set to 30
                            if (elapsed_seconds >= 30) {
                                mThreadEnabled = false;
                                ALOGW("%s: wait for HDCP authentication timeout for display:%" PRIu64,
                                      __FUNCTION__, mDisplayId);
                            }
                        }
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
} // namespace aidl::android::hardware::graphics::composer3::impl
