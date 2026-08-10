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
#define LOG_TAG "OclCvtDevice"

#include "OclCvtDevice.h"

#include <cutils/log.h>
#include <cutils/properties.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <system/graphics.h>

#define IMX_OCL_CONVERTER "lib_imx_opencl_converter.so"
#define DEWARP_COORD_FILE "/vendor/etc/configs/ox03c_absolute_32bpp_dewarp_file-1920x1280.bin"

namespace fsl {

OclCvtDevice::OclCvtDevice()
      : mInitialized(false),
        mLayoutMBCam(false),
        mOclBufferType(OCL_MEM_TYPE_GPU),
        mImxOclCvtModule(NULL),
        mHOcl(NULL),
        m_ocl_open(NULL),
        m_ocl_setParam(NULL),
        m_ocl_getParam(NULL),
        m_ocl_convert(NULL),
        m_ocl_close(NULL) {
    memset(mSocType, 0, sizeof(mSocType));
    memset(&mInputFormat, 0, sizeof(mInputFormat));
    memset(&mOutputFormat, 0, sizeof(mOutputFormat));
    memset(&mWarpBuffer, 0, sizeof(mWarpBuffer));
    memset(&m_warp_param, 0, sizeof(m_warp_param));
    property_get("ro.boot.soc_type", mSocType, "");
    mInitialized = init();
}

OclCvtDevice::~OclCvtDevice() {
    if (mHOcl) {
        m_ocl_close(mHOcl);
        mHOcl = NULL;
    }

    if (mImxOclCvtModule)
        dlclose(mImxOclCvtModule);

    if (m_warp_param.buf.planes[0].vaddr) {
        if (mWarpBuffer.buffer)
            FreePhyBuffer(mWarpBuffer.buffer);
        else
            free((void*)m_warp_param.buf.planes[0].vaddr);
    }
}

bool OclCvtDevice::init() {
    char path[PATH_MAX] = {0};
    getModulePath(path, IMX_OCL_CONVERTER);
    mImxOclCvtModule = dlopen(path, RTLD_NOW);
    if (mImxOclCvtModule == NULL) {
        ALOGW("OclCvtDevice: dlopen %s failed: %s", path, dlerror());
        return false;
    }

    m_ocl_open = (ocl_open)dlsym(mImxOclCvtModule, "OCL_Open");
    m_ocl_setParam = (ocl_setParam)dlsym(mImxOclCvtModule, "OCL_SetParam");
    m_ocl_getParam = (ocl_getParam)dlsym(mImxOclCvtModule, "OCL_GetParam");
    m_ocl_convert = (ocl_convert)dlsym(mImxOclCvtModule, "OCL_Convert");
    m_ocl_close = (ocl_close)dlsym(mImxOclCvtModule, "OCL_Close");

    char value[PROPERTY_VALUE_MAX];
    property_get("ro.boot.camera.layout", value, "");
    mLayoutMBCam = (strcmp(value, "mbcam") == 0);

    if (!strncmp(mSocType, "imx9", 4))
        mOclBufferType = OCL_MEM_TYPE_DEVICE;

    int ret = (*m_ocl_open)(OCL_OPEN_FLAG_PROFILE, &mHOcl);
    if (ret != 0) {
        mHOcl = NULL;
        ALOGW("OclCvtDevice: m_ocl_open failed, ret %d", ret);
    }
    ALOGI("OclCvtDevice: mHOcl %p", mHOcl);

    if (mImxOclCvtModule && mLayoutMBCam) {
        ret = parse_warp_file(DEWARP_COORD_FILE, &m_warp_param);
        if (ret) {
            ALOGW("OclCvtDevice: parse_warp_file failed, ret %d", ret);
        } else {
            ALOGI("OclCvtDevice: parse_warp_file ok");
            if (mHOcl && (m_warp_param.enable == 1))
                m_ocl_setParam(mHOcl, OCL_PARAM_INDEX_WARP_PARAM, &m_warp_param);
        }
    }

    ALOGI("OclCvtDevice: initialized, mLayoutMBCam=%d", mLayoutMBCam);
    return true;
}

float OclCvtDevice::sniff(uint32_t srcFormat, uint32_t dstFormat, uint32_t srcWidth,
                          uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight) {

    // Cache the soc_type property so property_get is called only once.
    static char sSocType[PROPERTY_VALUE_MAX] = {};
    static bool sSocTypeFetched = false;
    if (!sSocTypeFetched) {
        property_get("ro.boot.soc_type", sSocType, "");
        sSocTypeFetched = true;
    }
    const char* value = sSocType;

    if (!strncmp(value, "imx8mm", 6)
        || !strncmp(value, "imx943", 6)
        || !strncmp(value, "imx93", 5))
        return CONFIDENCE_NONE;

    // OCL converter is preferred on imx8mp for NV12 scale
    if (srcFormat == dstFormat && dstFormat == HAL_PIXEL_FORMAT_YCbCr_420_888) {
        if (srcWidth != dstWidth || srcHeight != dstHeight) {
            if (strstr(value, "imx8mp"))
                return CONFIDENCE_OCLCVT_PREFERRED;
        }
    }

    // OclCvt only supports RAW16 (SBGGR10) input for the following outputs:
    //   - RAW16 / YUV422I / NV12.
    // Any other RAW16 output is not supported.
    if (srcFormat == HAL_PIXEL_FORMAT_RAW16) {
        if (dstFormat != HAL_PIXEL_FORMAT_RAW16 &&
            dstFormat != HAL_PIXEL_FORMAT_YCbCr_422_I &&
            dstFormat != HAL_PIXEL_FORMAT_YCbCr_420_888 &&
            dstFormat != HAL_PIXEL_FORMAT_YCbCr_420_SP)
            return CONFIDENCE_NONE;
    }

    // General confidence for supported conversions
    return CONFIDENCE_OCLCVT;
}

int OclCvtDevice::HalPixelFormatToOclPixelFormat(uint32_t& halPixelFormat,
                                                  OCL_PIXEL_FORMAT& oclPixelFormat) {
    int ret = 0;
    switch (halPixelFormat) {
        case HAL_PIXEL_FORMAT_YCbCr_420_888:
        case HAL_PIXEL_FORMAT_YCbCr_420_SP:
            oclPixelFormat = OCL_FORMAT_NV12;
            break;
        case HAL_PIXEL_FORMAT_YCbCr_422_I:
            oclPixelFormat = OCL_FORMAT_YUYV;
            break;
        case HAL_PIXEL_FORMAT_YCbCr_422_SP:
            oclPixelFormat = OCL_FORMAT_NV16;
            break;
        case HAL_PIXEL_FORMAT_RGB_888:
            oclPixelFormat = OCL_FORMAT_RGB888;
            break;
        case HAL_PIXEL_FORMAT_RAW16:
            oclPixelFormat = OCL_FORMAT_SBGGR10;
            break;
        default:
            ALOGW("%s: unsupported halPixelFormat %d", __func__, halPixelFormat);
            ret = -1;
            break;
    }
    return ret;
}

void OclCvtDevice::ImxImageBufferToOclFormat(ImxImageBuffer& srcImgBuf, OCL_FORMAT& input_format,
                                             ImxImageBuffer& dstImgBuf, OCL_FORMAT& output_format) {
    int ret = 0;
    OCL_PIXEL_FORMAT srcPixelFormat, dstPixelFormat;

    ret = HalPixelFormatToOclPixelFormat(srcImgBuf.mFormat, srcPixelFormat);
    if (ret) {
        return;
    }

    ret |= HalPixelFormatToOclPixelFormat(dstImgBuf.mFormat, dstPixelFormat);

    if (ret) {
        dstPixelFormat = srcPixelFormat;
    }

    int h_offset = 0;
    int v_offset = 0;
    int crop_width = 0;
    int crop_height = 0;
    float h_scale_ratio = 1.0;
    float v_scale_ratio = 1.0;
    float scale_ratio = 1.0;

    // zoom in
    if ((srcImgBuf.mZoomRatio > 1.0) && (srcImgBuf.mWidth == dstImgBuf.mWidth) &&
        (srcImgBuf.mHeight == dstImgBuf.mHeight)) {
        crop_width = srcImgBuf.mWidth / srcImgBuf.mZoomRatio;
        crop_height = srcImgBuf.mHeight / srcImgBuf.mZoomRatio;
        h_offset = (srcImgBuf.mWidth - crop_width) / 2;
        v_offset = (srcImgBuf.mHeight - crop_height) / 2;
        goto set_format;
    }

    // Calculate scale ratios for each dimension
    crop_width = srcImgBuf.mWidth;
    crop_height = srcImgBuf.mHeightSpan;
    h_scale_ratio = (float)srcImgBuf.mWidth / dstImgBuf.mWidth;
    v_scale_ratio = (float)srcImgBuf.mHeightSpan / dstImgBuf.mHeight;

    scale_ratio = (h_scale_ratio < v_scale_ratio) ? h_scale_ratio : v_scale_ratio;

    if (scale_ratio > 1.0f) {
        crop_width = (int)(dstImgBuf.mWidth * scale_ratio);
        crop_height = (int)(dstImgBuf.mHeight * scale_ratio);

        if (crop_width > (int)srcImgBuf.mWidth)
            crop_width = srcImgBuf.mWidth;
        if (crop_height > (int)srcImgBuf.mHeightSpan)
            crop_height = srcImgBuf.mHeightSpan;

        h_offset = (srcImgBuf.mWidth - crop_width) / 2;
        v_offset = (srcImgBuf.mHeightSpan - crop_height) / 2;
    } else if (scale_ratio < 1.0f) {
        ALOGW("%s: cropping for upscale is not considered", __func__);
    }

set_format:
    input_format.format = srcPixelFormat;
    input_format.width = srcImgBuf.mWidth;
    input_format.height = srcImgBuf.mHeightSpan;
    input_format.stride = srcImgBuf.mStride;
    input_format.sliceheight = srcImgBuf.mHeightSpan;
    input_format.left = h_offset;
    input_format.top = v_offset;
    input_format.right = h_offset + crop_width;
    input_format.bottom = v_offset + crop_height;
    input_format.colorspace = OCL_COLORSPACE_BT709;

    output_format.format = dstPixelFormat;
    output_format.width = dstImgBuf.mWidth;
    output_format.height = dstImgBuf.mHeightSpan;
    output_format.stride = dstImgBuf.mStride;
    output_format.sliceheight = dstImgBuf.mHeightSpan;
    output_format.left = 0;
    output_format.top = 0;
    output_format.right = dstImgBuf.mWidth;
    output_format.bottom = dstImgBuf.mHeightSpan;
    output_format.colorspace = OCL_COLORSPACE_BT709;
}

void OclCvtDevice::ImxImageBufferToOclBuffer(ImxImageBuffer& imxImgBuf, OCL_BUFFER& oclBuf,
                                             OCL_FORMAT& oclFmt) {
    int ret = 0;
    OCL_FORMAT_PLANE_INFO plane_info;

    memset(&plane_info, 0, sizeof(plane_info));
    plane_info.ocl_format = &oclFmt;

    if (mHOcl == NULL) {
        ALOGE("%s: unexpected mHOcl NULL", __func__);
        return;
    }

    ret = m_ocl_getParam(mHOcl, OCL_PARAM_INDEX_FORMAT_PLANE_INFO, &plane_info);
    if (ret) {
        ALOGE("%s: m_ocl_getParam OCL_PARAM_INDEX_FORMAT_PLANE_INFO failed, ret %d", __func__, ret);
        return;
    }

    oclBuf.mem_type = mOclBufferType;
    oclBuf.plane_num = plane_info.plane_num;
    int offset = 0;

    if (mOclBufferType == OCL_MEM_TYPE_GPU) {
        for (int i = 0; i < oclBuf.plane_num; i++) {
            oclBuf.planes[i].paddr = (long long)imxImgBuf.mPhyAddr + (long long)offset;
            oclBuf.planes[i].size = plane_info.plane_size[i];
            offset += oclBuf.planes[i].size;
        }
    } else if (mOclBufferType == OCL_MEM_TYPE_DEVICE) {
        for (int i = 0; i < oclBuf.plane_num; i++) {
            oclBuf.planes[i].fd = (long long)imxImgBuf.mFd;
            oclBuf.planes[i].offset = (long long)offset;
            oclBuf.planes[i].size = plane_info.plane_size[i] + offset;
            offset += oclBuf.planes[i].size;
        }
    } else {
        for (int i = 0; i < oclBuf.plane_num; i++) {
            oclBuf.planes[i].vaddr = (long long)imxImgBuf.mVirtAddr + offset;
            oclBuf.planes[i].size = plane_info.plane_size[i];
            offset += oclBuf.planes[i].size;
        }
    }
}

int OclCvtDevice::convert(ImxImageBuffer& dstBuf, ImxImageBuffer& srcBuf) {
    int ret = 0;

    srcBuf.mFormat = unifyNV12Format(srcBuf.mFormat);
    dstBuf.mFormat = unifyNV12Format(dstBuf.mFormat);

    if (mHOcl == NULL) {
        ALOGE("%s: unexpected mHOcl NULL", __func__);
        return BAD_VALUE;
    }

    OCL_FORMAT input_format;
    OCL_FORMAT output_format;
    memset(&input_format, 0, sizeof(input_format));
    memset(&output_format, 0, sizeof(output_format));

    if ((mOclBufferType == OCL_MEM_TYPE_GPU) && (srcBuf.mPhyAddr == 0))
        GetBufferInfoFromHandle(srcBuf.buffer, srcBuf, true, false);
    if ((mOclBufferType == OCL_MEM_TYPE_GPU) && (dstBuf.mPhyAddr == 0))
        GetBufferInfoFromHandle(dstBuf.buffer, dstBuf, true, false);

    mOclCvtLock.lock();

    ImxImageBufferToOclFormat(srcBuf, input_format, dstBuf, output_format);

    if (memcmp(&input_format, &mInputFormat, sizeof(OCL_FORMAT)) != 0 ||
        memcmp(&output_format, &mOutputFormat, sizeof(OCL_FORMAT)) != 0) {
        ret = m_ocl_setParam(mHOcl, OCL_PARAM_INDEX_INPUT_FORMAT, &input_format);
        if (ret) {
            ALOGE("%s: m_ocl_setParam OCL_PARAM_INDEX_INPUT_FORMAT failed, ret %d", __func__, ret);
            goto finish;
        }

        ret = m_ocl_setParam(mHOcl, OCL_PARAM_INDEX_OUTPUT_FORMAT, &output_format);
        if (ret) {
            ALOGE("%s: m_ocl_setParam OCL_PARAM_INDEX_OUTPUT_FORMAT failed, ret %d", __func__, ret);
            goto finish;
        }

        mInputFormat = input_format;
        mOutputFormat = output_format;
    }

    {
        OCL_BUFFER inBuffer;
        OCL_BUFFER outBuffer;
        memset(&inBuffer, 0, sizeof(inBuffer));
        memset(&outBuffer, 0, sizeof(outBuffer));

        ImxImageBufferToOclBuffer(srcBuf, inBuffer, input_format);
        ImxImageBufferToOclBuffer(dstBuf, outBuffer, output_format);

        if (mDebug) {
            ALOGD("%s: convert begin, src: %dx%d fmt %d, dst: %dx%d fmt %d, ",
                  __func__, input_format.width, input_format.height, input_format.format,
                  output_format.width, output_format.height, output_format.format);
        }

        ret = m_ocl_convert(mHOcl, &inBuffer, &outBuffer);
        if (ret) {
            ALOGE("%s: m_ocl_convert failed, ret %d", __func__, ret);
            goto finish;
        }

        if (mDebug) {
            OCL_RUN_TIME time;
            m_ocl_getParam(mHOcl, OCL_PARAM_INDEX_RUN_TIME, &time);
            ALOGD("%s: convert success, run_time: %d us, kernel_time: %d us",
                  __func__, time.run_time, time.kernel_time);
        }
    }

finish:
    mOclCvtLock.unlock();

    return ret;
}

int OclCvtDevice::setSrcBufferFormat(ImxImageBuffer* buf) {
    int ret = ImageProcessDevice::setSrcBufferFormat(buf);
    if (ret || !buf)
        return ret;

    if (mHOcl == NULL) {
        ALOGE("%s: unexpected mHOcl NULL", __func__);
        return BAD_VALUE;
    }

    OCL_FORMAT input_format;
    memset(&input_format, 0, sizeof(input_format));

    OCL_PIXEL_FORMAT srcPixelFormat;
    ret = HalPixelFormatToOclPixelFormat(buf->mFormat, srcPixelFormat);
    if (ret) {
        ALOGE("%s: failed, ret=%d", __func__, ret);
        return ret;
    }

    input_format.format = srcPixelFormat;
    input_format.width = buf->mWidth;
    input_format.height = buf->mHeight;
    input_format.stride = buf->mStride;
    input_format.sliceheight = buf->mHeight;
    input_format.left = 0;
    input_format.top = 0;
    input_format.right = buf->mWidth;
    input_format.bottom = buf->mHeight;
    input_format.colorspace = OCL_COLORSPACE_BT709;

    ret = m_ocl_setParam(mHOcl, OCL_PARAM_INDEX_INPUT_FORMAT, &input_format);
    if (ret) {
        ALOGE("%s: m_ocl_setParam OCL_PARAM_INDEX_INPUT_FORMAT failed, ret %d", __func__, ret);
    }

    mInputFormat = input_format;

    return ret;
}

int OclCvtDevice::setDstBufferFormat(ImxImageBuffer* buf) {
    int ret = ImageProcessDevice::setDstBufferFormat(buf);
    if (ret || !buf)
        return ret;

    if (mHOcl == NULL) {
        ALOGE("%s: unexpected mHOcl NULL", __func__);
        return BAD_VALUE;
    }

    OCL_FORMAT output_format;
    memset(&output_format, 0, sizeof(output_format));

    OCL_PIXEL_FORMAT dstPixelFormat;
    ret = HalPixelFormatToOclPixelFormat(buf->mFormat, dstPixelFormat);
    if (ret) {
        ALOGD("%s: failed, ret=%d", __func__, ret);
        dstPixelFormat = mInputFormat.format;
    }

    output_format.format = dstPixelFormat;
    output_format.width = buf->mWidth;
    output_format.height = buf->mHeight;
    output_format.stride = buf->mStride;
    output_format.sliceheight = buf->mHeight;
    output_format.left = 0;
    output_format.top = 0;
    output_format.right = buf->mWidth;
    output_format.bottom = buf->mHeight;
    output_format.colorspace = OCL_COLORSPACE_BT709;

    ret = m_ocl_setParam(mHOcl, OCL_PARAM_INDEX_OUTPUT_FORMAT, &output_format);
    if (ret) {
        ALOGE("%s: m_ocl_setParam OCL_PARAM_INDEX_OUTPUT_FORMAT failed, ret %d", __func__, ret);
    }

    mOutputFormat = output_format;

    return ret;
}

// Warp file parsing for dewarp support
#pragma pack(push, 1)
typedef struct tag_warp_file_head {
    uint32_t head_size;
    uint8_t format_version;
    uint8_t algorithm;
    uint8_t reserved1;
    uint8_t reserved2;
    uint32_t width;
    uint32_t height;
    uint32_t arb_start_x;
    uint32_t arb_start_y;
    uint32_t arb_start_xx;
    uint32_t arb_start_xy;
    uint32_t arb_start_yx;
    uint32_t arb_start_yy;
} TWarpFileHead;
#pragma pack(pop)

#define WARP_FILE_HEAD_SIZE (uint32_t)sizeof(TWarpFileHead)
#define WARP_PNT_32BPP 1

int OclCvtDevice::parse_warp_file(const char* file_name, OCL_WARP_PARAM* warp_param) {
    int fd = -1;
    int ret = 0;
    struct stat statbuf;
    uint32_t file_size = 0;
    TWarpFileHead warpFileHead;
    uint32_t data_size = 0;
    uint32_t aligned_size = 0;

    if ((file_name == NULL) || (warp_param == NULL)) {
        ALOGE("%s: file_name %p, warp_param %p", __func__, file_name, warp_param);
        return -1;
    }

    ALOGI("%s: parse %s", __func__, file_name);
    fd = open(file_name, O_RDONLY);
    if (fd < 0) {
        ALOGW("%s: open %s failed, %s", __func__, file_name, strerror(errno));
        return -1;
    }

    ret = fstat(fd, &statbuf);
    if (ret) {
        ALOGE("%s: stat %s failed", __func__, file_name);
        ret = -1;
        goto fail;
    }

    file_size = statbuf.st_size;
    if (file_size < WARP_FILE_HEAD_SIZE) {
        ALOGE("%s: file too small", __func__);
        ret = -1;
        goto fail;
    }

    ret = read(fd, &warpFileHead, WARP_FILE_HEAD_SIZE);
    if ((uint32_t)ret != WARP_FILE_HEAD_SIZE) {
        ALOGE("%s: read head failed", __func__);
        ret = -1;
        goto fail;
    }

    if ((warpFileHead.format_version == 1) && (warpFileHead.head_size != WARP_FILE_HEAD_SIZE)) {
        ret = -1;
        goto fail;
    }

    if (warpFileHead.algorithm != WARP_PNT_32BPP) {
        ret = -1;
        goto fail;
    }

    data_size = warpFileHead.width * warpFileHead.height * 4;
    if (data_size + warpFileHead.head_size != file_size) {
        ret = -1;
        goto fail;
    }

    aligned_size = (data_size + 4095) & ~4095;
    if (mOclBufferType == OCL_MEM_TYPE_DEVICE) {
        ret = AllocPhyBuffer(aligned_size, 1, HAL_PIXEL_FORMAT_BLOB, mWarpBuffer, false, false,
                             true);
        if (ret) {
            ALOGE("%s: AllocPhyBuffer for mWarpBuffer failed", __func__);
            ret = -1;
            goto fail;
        }
        warp_param->buf.planes[0].fd = mWarpBuffer.mFd;
        warp_param->buf.planes[0].size = aligned_size;
        warp_param->buf.planes[0].vaddr = (long long)mWarpBuffer.mVirtAddr;
    } else {
        void* ptr = malloc(aligned_size);
        if (!ptr) {
            ALOGE("%s: malloc failed", __func__);
            ret = -1;
            goto fail;
        }
        warp_param->buf.planes[0].fd = -1;
        warp_param->buf.planes[0].size = aligned_size;
        warp_param->buf.planes[0].vaddr = (long long)ptr;
    }

    if (data_size > (uint32_t)(warp_param->buf.planes[0].size)) {
        ret = -1;
        goto fail;
    }

    ret = read(fd, (void*)warp_param->buf.planes[0].vaddr, data_size);
    if ((uint32_t)ret != data_size) {
        ret = -1;
        goto fail;
    }

    warp_param->enable = 1;
    warp_param->map = OCL_WARP_MAP_PNT;
    warp_param->width = warpFileHead.width;
    warp_param->height = warpFileHead.height;
    warp_param->buf.mem_type = mOclBufferType;
    warp_param->buf.plane_num = 1;

    close(fd);
    return 0;

fail:
    if (warp_param->buf.planes[0].fd > 0)
        FreePhyBuffer(mWarpBuffer.buffer);
    else if (warp_param->buf.planes[0].vaddr != 0)
        free((void*)warp_param->buf.planes[0].vaddr);

    close(fd);
    return ret;
}

} // namespace fsl
