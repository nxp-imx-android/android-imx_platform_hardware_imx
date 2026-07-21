/*
 *  Copyright 2026 NXP.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#define LOG_TAG "CameraWrapper"

#include "CameraWrapper.h"
#include <cutils/log.h>

namespace android {

CameraWrapper::CameraWrapper(std::shared_ptr<libcamera::Camera> &camera) {
    camera_ = camera;
}

CameraWrapper::~CameraWrapper() {
    camera_.reset();
}

static libcamera::PixelFormat HalFormat2PixelFormat(int halFormat) {
    libcamera::PixelFormat pixelFmt = libcamera::formats::YUYV;

    switch (halFormat) {
        case HAL_PIXEL_FORMAT_YCBCR_420_888:
        case HAL_PIXEL_FORMAT_YV12:
            pixelFmt = libcamera::formats::NV12;
            break;
        case HAL_PIXEL_FORMAT_YCBCR_422_I:
        case HAL_PIXEL_FORMAT_BLOB:
            pixelFmt = libcamera::formats::YUYV;
            break;
        case HAL_PIXEL_FORMAT_RGB_888:
            pixelFmt = libcamera::formats::RGB888;
            break;
        default:
            ALOGW("%s: unsupported HalFromat 0x%x", __func__, halFormat);
            break;
    }

    return pixelFmt;
}

void CameraWrapper::SetHalFormat(int halFormat) {
    mHalFormat = (android_pixel_format_t)halFormat;
    mStreamFormat = HalFormat2PixelFormat(mHalFormat);
}

void CameraWrapper::GetConfigSize(uint32_t refWidth, uint32_t refHeight,
                                  uint32_t &configWidth, uint32_t &configHeight) {
    ALOGI("%s: mHalFormat 0x%x, refWidth %u, refHeight %u", __func__, mHalFormat, refWidth, refHeight);

    std::vector<libcamera::Size> sizes = mSupportedFormats.sizes(mStreamFormat);
    configWidth = INT_MAX;
    configHeight = INT_MAX;

    for (auto size : sizes) {
        ALOGI("%s: check size %s", __func__, size.toString().c_str());

        // The best is to find the matched size.
        if ((size.width == refWidth) && (size.height == refHeight)) {
            ALOGI("%s: found matched size", __func__);
            configWidth = refWidth;
            configHeight = refHeight;
            break;
        }

        // Then use the minimal size that greater than ref size.
        // Use "=" due to case as framework requests 1920x1080,
        // but libcamera only supports 1920x1280, as for 0x03c10.
        if ((size.width >= refWidth) && (size.height >= refHeight)) {
            if ((size.width < configWidth) && (size.height < configHeight)) {
                configWidth = size.width;
                configHeight = size.height;
            }
        }
    }

    return;
}

int CameraWrapper::Config(uint32_t bufferNum, uint32_t width, uint32_t height) {
    if (state_ != CameraState::Stopped) {
        ALOGW("%s: state %d is not stopped", __func__, state_);
        return 0;
    }

    int ret = 0;
    std::set<libcamera::Stream *> libCameraStreamSet;
    libcamera::StreamConfiguration cfg;
    std::unique_ptr<libcamera::CameraConfiguration> camCfg;

    uint32_t configWidth = 0;
    uint32_t configHeight = 0;
    GetConfigSize(width, height, configWidth, configHeight);
    if ((configWidth == INT_MAX) || (configHeight == INT_MAX)) {
        ALOGE("%s: no config size found for %ux%u", __func__, width, height);
        return BAD_VALUE;
    }

    ALOGI("%s:, buffers %u, format 0x%x, width %u, height %u, configWidth %u, configHeight %u",
          __func__, bufferNum, mHalFormat, width, height, configWidth, configHeight);

    camCfg = camera_->generateConfiguration();
    if (!camCfg) {
        ALOGE("%s: Failed to generate camera camCfguration", __func__);
        return BAD_VALUE;
    }
    ALOGI("%s: generate camera camCfguration, size %zu", __func__, camCfg->size());

    // config libcamera with 1 stream
    cfg.bufferCount = bufferNum;
    cfg.size.width = configWidth;
    cfg.size.height = configHeight;
    cfg.pixelFormat = mStreamFormat;
    camCfg->addConfiguration(cfg);

    switch (camCfg->validate()) {
        case libcamera::CameraConfiguration::Valid:
            break;
        case libcamera::CameraConfiguration::Adjusted:
            ALOGW("%s: Camera configuration adjusted", __func__);
            for (const libcamera::StreamConfiguration &config : *camCfg)
                ALOGI("%s: - %s", __func__, config.toString().c_str());
            return -EINVAL;
        default:
            ALOGE("%s: Camera configuration invalid", __func__);
            return -EINVAL;
    }

    ret = camera_->configure(camCfg.get());
    if (ret) {
        ALOGE("%s: Failed to configure camera %s", __func__, camera_->id().c_str());
        return -EINVAL;
    }

    // After configure, get the exposure time limits from camera controls
    const libcamera::ControlInfoMap &controls = camera_->controls();
    auto exposureTimeIt = controls.find(libcamera::controls::ExposureTime.id());
    if (exposureTimeIt != controls.end()) {
        const libcamera::ControlInfo &exposureInfo = exposureTimeIt->second;
        mExposureMinUs = exposureInfo.min().get<int64_t>();
        mExposureMaxUs = exposureInfo.max().get<int64_t>();
        mExposureDefUs = exposureInfo.def().get<int64_t>();
        ALOGI("%s: Exposure time range: min %ld us, max %ld us, default %ld us", __func__,
            mExposureMinUs, mExposureMaxUs, mExposureDefUs);
    } else {
        ALOGW("%s: ExposureTime control not found in camera controls", __func__);
    }

    libCameraStreamSet = camera_->streams();
    ALOGI("%s: libCameraStreamSet size %lu, libcameraBuffers %u", __func__,
          libCameraStreamSet.size(), bufferNum);

    for (auto libcameraStream : libCameraStreamSet) {
        const libcamera::StreamConfiguration &config = libcameraStream->configuration();
        ALOGI("%s: check libcamera stream %p, config, format 0x%x, res %dx%d", __func__,
              libcameraStream, (uint32_t)config.pixelFormat, config.size.width, config.size.height);
        if ((config.pixelFormat == mStreamFormat) &&
            (config.size.width == configWidth) && (config.size.height == configHeight)) {
            ALOGI("%s: find the stream", __func__);
            mLibCameraStream = libcameraStream;
            break;
        }
    }

    if (mLibCameraStream == NULL) {
        ALOGE("%s: no libcamera stream found", __func__);
        return -EINVAL;
    }

    mStreamWidth = configWidth;
    mStreamHeight = configHeight;

    state_ = CameraState::Configured;
    return 0;
}

int CameraWrapper::Start() {
    if (state_ != CameraState::Configured) {
        ALOGE("%s: state %d is not configured", __func__, state_);
        return -EINVAL;
    }

    ALOGI("%s: call camera_->start", __func__);
    int ret = camera_->start();
    if (ret) {
        ALOGE("%s: Failed to start camera", __func__);
        return -EINVAL;
    }
    ALOGI("%s: camera_->start ok", __func__);

    state_ = CameraState::Running;
    return 0;
}

void CameraWrapper::Stop() {
    if (state_ != CameraState::Running) {
        ALOGW("%s: state %d is not running", __func__, state_);
        return;
    }

    ALOGI("%s: call camera_->stop", __func__);
    int ret = camera_->stop();
    if (ret) {
        ALOGW("%s: Failed to stop camera", __func__);
    }
    state_ = CameraState::Stopped;
}

} // namespace android
