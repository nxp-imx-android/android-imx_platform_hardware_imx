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

#ifndef _BYPASS_DEVICE_H
#define _BYPASS_DEVICE_H

#include "ImageProcessDevice.h"

namespace fsl {

// BYPASS device, do nothing, used for debug.
class BypassDevice : public ImageProcessDevice {
public:
    static BypassDevice& getInstance();

    ImxEngine getEngine() const override { return ENG_BYPASS; }
    const char* getName() const override { return "bypass"; }

    static float sniff(uint32_t srcFormat, uint32_t dstFormat, uint32_t srcWidth,
                       uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight);

    int convert(ImxImageBuffer&, ImxImageBuffer&) override {return 0;}

private:
    BypassDevice();
    ~BypassDevice() override = default;
    BypassDevice(const BypassDevice&) = delete;
    BypassDevice& operator=(const BypassDevice&) = delete;
};

} // namespace fsl

#endif // _BYPASS_DEVICE_H
