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
#define LOG_TAG "CpuDevice"

#include "CpuDevice.h"

#include <cutils/log.h>
#include <utils/Errors.h>
#include <cutils/properties.h>
#include <libyuv.h>
#include <libyuv/convert.h>
#include <system/graphics.h>

namespace fsl {

CpuDevice::CpuDevice() {
    ALOGI("CpuDevice: initialized");
}

bool CpuDevice::IsCscSupportByCPU(int srcFormat, int dstFormat) {
    // yuyv -> nv12
    if ((dstFormat == HAL_PIXEL_FORMAT_YCbCr_420_888) &&
        (srcFormat == HAL_PIXEL_FORMAT_YCbCr_422_I))
        return true;

    // nv12 -> nv21
    if ((srcFormat == HAL_PIXEL_FORMAT_YCbCr_420_888) &&
        (dstFormat == HAL_PIXEL_FORMAT_YCrCb_420_SP))
        return true;

    // nv12 -> yv12
    if ((srcFormat == HAL_PIXEL_FORMAT_YCbCr_420_888) && (dstFormat == HAL_PIXEL_FORMAT_YV12))
        return true;

    // bggr -> rgb888 -> nv12
    if ((srcFormat == HAL_PIXEL_FORMAT_RAW16) && (dstFormat == HAL_PIXEL_FORMAT_YCbCr_420_888))
        return true;

    return false;
}

float CpuDevice::sniff(uint32_t srcFormat, uint32_t dstFormat, uint32_t srcWidth,
                       uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight) {
    (void)srcWidth;
    (void)srcHeight;
    (void)dstWidth;
    (void)dstHeight;

    // Same format: always can copy/resize
    if (srcFormat == dstFormat)
        return CONFIDENCE_CPU;

    // CSC supported?
    if (IsCscSupportByCPU(srcFormat, dstFormat))
        return CONFIDENCE_CPU;

    return CONFIDENCE_NONE;
}

int CpuDevice::convert(ImxImageBuffer& dstBuf, ImxImageBuffer& srcBuf) {
    ImxImageBuffer resizeBuf;
    memset(&resizeBuf, 0, sizeof(resizeBuf));
    int ret;
    bool bResize = false;

    srcBuf.mFormat = unifyNV12Format(srcBuf.mFormat);
    dstBuf.mFormat = unifyNV12Format(dstBuf.mFormat);

    if (srcBuf.mVirtAddr == NULL)
        GetBufferInfoFromHandle(srcBuf.buffer, srcBuf, false, true);
    if (dstBuf.mVirtAddr == NULL)
        GetBufferInfoFromHandle(dstBuf.buffer, dstBuf, false, true);

    if ((srcBuf.mVirtAddr == NULL) || (dstBuf.mVirtAddr == NULL)) {
        ALOGE("CpuDevice::%s: bad virtual addr", __func__);
        return BAD_VALUE;
    }

    if (mDebug) {
        ALOGD("CpuDevice::%s: convert"
              " src(fmt=0x%x, %ux%u stride=%u hspan=%u zoom=%.2f)"
              " dst(fmt=0x%x, %ux%u stride=%u hspan=%u)",
              __func__, srcBuf.mFormat, srcBuf.mWidth, srcBuf.mHeight, srcBuf.mStride,
              srcBuf.mHeightSpan, srcBuf.mZoomRatio, dstBuf.mFormat,
              dstBuf.mWidth, dstBuf.mHeight, dstBuf.mStride, dstBuf.mHeightSpan);
    }

    // Case 1: same format, same resolution -> copy
    if ((srcBuf.mFormat == dstBuf.mFormat) && (srcBuf.mWidth == dstBuf.mWidth) &&
        (srcBuf.mHeight == dstBuf.mHeight)) {
        if (srcBuf.mZoomRatio <= 1.0) {
            memcpy((uint8_t*)dstBuf.mVirtAddr, (uint8_t*)srcBuf.mVirtAddr, dstBuf.mFormatSize);
        } else if (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_888) {
            int crop_width = srcBuf.mWidth / srcBuf.mZoomRatio;
            int crop_height = srcBuf.mHeight / srcBuf.mZoomRatio;

            resizeBuf.mFormatSize = srcBuf.mFormatSize;
            ret = AllocPhyBuffer(crop_width, crop_height, srcBuf.mFormat, resizeBuf, true, false,
                                 true);
            if (ret) {
                ALOGE("%s:%d AllocPhyBuffer failed", __func__, __LINE__);
                return BAD_VALUE;
            }

            decreaseNV12WithCut((uint8_t*)srcBuf.mVirtAddr, srcBuf.mWidth, srcBuf.mHeight,
                                (uint8_t*)resizeBuf.mVirtAddr, resizeBuf.mWidth, resizeBuf.mHeight);

            uint8_t* pUVSrcStart =
                    (uint8_t*)resizeBuf.mVirtAddr + resizeBuf.mWidth * resizeBuf.mHeight;
            uint8_t* pUVDstStart = (uint8_t*)dstBuf.mVirtAddr + dstBuf.mWidth * dstBuf.mHeight;

            ret = libyuv::NV12Scale(static_cast<uint8_t*>(resizeBuf.mVirtAddr), resizeBuf.mWidth,
                                    static_cast<uint8_t*>(pUVSrcStart), resizeBuf.mWidth,
                                    resizeBuf.mWidth, resizeBuf.mHeight,
                                    static_cast<uint8_t*>(dstBuf.mVirtAddr), dstBuf.mWidth,
                                    static_cast<uint8_t*>(pUVDstStart), dstBuf.mWidth,
                                    dstBuf.mWidth, dstBuf.mHeight, libyuv::FilterMode::kFilterNone);

            FreePhyBuffer(resizeBuf.buffer);
            if (ret != 0) {
                ALOGE("%s: NV12Scale failed %d", __func__, ret);
                return ret;
            }
        }

        if (mDebug) {
            ALOGD("CpuDevice::%s: case 1 success", __func__);
        }
        return 0;
    }

    // Case 2: same format, different resolution -> resize
    if (srcBuf.mFormat == dstBuf.mFormat) {
        if (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_422_I)
            ret = yuv422iResize((uint8_t*)srcBuf.mVirtAddr, srcBuf.mWidth, srcBuf.mHeight,
                                (uint8_t*)dstBuf.mVirtAddr, dstBuf.mWidth, dstBuf.mHeight);
        else if (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_422_SP)
            ret = yuv422spResize((uint8_t*)srcBuf.mVirtAddr, srcBuf.mWidth, srcBuf.mHeight,
                                 (uint8_t*)dstBuf.mVirtAddr, dstBuf.mWidth, dstBuf.mHeight,
                                 dstBuf.mStride, srcBuf.mHeightSpan);
        else if (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_888)
            ret = yuv420spResize((uint8_t*)srcBuf.mVirtAddr, srcBuf.mWidth, srcBuf.mHeight,
                                 (uint8_t*)dstBuf.mVirtAddr, dstBuf.mWidth, dstBuf.mHeight);
        else {
            ALOGE("%s: resize unsupported format 0x%x", __func__, srcBuf.mFormat);
            ret = -EINVAL;
        }
        if (mDebug) {
            ALOGD("CpuDevice::%s: case 2 end, ret=%d", __func__, ret);
        }
        return ret;
    }

    // filter out unsupported CSC format
    if (!IsCscSupportByCPU(srcBuf.mFormat, dstBuf.mFormat)) {
        ALOGE("%s: format convert from 0x%x to 0x%x not supported", __func__, srcBuf.mFormat,
              dstBuf.mFormat);
        return -EINVAL;
    }

    // Case 3: different format, different resolution -> resize first
    if ((srcBuf.mWidth != dstBuf.mWidth) || (srcBuf.mHeight != dstBuf.mHeight)) {
        ret = AllocPhyBuffer(dstBuf.mStride, dstBuf.mHeight, srcBuf.mFormat, resizeBuf);
        if (ret) {
            ALOGE("%s:%d AllocPhyBuffer failed", __func__, __LINE__);
            return -EINVAL;
        }

        if (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_422_I)
            yuv422iResize((uint8_t*)srcBuf.mVirtAddr, srcBuf.mWidth, srcBuf.mHeight,
                          (uint8_t*)resizeBuf.mVirtAddr, resizeBuf.mWidth, resizeBuf.mHeight);
        else if (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_422_SP)
            yuv422spResize((uint8_t*)srcBuf.mVirtAddr, srcBuf.mWidth, srcBuf.mHeight,
                           (uint8_t*)resizeBuf.mVirtAddr, resizeBuf.mWidth, resizeBuf.mHeight,
                           resizeBuf.mStride, srcBuf.mHeightSpan);
        else if (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_888)
            yuv420spResize((uint8_t*)srcBuf.mVirtAddr, srcBuf.mWidth, srcBuf.mHeight,
                           (uint8_t*)resizeBuf.mVirtAddr, resizeBuf.mWidth, resizeBuf.mHeight);

        SwitchImxBuf(srcBuf, resizeBuf);
        bResize = true;
    }

    // Case 4: different format, same resolution -> CSC
    if ((dstBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_888) &&
        (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_422_I)) {
        convertYUYVtoNV12SP((uint8_t*)srcBuf.mVirtAddr, (uint8_t*)dstBuf.mVirtAddr, dstBuf.mWidth,
                            dstBuf.mHeight, srcBuf.mStride, dstBuf.mStride);
    } else if ((srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_888) &&
               (dstBuf.mFormat == HAL_PIXEL_FORMAT_YCrCb_420_SP)) {
        convertNV12toNV21(dstBuf, srcBuf);
    } else if ((srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_888) &&
               (dstBuf.mFormat == HAL_PIXEL_FORMAT_YV12)) {
        convertNV12toYV12((uint8_t*)srcBuf.mVirtAddr, (uint8_t*)dstBuf.mVirtAddr, dstBuf.mWidth,
                          dstBuf.mHeight);
    } else if ((srcBuf.mFormat == HAL_PIXEL_FORMAT_RAW16) &&
               (dstBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_888)) {
        convertBGGRtoNV12((uint8_t*)srcBuf.mVirtAddr, (uint8_t*)dstBuf.mVirtAddr, dstBuf.mWidth,
                          dstBuf.mHeight);
    }

    if (bResize) {
        SwitchImxBuf(srcBuf, resizeBuf);
        FreePhyBuffer(resizeBuf.buffer);
    }

    if (mDebug) {
        ALOGD("CpuDevice::%s: case 3,4 success", __func__);
    }
    return 0;
}

int CpuDevice::convertNV12toNV21(ImxImageBuffer& dstBuf, ImxImageBuffer& srcBuf) {
    int Ysize = srcBuf.mWidth * srcBuf.mHeight;
    int UVsize = srcBuf.mWidth * srcBuf.mHeight >> 2;
    uint8_t* srcIn = (uint8_t*)srcBuf.mVirtAddr;
    uint8_t* dstOut = (uint8_t*)dstBuf.mVirtAddr;
    uint32_t* UVout = (uint32_t*)(dstOut + Ysize);
    int size = (srcBuf.mSize > dstBuf.mSize) ? dstBuf.mSize : srcBuf.mSize;

    memcpy(dstOut, srcIn, size);
    Revert16BitEndian((uint8_t*)UVout, (uint8_t*)UVout, UVsize / 2);

    return 0;
}

void CpuDevice::convertYUYVtoNV12SP(uint8_t* inputBuffer, uint8_t* outputBuffer, int width,
                                    int height, int srcStride, int dstStride) {
#define u32 unsigned int
#define u8 unsigned char

    u32 h, w;
    u32 nHeight = height;
    u32 nWidthDiv4 = srcStride / 4;

    u32* pYSrcOffset = (u32*)inputBuffer;
    u32 value = 0;
    u32 value2 = 0;

    u32* pYDstOffset = (u32*)outputBuffer;
    u32* pUVDstOffset = (u32*)(((u8*)(outputBuffer)) + dstStride * height);

    (void)width;

    for (h = 0; h < nHeight; h++) {
        if (!(h & 0x1)) {
            for (w = 0; w < nWidthDiv4; w++) {
                value = (*pYSrcOffset);
                value2 = (*(pYSrcOffset + 1));
                *pYDstOffset = ((value & 0x000000ff) >> 0) | ((value & 0x00ff0000) >> 8) |
                        ((value2 & 0x000000ff) << 16) | ((value2 & 0x00ff0000) << 8);
                pYDstOffset += 1;

#ifdef PLATFORM_VERSION_4
                *pUVDstOffset = ((value & 0xff000000) >> 24) | ((value & 0x0000ff00) >> 0) |
                        ((value2 & 0xff000000) >> 8) | ((value2 & 0x0000ff00) << 16);
#else
                *pUVDstOffset = ((value & 0x0000ff00) >> 8) | ((value & 0xff000000) >> 16) |
                        ((value2 & 0x0000ff00) << 8) | ((value2 & 0xff000000) << 0);
#endif
                pUVDstOffset += 1;
                pYSrcOffset += 2;
            }
        } else {
            for (w = 0; w < nWidthDiv4; w++) {
                value = (*pYSrcOffset);
                value2 = (*(pYSrcOffset + 1));
                *pYDstOffset = ((value & 0x000000ff) >> 0) | ((value & 0x00ff0000) >> 8) |
                        ((value2 & 0x000000ff) << 16) | ((value2 & 0x00ff0000) << 8);
                pYSrcOffset += 2;
                pYDstOffset += 1;
            }
        }
        pYDstOffset += ((dstStride - srcStride) / 4);
        pUVDstOffset += ((dstStride - srcStride) / 4 / 2);
    }
}

void CpuDevice::convertNV12toYV12(uint8_t* inputBuffer, uint8_t* outputBuffer, int width,
                                  int height) {
    int size = width * height;
    memcpy(outputBuffer, inputBuffer, size);
    uint8_t* ptrV1 = outputBuffer + size;
    uint8_t* ptrV2 = inputBuffer + size + 1;
    uint8_t* ptrU1 = outputBuffer + size * 5 / 4;
    uint8_t* ptrU2 = inputBuffer + size;
    int n = 0;
    while (n < size / 4) {
        *(ptrV1) = *(ptrV2);
        ptrV1++;
        ptrV2 = ptrV2 + 2;
        *(ptrU1) = *(ptrU2);
        ptrU1++;
        ptrU2 = ptrU2 + 2;
        n++;
    }
}

void CpuDevice::convertBGGRtoNV12(uint8_t* inputBuffer, uint8_t* outputBuffer, int width,
                                  int height) {
    int size = width * height * 3;
    uint8_t* rgb888 = (uint8_t*)malloc(size);
    if (rgb888 == NULL) {
        ALOGE("%s: Failed to allocate RGB888 buffer", __func__);
        return;
    }

    SbggrToRgb888((const uint16_t*)inputBuffer, rgb888, width, height);
    Rgb888ToNv12((const uint8_t*)rgb888, outputBuffer, width, height);

    free(rgb888);
}

int CpuDevice::convertMultiSrc(ImxImageBuffer& dstBuf, ImxImageBuffer& srcBufA,
                                ImxImageBuffer& srcBufB) {
    srcBufA.mFormat = unifyNV12Format(srcBufA.mFormat);
    srcBufB.mFormat = unifyNV12Format(srcBufB.mFormat);
    dstBuf.mFormat  = unifyNV12Format(dstBuf.mFormat);

    if (!((srcBufA.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_888) &&
          (srcBufA.mFormat == srcBufB.mFormat) &&
          (srcBufA.mFormat == dstBuf.mFormat))) {
        ALOGE("CpuDevice::%s: only support NV12, srcA 0x%x srcB 0x%x dst 0x%x",
              __func__, srcBufA.mFormat, srcBufB.mFormat, dstBuf.mFormat);
        return BAD_VALUE;
    }

    if (!((srcBufA.mWidth == srcBufB.mWidth) && (srcBufA.mHeight == srcBufB.mHeight) &&
          (srcBufA.mWidth == dstBuf.mWidth)  && (srcBufA.mHeight == dstBuf.mHeight))) {
        ALOGE("CpuDevice::%s: only support same resolution, srcA %ux%u srcB %ux%u dst %ux%u",
              __func__, srcBufA.mWidth, srcBufA.mHeight,
              srcBufB.mWidth, srcBufB.mHeight, dstBuf.mWidth, dstBuf.mHeight);
        return BAD_VALUE;
    }

    if (mDebug) {
        ALOGD("CpuDevice::%s: convertMultiSrc"
              " srcA(fmt=0x%x, %ux%u stride=%u hspan=%u)"
              " srcB(fmt=0x%x, %ux%u stride=%u hspan=%u)"
              " dst(fmt=0x%x, %ux%u stride=%u hspan=%u)",
              __func__,
              srcBufA.mFormat, srcBufA.mWidth, srcBufA.mHeight, srcBufA.mStride, srcBufA.mHeightSpan,
              srcBufB.mFormat, srcBufB.mWidth, srcBufB.mHeight, srcBufB.mStride, srcBufB.mHeightSpan,
              dstBuf.mFormat,  dstBuf.mWidth,  dstBuf.mHeight,  dstBuf.mStride,  dstBuf.mHeightSpan);
    }

    if (srcBufA.mVirtAddr == NULL)
        GetBufferInfoFromHandle(srcBufA.buffer, srcBufA, false, true);
    if (srcBufB.mVirtAddr == NULL)
        GetBufferInfoFromHandle(srcBufB.buffer, srcBufB, false, true);
    if (dstBuf.mVirtAddr == NULL)
        GetBufferInfoFromHandle(dstBuf.buffer, dstBuf, false, true);

    if ((srcBufA.mVirtAddr == NULL) || (srcBufB.mVirtAddr == NULL) || (dstBuf.mVirtAddr == NULL)) {
        ALOGE("CpuDevice::%s: bad virtual addr srcA %p srcB %p dst %p",
              __func__, srcBufA.mVirtAddr, srcBufB.mVirtAddr, dstBuf.mVirtAddr);
        return BAD_VALUE;
    }

    MergeImageNV12(srcBufA.mVirtAddr, srcBufB.mVirtAddr, dstBuf.mVirtAddr,
                   dstBuf.mWidth, dstBuf.mHeight);
    return 0;
}

// Merge two NV12 frames side by side into dst.
// Left half comes from srcA (every other column), right half from srcB.
void CpuDevice::MergeImageNV12(void* srcA, void* srcB, void* dst,
                                uint32_t width, uint32_t height) {
    uint32_t ySize   = width * height;
    uint32_t yStride = width;

    uint8_t  *ySrcA = (uint8_t*)srcA;
    uint8_t  *ySrcB = (uint8_t*)srcB;
    uint8_t  *yDst  = (uint8_t*)dst;

    uint32_t cStride = width;
    uint32_t cHeight = height / 2;
    uint16_t *cSrcA  = (uint16_t*)(ySrcA + ySize);
    uint16_t *cSrcB  = (uint16_t*)(ySrcB + ySize);
    uint16_t *cDst   = (uint16_t*)(yDst  + ySize);

    // Merge Y plane: left half from srcA, right half from srcB.
    uint32_t yOffset = 0;
    for (uint32_t row = 0; row < height; row++) {
        uint8_t *yRowSrcA = ySrcA + yOffset;
        uint8_t *yRowSrcB = ySrcB + yOffset;
        uint8_t *yRowDst  = yDst  + yOffset;

        for (uint32_t col = 0; col < width / 2; col++)
            yRowDst[col] = yRowSrcA[col * 2];

        for (uint32_t col = width / 2; col < width; col++)
            yRowDst[col] = yRowSrcB[(col - width / 2) * 2];

        yOffset += yStride;
    }

    // Merge UV plane (interleaved, each uint16 is one UV pair).
    uint32_t cOffset = 0;
    for (uint32_t row = 0; row < cHeight; row++) {
        uint16_t *cRowSrcA = cSrcA + cOffset;
        uint16_t *cRowSrcB = cSrcB + cOffset;
        uint16_t *cRowDst  = cDst  + cOffset;

        for (uint32_t col = 0; col < width / 4; col++)
            cRowDst[col] = cRowSrcA[col * 2];

        for (uint32_t col = width / 4; col < width / 2; col++)
            cRowDst[col] = cRowSrcB[(col - width / 4) * 2];

        cOffset += cStride / 2; // unit is uint16
    }
}

} // namespace fsl
