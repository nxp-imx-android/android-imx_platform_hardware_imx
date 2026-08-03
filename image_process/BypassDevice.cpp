/*
 * Copyright 2026 NXP.
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
#define LOG_TAG "BypassDevice"

#include "BypassDevice.h"

#include <cutils/log.h>

namespace fsl {

BypassDevice& BypassDevice::getInstance() {
    static BypassDevice sInstance;
    return sInstance;
}

BypassDevice::BypassDevice() {
    ALOGI("BypassDevice: initialized");
}

float BypassDevice::sniff(uint32_t srcFormat, uint32_t dstFormat, uint32_t srcWidth,
                       uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight) {
    (void)srcFormat;
    (void)srcWidth;
    (void)srcHeight;
    (void)dstFormat;
    (void)dstWidth;
    (void)dstHeight;

    return CONFIDENCE_BYPASS;
}

} // namespace fsl
