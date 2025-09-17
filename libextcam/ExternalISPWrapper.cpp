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
#define LOG_TAG "ExternalISPWrapper"

#include "ExternalISPWrapper.h"

#include <errno.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <log/log.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utils/Errors.h>

namespace android {

ExternalISPWrapper::ExternalISPWrapper(int32_t fd) : m_fd(fd) {
    // Set ISP feature to it's default value.
    m_lastAwbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
    m_lastAeMode = ANDROID_CONTROL_AE_MODE_ON;
    m_lastAfMode = ANDROID_CONTROL_AF_MODE_AUTO;

    m_lastExposureTime = 0;
    m_lastExposureGain = 0;
    m_lastFocusDistance = 0.0f;

    m_lastBrightness = 0;
    m_lastContrast = 0.0f;
    m_lastSaturation = 0.0f;
    m_lastSharpLevel = 0;
}

ExternalISPWrapper::~ExternalISPWrapper() {}

static int32_t queryV4L2Control(int32_t fd, uint32_t controlId, v4l2_queryctrl& queryctrl) {
    memset(&queryctrl, 0, sizeof(queryctrl));
    queryctrl.id = controlId;

    if (ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl) != 0) {
        ALOGI("%s, VIDIOC_QUERYCTRL for control 0x%08x failed: %s", __func__, controlId,
              strerror(errno));
        return -1;
    }

    ALOGI("%s, VIDIOC_QUERYCTRL for control 0x%08x success, type:%u, name:%s,  minimum: %d, maximum: %d, step: %d,  default_value: %d",
          __func__, queryctrl.id, queryctrl.type, queryctrl.name, queryctrl.minimum,
          queryctrl.maximum, queryctrl.step, queryctrl.default_value);

    return 0;
}

static int32_t setV4L2ControlValue(int32_t fd, uint32_t controlId, int32_t value) {
    v4l2_control ctrl;
    ctrl.id = controlId;
    ctrl.value = value;

    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) != 0) {
        ALOGI("%s, VIDIOC_S_CTRL for control 0x%08x failed: %s", __func__, controlId,
              strerror(errno));
        return -1;
    }
    ALOGV("%s, VIDIOC_S_CTRL for control 0x%08x success", __func__, controlId);

    return 0;
}

static int32_t getV4L2ControlValue(int32_t fd, uint32_t controlId, int32_t& value) {
    v4l2_control ctrl;
    ctrl.id = controlId;

    if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) != 0) {
        value = ctrl.value;
        ALOGI("%s, VIDIOC_G_CTRL for control 0x%08x failed: %s", __func__, controlId,
              strerror(errno));
        return -1;
    }
    ALOGV("%s, VIDIOC_G_CTRL for control 0x%08x success, value: %d", __func__, controlId,
          ctrl.value);

    return 0;
}

static int mapFloatToIntWithStep(float value, float in_min, float in_max, int out_min, int out_max,
                                 int step) {
    // Limit the input range
    if (value < in_min)
        value = in_min;
    if (value > in_max)
        value = in_max;

    // Linear mapping to output interval
    float ratio = (value - in_min) / (in_max - in_min);
    int mappedValue = static_cast<int>(ratio * (out_max - out_min) + out_min + 0.5f);

    // step alignment
    if (step > 1) {
        int remainder = (mappedValue - out_min) % step;
        mappedValue -= remainder;
    }
    // Guaranteed boundaries
    if (mappedValue < out_min)
        mappedValue = out_min;
    if (mappedValue > out_max)
        mappedValue = out_max;

    return mappedValue;
}

int32_t ExternalISPWrapper::enableAWB(bool enable) {
    (void)setV4L2ControlValue(m_fd, V4L2_CID_AUTO_WHITE_BALANCE, enable);

    return 0;
}

int32_t ExternalISPWrapper::processAWB(uint8_t mode, bool force) {
    int32_t ret = 0;

    ALOGV("%s, mode %d, m_lastAwbMode %d", __func__, mode, m_lastAwbMode);
    if (mode == m_lastAwbMode && force == false)
        return 0;
    ALOGI("%s, change WB mode from %d to %d, force %d", __func__, m_lastAwbMode, mode, force);

    if ((mode == ANDROID_CONTROL_AWB_MODE_AUTO) || (mode == ANDROID_CONTROL_AWB_MODE_OFF)) {
        bool bEnable = (mode == ANDROID_CONTROL_AWB_MODE_AUTO) ? true : false;
        ret = enableAWB(bEnable);
        if (ret == 0)
            m_lastAwbMode = mode;

        return ret;
    }

    // If shift from AWB to MWB, first disable AWB.
    if (m_lastAwbMode == ANDROID_CONTROL_AWB_MODE_AUTO) {
        ret = enableAWB(false);
        if (ret) {
            return ret;
        }
    }

    int32_t temperature = 0;
    switch (mode) {
        case ANDROID_CONTROL_AWB_MODE_INCANDESCENT:
            temperature = 2800;
            break;
        case ANDROID_CONTROL_AWB_MODE_FLUORESCENT:
            temperature = 4500;
            break;
        case ANDROID_CONTROL_AWB_MODE_WARM_FLUORESCENT:
            temperature = 3000;
            break;
        case ANDROID_CONTROL_AWB_MODE_DAYLIGHT:
            temperature = 6000;
            break;
        case ANDROID_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT:
            temperature = 6800;
            break;
        case ANDROID_CONTROL_AWB_MODE_TWILIGHT:
            temperature = 7000;
            break;
        case ANDROID_CONTROL_AWB_MODE_SHADE:
            temperature = 7200;
            break;
        default:
            ALOGW("%s, Unsupported AWB mode: %d", __func__, mode);
            return -1;
    }

    v4l2_queryctrl queryctrl;
    ret = queryV4L2Control(m_fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE, queryctrl);
    if (ret != 0) {
        ALOGE("%s, control 0x%08x not support!", __func__, V4L2_CID_WHITE_BALANCE_TEMPERATURE);
        m_lastAwbMode = mode;
        return -1;
    }

    if (temperature > queryctrl.maximum)
        temperature = queryctrl.maximum;
    if (temperature < queryctrl.minimum)
        temperature = queryctrl.minimum;

    ALOGI("%s, Setting white balance temperature to %dK for AWB mode %d", __func__, temperature,
          mode);
    ret = setV4L2ControlValue(m_fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE, temperature);
    if (ret != 0) {
        ALOGE("%s, Failed to set white balance temperature for AWB mode %d", __func__, mode);
        return -1;
    }
    m_lastAwbMode = mode;

    return ret;
}

int32_t ExternalISPWrapper::processAeMode(uint8_t mode, bool force) {
    int32_t ret = 0;

    ALOGV("%s, mode %d, m_lastAeMode %d", __func__, mode, m_lastAeMode);
    if (mode == m_lastAeMode && force == false)
        return 0;
    ALOGI("%s: set ae mode to %d, force %d", __func__, mode, force);

    int32_t autoExposureMode = V4L2_EXPOSURE_MANUAL;
    switch (mode) {
        case ANDROID_CONTROL_AE_MODE_OFF:
            autoExposureMode = V4L2_EXPOSURE_MANUAL;
            ALOGI("%s, Auto exposure mode set to manual", __func__);
            break;
        case ANDROID_CONTROL_AE_MODE_ON:
            autoExposureMode = V4L2_EXPOSURE_APERTURE_PRIORITY;
            ALOGI("%s, Manual exposure mode set to Aperture priority exposure", __func__);
            break;
        default:
            ALOGW("%s, Unsupported AE mode: %d", __func__, mode);
            return -1;
    }

    ret = setV4L2ControlValue(m_fd, V4L2_CID_EXPOSURE_AUTO, autoExposureMode);
    if (ret != 0) {
        ALOGE("%s, Failed to set exposure mode %d", __func__, mode);
        m_lastAeMode = mode;
        return -1;
    }
    m_lastAeMode = mode;

    return 0;
}

int32_t ExternalISPWrapper::processExposureTime(int64_t exposureTime) {
    int32_t ret = 0;

    if (exposureTime == m_lastExposureTime)
        return 0;

    v4l2_queryctrl queryctrl;
    ret = queryV4L2Control(m_fd, V4L2_CID_EXPOSURE_ABSOLUTE, queryctrl);
    if (ret != 0) {
        ALOGE("%s, control 0x%08x not support!", __func__, V4L2_CID_EXPOSURE_ABSOLUTE);
        m_lastExposureTime = exposureTime;
        return -1;
    }

    if (exposureTime > queryctrl.maximum)
        exposureTime = queryctrl.maximum;
    if (exposureTime < queryctrl.minimum)
        exposureTime = queryctrl.minimum;

    // first disable AEC
    processAeMode(ANDROID_CONTROL_AE_MODE_OFF);

    ALOGI("%s: Setting exposureTime from %ld to %ld", __func__, m_lastExposureTime, exposureTime);
    ret = setV4L2ControlValue(m_fd, V4L2_CID_EXPOSURE_ABSOLUTE, exposureTime);
    if (ret != 0) {
        ALOGE("%s, Failed to set exposureTime %ld", __func__, exposureTime);
        return -1;
    }

    m_lastExposureTime = exposureTime;

    return 0;
}

int32_t ExternalISPWrapper::processExposureGain(int32_t exposureGain) {
    int32_t ret = 0;

    if (exposureGain == m_lastExposureGain)
        return 0;

    v4l2_queryctrl queryctrl;
    ret = queryV4L2Control(m_fd, V4L2_CID_GAIN, queryctrl);
    if (ret != 0) {
        ALOGE("%s, control 0x%08x not support!", __func__, V4L2_CID_GAIN);
        m_lastExposureGain = exposureGain;
        return -1;
    }

    // first disable AEC
    processAeMode(ANDROID_CONTROL_AE_MODE_OFF);

    // [1 10] maps to [0 255], step 1.
    int32_t gainAbsolute =
            mapFloatToIntWithStep(static_cast<float>(exposureGain), 1.0f, 10.0f, queryctrl.minimum,
                                  queryctrl.maximum, queryctrl.step);

    ret = setV4L2ControlValue(m_fd, V4L2_CID_GAIN, gainAbsolute);
    if (ret != 0) {
        ALOGE("%s, Failed to set gainAbsolute %d", __func__, gainAbsolute);
        return -1;
    }
    ALOGI("%s: Setting exposureGain from %d to %d, gainAbsolute: %d", __func__, m_lastExposureGain,
          exposureGain, gainAbsolute);

    m_lastExposureGain = exposureGain;

    return 0;
}

int32_t ExternalISPWrapper::processAfMode(uint8_t mode, bool force) {
    int32_t ret = 0;

    ALOGV("%s, mode %d, m_lastAfMode %d", __func__, mode, m_lastAfMode);
    if (mode == m_lastAfMode && force == false)
        return 0;
    ALOGI("%s: set af mode to %d, force %d", __func__, mode, force);

    bool autoFocusMode = false;
    switch (mode) {
        case ANDROID_CONTROL_AF_MODE_OFF:
            autoFocusMode = false;
            ALOGI("%s, Auto focus mode set to manual", __func__);
            break;
        case ANDROID_CONTROL_AF_MODE_AUTO:
        case ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
        case ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE:
            autoFocusMode = true;
            ALOGI("%s, Manual focus mode set to auto %d", __func__, mode);
            break;
        default:
            ALOGW("%s, Unsupported focus mode: %d", __func__, mode);
            return -1;
    }

    ret = setV4L2ControlValue(m_fd, V4L2_CID_FOCUS_AUTO, autoFocusMode);
    if (ret != 0) {
        ALOGE("%s, Failed to set focue mode %d", __func__, mode);
        m_lastAfMode = mode;
        return -1;
    }
    m_lastAfMode = mode;

    return 0;
}

int32_t ExternalISPWrapper::processFocusDistance(float focusDistance) {
    int32_t ret = 0;

    if (focusDistance == m_lastFocusDistance)
        return 0;

    v4l2_queryctrl queryctrl;
    ret = queryV4L2Control(m_fd, V4L2_CID_FOCUS_ABSOLUTE, queryctrl);
    if (ret != 0) {
        ALOGE("%s, control 0x%08x not support!", __func__, V4L2_CID_FOCUS_ABSOLUTE);
        m_lastFocusDistance = focusDistance;
        return -1;
    }

    // (0.0f ~ 10.0f) maps to [0 255], step 5.
    int32_t focusAbsolute = mapFloatToIntWithStep(focusDistance, 0.0f, 10.0f, queryctrl.minimum,
                                                  queryctrl.maximum, queryctrl.step);

    ret = setV4L2ControlValue(m_fd, V4L2_CID_FOCUS_ABSOLUTE, focusAbsolute);
    if (ret != 0) {
        ALOGE("%s, Failed to set focusAbsolute %d", __func__, focusAbsolute);
        return -1;
    }
    ALOGI("%s: Setting focusDistance from %f to %f, focusAbsolute: %d", __func__,
          m_lastFocusDistance, focusDistance, focusAbsolute);
    m_lastFocusDistance = focusDistance;

    return 0;
}

int32_t ExternalISPWrapper::processBrightness(int32_t brightness, bool force) {
    int32_t ret = 0;

    if (brightness == m_lastBrightness && force == false)
        return 0;

    v4l2_queryctrl queryctrl;
    ret = queryV4L2Control(m_fd, V4L2_CID_BRIGHTNESS, queryctrl);
    if (ret != 0) {
        ALOGE("%s, control 0x%08x not support!", __func__, V4L2_CID_BRIGHTNESS);
        m_lastBrightness = brightness;
        return -1;
    }

    // [-127 127] maps to [0 255], step 1.
    int32_t brightnessAbsolute =
            mapFloatToIntWithStep(static_cast<float>(brightness), -127.0f, 127.0f,
                                  queryctrl.minimum, queryctrl.maximum, queryctrl.step);
    if (force) {
        brightnessAbsolute = queryctrl.default_value;
    }

    ret = setV4L2ControlValue(m_fd, V4L2_CID_BRIGHTNESS, brightnessAbsolute);
    if (ret != 0) {
        ALOGE("%s, Failed to set brightnessAbsolute %d", __func__, brightnessAbsolute);
        return -1;
    }
    ALOGI("%s: Setting brightness from %d to %d, brightnessAbsolute: %d", __func__,
          m_lastBrightness, brightness, brightnessAbsolute);
    m_lastBrightness = brightness;

    return 0;
}

int32_t ExternalISPWrapper::processContrast(float contrast, bool force) {
    int32_t ret = 0;

    if (contrast == m_lastContrast && force == false)
        return 0;

    v4l2_queryctrl queryctrl;
    ret = queryV4L2Control(m_fd, V4L2_CID_CONTRAST, queryctrl);
    if (ret != 0) {
        ALOGE("%s, control 0x%08x not support!", __func__, V4L2_CID_CONTRAST);
        m_lastContrast = contrast;
        return -1;
    }

    // [0.0 1.99] maps to [0 255], step 1.
    int32_t contrastAbsolute = mapFloatToIntWithStep(contrast, 0.0f, 1.99f, queryctrl.minimum,
                                                     queryctrl.maximum, queryctrl.step);
    if (force) {
        contrastAbsolute = queryctrl.default_value;
    }

    ret = setV4L2ControlValue(m_fd, V4L2_CID_CONTRAST, contrastAbsolute);
    if (ret != 0) {
        ALOGE("%s, Failed to set contrastAbsolute %d", __func__, contrastAbsolute);
        return -1;
    }
    ALOGI("%s: Setting contrast from %f to %f, contrastAbsolute: %d", __func__, m_lastContrast,
          contrast, contrastAbsolute);
    m_lastContrast = contrast;

    return 0;
}

int32_t ExternalISPWrapper::processSaturation(float saturation, bool force) {
    int32_t ret = 0;

    if (saturation == m_lastSaturation && force == false)
        return 0;

    v4l2_queryctrl queryctrl;
    ret = queryV4L2Control(m_fd, V4L2_CID_SATURATION, queryctrl);
    if (ret != 0) {
        ALOGE("%s, control 0x%08x not support!", __func__, V4L2_CID_SATURATION);
        m_lastSaturation = saturation;
        return -1;
    }

    // [0.0 1.99] maps to [0 255], step 1.
    int32_t saturationAbsolute = mapFloatToIntWithStep(saturation, 0.0f, 1.99f, queryctrl.minimum,
                                                       queryctrl.maximum, queryctrl.step);
    if (force) {
        saturationAbsolute = queryctrl.default_value;
    }

    ret = setV4L2ControlValue(m_fd, V4L2_CID_SATURATION, saturationAbsolute);
    if (ret != 0) {
        ALOGE("%s, Failed to set saturationAbsolute %d", __func__, saturationAbsolute);
        return -1;
    }
    ALOGI("%s: Setting saturation from %f to %f, saturationAbsolute: %d", __func__,
          m_lastSaturation, saturation, saturationAbsolute);
    m_lastSaturation = saturation;

    return 0;
}

int32_t ExternalISPWrapper::processSharpLevel(uint8_t sharpLevel, bool force) {
    int32_t ret = 0;

    if (sharpLevel == m_lastSharpLevel && force == false)
        return 0;

    v4l2_queryctrl queryctrl;
    ret = queryV4L2Control(m_fd, V4L2_CID_SHARPNESS, queryctrl);
    if (ret != 0) {
        ALOGE("%s, control 0x%08x not support!", __func__, V4L2_CID_SHARPNESS);
        m_lastSharpLevel = sharpLevel;
        return -1;
    }

    // [1 10] maps to [0 255], step 1.
    int32_t sharpLevelAbsolute =
            mapFloatToIntWithStep(static_cast<float>(sharpLevel), 1.0f, 10.0f, queryctrl.minimum,
                                  queryctrl.maximum, queryctrl.step);
    if (force) {
        sharpLevelAbsolute = queryctrl.default_value;
    }

    ret = setV4L2ControlValue(m_fd, V4L2_CID_SHARPNESS, sharpLevelAbsolute);
    if (ret != 0) {
        ALOGE("%s, Failed to set sharpLevelAbsolute %d", __func__, sharpLevelAbsolute);
        return -1;
    }
    ALOGI("%s: Setting sharpLevel from %u to %u, sharpLevelAbsolute: %d", __func__,
          m_lastSharpLevel, sharpLevel, sharpLevelAbsolute);
    m_lastSharpLevel = sharpLevel;

    return 0;
}

using namespace android::hardware::camera::device::implementation;

// Current tactic: don't return if some meta process failed,
// since may have other meta to process.
int32_t ExternalISPWrapper::process(CameraMetadata& meta, const char* deviceCardName) {
    if (meta.isEmpty()) {
        return BAD_VALUE;
    }
    camera_metadata_entry entry;

    // AWB
    entry = meta.find(ANDROID_CONTROL_AWB_MODE);
    if (entry.count > 0) {
        (void)processAWB(entry.data.u8[0]);
    }

    // AE
    entry = meta.find(ANDROID_CONTROL_AE_MODE);
    if (entry.count > 0) {
        (void)processAeMode(entry.data.u8[0]);
    }

    // ExposureTime
    entry = meta.find(ANDROID_SENSOR_EXPOSURE_TIME);
    if (entry.count > 0) {
        // skip for c270 uvc type to avoid block issue
        if (!strstr(deviceCardName, "UVC Camera (046d:0825)")) {
            (void)processExposureTime(entry.data.i64[0]);
        }
    }

    // ExposureGain
    entry = meta.find(ANDROID_SENSOR_SENSITIVITY);
    if (entry.count > 0) {
        (void)processExposureGain(entry.data.i32[0]);
    }

    // AF
    entry = meta.find(ANDROID_CONTROL_AF_MODE);
    if (entry.count > 0) {
        // Skip c270 as focus adjustment is not supported
        if (!(strstr(deviceCardName, "UVC Camera (046d:0825)") ||
              strstr(deviceCardName, "C270 HD WEBCAM"))) {
            (void)processAfMode(entry.data.u8[0]);
        }
    }

    // Focus Distance
    entry = meta.find(ANDROID_LENS_FOCUS_DISTANCE);
    if (entry.count > 0) {
        // Skip c270 as focus adjustment is not supported
        if (!(strstr(deviceCardName, "UVC Camera (046d:0825)") ||
              strstr(deviceCardName, "C270 HD WEBCAM"))) {
            (void)processFocusDistance(entry.data.f[0]);
        }
    }

    // brightness
    entry = meta.find(EXT_BRIGHTNESS);
    if (entry.count > 0) {
        (void)processBrightness(entry.data.i32[0]);
    }

    // contrast
    entry = meta.find(EXT_CONTRAST);
    if (entry.count > 0) {
        (void)processContrast(entry.data.f[0]);
    }

    // saturation
    entry = meta.find(EXT_SATURATION);
    if (entry.count > 0) {
        (void)processSaturation(entry.data.f[0]);
    }

    // sharpLevel
    entry = meta.find(EXT_SHARP_LEVEL);
    if (entry.count > 0) {
        (void)processSharpLevel(entry.data.u8[0]);
    }

    return 0;
}

} // namespace android
