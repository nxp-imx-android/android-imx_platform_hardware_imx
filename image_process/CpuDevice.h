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

#ifndef _CPU_DEVICE_H
#define _CPU_DEVICE_H

#include "ImageProcessDevice.h"

namespace fsl {

// CPU software fallback image processing device.
// Supports basic copy, resize, and format conversions using libyuv and manual code.
// Always available as the lowest-priority fallback.
class CpuDevice : public ImageProcessDevice {
public:
    static CpuDevice& getInstance();

    ImxEngine getEngine() const override { return ENG_CPU; }
    const char* getName() const override { return "cpu"; }

    static float sniff(uint32_t srcFormat, uint32_t dstFormat, uint32_t srcWidth,
                       uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight);

    int convert(ImxImageBuffer& dst, ImxImageBuffer& src) override;
    int convertMultiSrc(ImxImageBuffer& dst, ImxImageBuffer& srcA, ImxImageBuffer& srcB) override;

private:
    CpuDevice();
    ~CpuDevice() override = default;
    CpuDevice(const CpuDevice&) = delete;
    CpuDevice& operator=(const CpuDevice&) = delete;

    static void MergeImageNV12(void* srcA, void* srcB, void* dst, uint32_t width, uint32_t height);

    static bool IsCscSupportByCPU(int srcFormat, int dstFormat);

    int convertNV12toNV21(ImxImageBuffer& dst, ImxImageBuffer& src);
    void convertYUYVtoNV12SP(uint8_t* inputBuffer, uint8_t* outputBuffer, int width, int height,
                             int srcStride, int dstStride);
    void convertNV12toYV12(uint8_t* inputBuffer, uint8_t* outputBuffer, int width, int height);
    void convertBGGRtoNV12(uint8_t* inputBuffer, uint8_t* outputBuffer, int width, int height);
};

} // namespace fsl

#endif // _CPU_DEVICE_H
