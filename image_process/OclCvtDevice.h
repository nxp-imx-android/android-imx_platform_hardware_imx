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

#ifndef _OCLCVT_DEVICE_H
#define _OCLCVT_DEVICE_H

#include <utils/Mutex.h>

#include "ImageProcessDevice.h"
#include "imx_opencl_converter.h"

namespace fsl {

using namespace android;

typedef OCL_RESULT (*ocl_open)(OCL_OPEN_FLAG flag, OCL_HANDLE* handle);
typedef OCL_RESULT (*ocl_setParam)(OCL_HANDLE handle, OCL_PARAM_INDEX index, void* param);
typedef OCL_RESULT (*ocl_getParam)(OCL_HANDLE handle, OCL_PARAM_INDEX index, void* param);
typedef OCL_RESULT (*ocl_convert)(OCL_HANDLE handle, OCL_BUFFER* in_buf, OCL_BUFFER* out_buf);
typedef OCL_RESULT (*ocl_close)(OCL_HANDLE handle);

// OpenCL Converter image processing device.
// Uses lib_imx_opencl_converter.so for GPU-based CSC, scale, and dewarp.
class OclCvtDevice : public ImageProcessDevice {
public:
    OclCvtDevice();
    ~OclCvtDevice() override;

    ImxEngine getEngine() const override { return ENG_OCLCVT; }
    const char* getName() const override { return "oclcvt"; }

    static float sniff(uint32_t srcFormat, uint32_t dstFormat, uint32_t srcWidth,
                       uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight);

    int convert(ImxImageBuffer& dst, ImxImageBuffer& src) override;

    int setSrcBufferFormat(ImxImageBuffer* buf) override;
    int setDstBufferFormat(ImxImageBuffer* buf) override;

private:
    bool init();
    void ImxImageBufferToOclBuffer(ImxImageBuffer& imxImgBuf, OCL_BUFFER& oclBuf,
                                   OCL_FORMAT& oclFmt);
    int parse_warp_file(const char* file_name, OCL_WARP_PARAM* warp_param);
    static int HalPixelFormatToOclPixelFormat(uint32_t& halPixelFormat,
                                               OCL_PIXEL_FORMAT& oclPixelFormat);
    static void ImxImageBufferToOclFormat(ImxImageBuffer& srcImgBuf, OCL_FORMAT& input_format,
                                          ImxImageBuffer& dstImgBuf, OCL_FORMAT& output_format);

    bool mInitialized;
    char mSocType[128];
    bool mLayoutMBCam;
    OCL_MEMORY_TYPE mOclBufferType;

    void* mImxOclCvtModule;
    OCL_HANDLE mHOcl;
    ocl_open m_ocl_open;
    ocl_setParam m_ocl_setParam;
    ocl_getParam m_ocl_getParam;
    ocl_convert m_ocl_convert;
    ocl_close m_ocl_close;
    Mutex mOclCvtLock;

    // Cached input/output formats — only re-set when changed
    OCL_FORMAT mInputFormat;
    OCL_FORMAT mOutputFormat;

    // g3d dewarp
    OCL_WARP_PARAM m_warp_param;
    ImxImageBuffer mWarpBuffer;
};

} // namespace fsl

#endif // _OCLCVT_DEVICE_H
