/*
 *  Copyright 2025 NXP
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

#ifndef _EXTERNAL_ISP_WRAPPER_H
#define _EXTERNAL_ISP_WRAPPER_H

#include "ExternalCameraUtils.h"

namespace android {

class ExternalISPWrapper {
public:
    ExternalISPWrapper(int32_t fd);
    ~ExternalISPWrapper();
    int32_t process(CameraMetadata& pMeta, const char* deviceCardName);
    int32_t processAWB(uint8_t mode, bool force = false);
    int32_t processAeMode(uint8_t mode, bool force = false);
    int32_t processAfMode(uint8_t mode, bool force = false);
    int32_t processBrightness(int32_t brightness, bool force = false);
    int32_t processContrast(float contrast, bool force = false);
    int32_t processSaturation(float contrast, bool force = false);
    int32_t processSharpLevel(uint8_t sharpLevel, bool force = false);

private:
    int32_t enableAWB(bool enable);
    int32_t processExposureTime(int64_t exposureTime);
    int32_t processExposureGain(int32_t exposureGain);
    int32_t processFocusDistance(float focusDistance);

private:
    int32_t m_fd = -1;
    uint8_t m_lastAwbMode;
    uint8_t m_lastAeMode;
    int64_t m_lastExposureTime;
    int32_t m_lastExposureGain;
    uint8_t m_lastAfMode;
    float m_lastFocusDistance;
    int32_t m_lastBrightness;
    float m_lastContrast;
    float m_lastSaturation;
    uint8_t m_lastSharpLevel;
};

} // namespace android

#endif // _EXTERNAL_ISP_WRAPPER_H
