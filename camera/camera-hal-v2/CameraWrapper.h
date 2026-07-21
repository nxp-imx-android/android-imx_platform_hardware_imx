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

#ifndef LIBCAMERA_WRAPPER_H
#define LIBCAMERA_WRAPPER_H

#include <utils/Errors.h>
#include <system/graphics.h>
#include "libcamera/camera.h"
#include "libcamera/camera_manager.h"
#include "libcamera/formats.h"
#include "libcamera/framebuffer.h"
#include "libcamera/pixel_format.h"
#include "libcamera/request.h"
#include "libcamera/control_ids.h"

namespace android {

class CameraWrapper {

public:
    CameraWrapper(std::shared_ptr<libcamera::Camera> &camera);
    ~CameraWrapper();

    enum CameraState {
        Stopped,
        Configured,
        Running,
    };
    CameraState state_ = Stopped;

    std::shared_ptr<libcamera::Camera> camera_;
    libcamera::StreamFormats mSupportedFormats;
    libcamera::Stream *mLibCameraStream = NULL;
    android_pixel_format_t mHalFormat = HAL_PIXEL_FORMAT_YCBCR_422_I;
    libcamera::PixelFormat mStreamFormat = libcamera::formats::YUYV;
    uint32_t mStreamWidth = 0;
    uint32_t mStreamHeight = 0;
    uint64_t mExposureMinUs = 0;
    uint64_t mExposureMaxUs = 0;
    uint64_t mExposureDefUs = 0;

public:
    int Config(uint32_t bufferNum, uint32_t width, uint32_t height);
    int Start();
    void Stop();
    bool IsRunning() { return (state_ == Running); };
    void SetHalFormat(int halFormat);

private:
    void GetConfigSize(uint32_t refWidth, uint32_t refHeight, uint32_t &configWidth,
                       uint32_t &configHeight);
};

} // namespace android

#endif // LIBCAMERA_WRAPPER_H
