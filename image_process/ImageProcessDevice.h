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

#ifndef _IMAGE_PROCESS_DEVICE_H
#define _IMAGE_PROCESS_DEVICE_H

#include <cutils/native_handle.h>
#include <limits.h>
#include <unistd.h>

#include "ImageUtils.h"

#if defined(__LP64__)
#define IMG_LIB_PATH1 "/system/lib64"
#define IMG_LIB_PATH2 "/vendor/lib64"
#else
#define IMG_LIB_PATH1 "/system/lib"
#define IMG_LIB_PATH2 "/vendor/lib"
#endif

namespace fsl {

using namespace android;

// Confidence scores for device selection.
// Higher confidence means the device is more suitable for the given conversion.
// A score of 0.0 means the device cannot handle the conversion.
static constexpr float CONFIDENCE_NONE = 0.0f;
static constexpr float CONFIDENCE_BYPASS = 0.2f;
static constexpr float CONFIDENCE_CPU = 0.4f;
static constexpr float CONFIDENCE_OCLCVT = 0.7f;
static constexpr float CONFIDENCE_DPU = 0.8f;
static constexpr float CONFIDENCE_G2D = 0.9f;
static constexpr float CONFIDENCE_G2D_RGBA = 1.2f;
static constexpr float CONFIDENCE_OCLCVT_PREFERRED = 1.4f;

// Holds the format and resolution portion of an ImxImageBuffer.
struct BufferFormat {
    uint32_t mFormat = 0;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
};

// Abstract base class for image processing devices.
// Each concrete device wraps a specific HW/SW engine.
// Modeled after codec2 FilterDevice pattern.
class ImageProcessDevice {
public:
    virtual ~ImageProcessDevice() = default;

    // Get the engine type this device represents.
    virtual ImxEngine getEngine() const = 0;

    // Get a human-readable name for this device.
    virtual const char* getName() const = 0;

    // Perform the image conversion.
    // @return 0 on success, negative error code on failure.
    virtual int convert(ImxImageBuffer& dst, ImxImageBuffer& src) = 0;

    // Perform a two-source image merge into dst.
    // Default implementation returns -ENOTSUP.
    // @return 0 on success, -ENOTSUP if not supported, negative error code on failure.
    virtual int convertMultiSrc(ImxImageBuffer& dst, ImxImageBuffer& srcA, ImxImageBuffer& srcB) {
        (void)dst;
        (void)srcA;
        (void)srcB;
        return -ENOTSUP;
    }

    // Save the format and resolution from the source buffer.
    // @return 0 on success, negative error code on failure.
    virtual int setSrcBufferFormat(ImxImageBuffer* buf) {
        if (buf) {
            mSrcBufferFormat = {buf->mFormat, buf->mWidth, buf->mHeight};
        }
        return 0;
    }

    // Save the format and resolution from the destination buffer.
    // @return 0 on success, negative error code on failure.
    virtual int setDstBufferFormat(ImxImageBuffer* buf) {
        if (buf) {
            mDstBufferFormat = {buf->mFormat, buf->mWidth, buf->mHeight};
        }
        return 0;
    }

    void setDebug(bool debug) { mDebug = debug; }

protected:
    bool mDebug = false;
    BufferFormat mSrcBufferFormat;
    BufferFormat mDstBufferFormat;

    // Utility: find library path in system or vendor lib dirs.
    static void getModulePath(char* path, const char* name) {
        snprintf(path, PATH_MAX, "%s/%s", IMG_LIB_PATH1, name);
        if (access(path, R_OK) == 0)
            return;
        snprintf(path, PATH_MAX, "%s/%s", IMG_LIB_PATH2, name);
    }
};

} // namespace fsl

#endif // _IMAGE_PROCESS_DEVICE_H
