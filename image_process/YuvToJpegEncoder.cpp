/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (C) 2012-2016 Freescale Semiconductor, Inc.
 * Copyright 2017-2023, 2025-2026 NXP
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

#include "YuvToJpegEncoder.h"

#include <hardware/hardware.h>
#include <log/log.h>

#include "ImageUtils.h"

YuvToJpegEncoder *YuvToJpegEncoder::create(int format) {
    // Only ImageFormat.NV21 and ImageFormat.YUY2 are supported
    // for now.
    ALOGI("YuvToJpegEncoder::create, format 0x%x", format);
    if ((format == HAL_PIXEL_FORMAT_YCbCr_420_SP) || (format == HAL_PIXEL_FORMAT_YCbCr_420_888)) {
        return new Yuv420SpToJpegEncoder(format);
    } else if (format == HAL_PIXEL_FORMAT_YCbCr_422_I) {
        return new Yuv422IToJpegEncoder(format);
    } else if (format == HAL_PIXEL_FORMAT_YCbCr_422_SP) {
        return new Yuv422SpToJpegEncoder(format);
    } else {
        ALOGE("YuvToJpegEncoder:create format:%d not support", format);
        return NULL;
    }
}

YuvToJpegEncoder::YuvToJpegEncoder(int format)
      : fNumPlanes(1),
        color(1),
        mPixelFormat(format),
        mDebug(false) {}

int YuvToJpegEncoder::encode(void *inYuv, int quality, void *outBuf, int outSize, int outWidth,
                             int outHeight, const void *app1Buffer, size_t app1Size, bool debug) {
    mDebug = debug;
    jpeg_compress_struct cinfo;
    jpegBuilder_error_mgr sk_err;
    jpegBuilder_destination_mgr dest_mgr((uint8_t *)outBuf, outSize);
    memset(&cinfo, 0, sizeof(cinfo));

    cinfo.err = jpeg_std_error(&sk_err);
    jpeg_create_compress(&cinfo);

    cinfo.dest = &dest_mgr;

    setJpegCompressStruct(&cinfo, outWidth, outHeight, quality);

    jpeg_start_compress(&cinfo, TRUE);

    /* If APP1 data was passed in, use it */
    if (app1Buffer && app1Size) {
        jpeg_write_marker(&cinfo, JPEG_APP0 + 1, static_cast<const JOCTET *>(app1Buffer), app1Size);
    }

    if (mDebug)
        ALOGI("%s: before compress", __func__);
    compress(&cinfo, (uint8_t *)inYuv);
    if (mDebug)
        ALOGI("%s: after compress", __func__);

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    return dest_mgr.jpegsize;
}

void YuvToJpegEncoder::setJpegCompressStruct(jpeg_compress_struct *cinfo, int width, int height,
                                             int quality) {
    cinfo->image_width = width;
    cinfo->image_height = height;
    cinfo->input_components = 3;
    cinfo->in_color_space = JCS_YCbCr;
    jpeg_set_defaults(cinfo);

    jpeg_set_quality(cinfo, quality, TRUE);
    jpeg_set_colorspace(cinfo, JCS_YCbCr);
    cinfo->raw_data_in = TRUE;
    cinfo->dct_method = JDCT_IFAST;
    configSamplingFactors(cinfo);
}

int YuvToJpegEncoder::yuvResize(uint8_t *srcBuf, int srcWidth, int srcHeight, uint8_t *dstBuf,
                                int dstWidth, int dstHeight) {
    return yuv422iResize(srcBuf, srcWidth, srcHeight, dstBuf, dstWidth, dstHeight);
}

// /////////////////////////////////////////////////////////////////
Yuv420SpToJpegEncoder::Yuv420SpToJpegEncoder(int format) : YuvToJpegEncoder(format) {
    fNumPlanes = 2;
    color = 0;
}

void Yuv420SpToJpegEncoder::compress(jpeg_compress_struct *cinfo, uint8_t *yuv) {
    JSAMPROW y[16];
    JSAMPROW cb[8];
    JSAMPROW cr[8];
    JSAMPARRAY planes[3];

    planes[0] = y;
    planes[1] = cb;
    planes[2] = cr;

    int width = cinfo->image_width;
    int height = cinfo->image_height;
    uint8_t *yPlanar = yuv;
    uint8_t *vuPlanar = yuv + width * height;
    uint8_t *uRows = new uint8_t[8 * (width >> 1)];
    uint8_t *vRows = new uint8_t[8 * (width >> 1)];
    int processLines = DEINTERLEAVE_LINES_ONE_TIME;

    int scanCount = 0;
    if (mDebug)
        ALOGI("%s: before scan line", __func__);

    // process 16 lines of Y and 8 lines of U/V each time.
    while (cinfo->next_scanline < cinfo->image_height) {
        if (cinfo->next_scanline + DEINTERLEAVE_LINES_ONE_TIME > cinfo->image_height)
            processLines = cinfo->image_height - cinfo->next_scanline;
        // deitnerleave u and v
        deinterleave(vuPlanar, uRows, vRows, cinfo->next_scanline, width, height, processLines);

        for (int i = 0; i < processLines; i++) {
            // y row
            y[i] = yPlanar + (cinfo->next_scanline + i) * width;

            // construct u row and v row
            if ((i & 1) == 0) {
                // height and width are both halved because of downsampling
                int offset = (i >> 1) * (width >> 1);
                cb[i / 2] = uRows + offset;
                cr[i / 2] = vRows + offset;
            }
        }
        jpeg_write_raw_data(cinfo, planes, 16);
        scanCount++;
    }

    if (mDebug)
        ALOGI("%s: after scan line, count %d", __func__, scanCount);

    delete[] uRows;
    delete[] vRows;
}

void Yuv420SpToJpegEncoder::deinterleave(uint8_t *vuPlanar, uint8_t *uRows, uint8_t *vRows,
                                         int rowIndex, int width, int height, int processLines) {
    for (int row = 0; row < processLines / 2; ++row) {
        int hoff = (rowIndex >> 1) + row;
        if (hoff >= (height >> 1)) {
            return;
        }
        int offset = hoff * width;
        uint8_t *vu = vuPlanar + offset;
        for (int i = 0; i < (width >> 1); ++i) {
            int index = row * (width >> 1) + i;
            uRows[index] = vu[0];
            vRows[index] = vu[1];
            vu += 2;
        }
    }
}

void Yuv420SpToJpegEncoder::configSamplingFactors(jpeg_compress_struct *cinfo) {
    // cb and cr are horizontally downsampled and vertically downsampled as
    // well.
    cinfo->comp_info[0].h_samp_factor = 2;
    cinfo->comp_info[0].v_samp_factor = 2;
    cinfo->comp_info[1].h_samp_factor = 1;
    cinfo->comp_info[1].v_samp_factor = 1;
    cinfo->comp_info[2].h_samp_factor = 1;
    cinfo->comp_info[2].v_samp_factor = 1;
}

int Yuv420SpToJpegEncoder::yuvResize(uint8_t *srcBuf, int srcWidth, int srcHeight, uint8_t *dstBuf,
                                     int dstWidth, int dstHeight) {
    return yuv420spResize(srcBuf, srcWidth, srcHeight, dstBuf, dstWidth, dstHeight);
}

// /////////////////////////////////////////////////////////////////////////////
Yuv422IToJpegEncoder::Yuv422IToJpegEncoder(int format) : YuvToJpegEncoder(format) {
    fNumPlanes = 1;
    color = 1;
}

void Yuv422IToJpegEncoder::compress(jpeg_compress_struct *cinfo, uint8_t *yuv) {
    JSAMPROW y[16];
    JSAMPROW cb[16];
    JSAMPROW cr[16];
    JSAMPARRAY planes[3];

    planes[0] = y;
    planes[1] = cb;
    planes[2] = cr;

    int width = cinfo->image_width;
    int height = cinfo->image_height;
    uint8_t *yRows = new uint8_t[16 * width];
    uint8_t *uRows = new uint8_t[16 * (width >> 1)];
    uint8_t *vRows = new uint8_t[16 * (width >> 1)];
    int processLines = DEINTERLEAVE_LINES_ONE_TIME;

    uint8_t *yuvOffset = yuv;

    int scanCount = 0;
    if (mDebug)
        ALOGI("%s: before scan line", __func__);

    // process 16 lines of Y and 16 lines of U/V each time.
    while (cinfo->next_scanline < cinfo->image_height) {
        if (cinfo->next_scanline + DEINTERLEAVE_LINES_ONE_TIME > cinfo->image_height)
            processLines = cinfo->image_height - cinfo->next_scanline;
        deinterleave(yuvOffset, yRows, uRows, vRows, cinfo->next_scanline, width, height,
                     processLines);

        for (int i = 0; i < processLines; i++) {
            // y row
            y[i] = yRows + i * width;

            // construct u row and v row
            // width is halved because of downsampling
            int offset = i * (width >> 1);
            cb[i] = uRows + offset;
            cr[i] = vRows + offset;
        }

        jpeg_write_raw_data(cinfo, planes, 16);
        scanCount++;
    }

    if (mDebug)
        ALOGI("%s: after scan line, count %d", __func__, scanCount);

    delete[] yRows;
    delete[] uRows;
    delete[] vRows;
}

void Yuv422IToJpegEncoder::deinterleave(uint8_t *yuv, uint8_t *yRows, uint8_t *uRows,
                                        uint8_t *vRows, int rowIndex, int width, int /*height*/,
                                        int processLines) {
    for (int row = 0; row < processLines; ++row) {
        uint8_t *yuvSeg = yuv + (rowIndex + row) * width * 2;
        for (int i = 0; i < (width >> 1); ++i) {
            int indexY = row * width + (i << 1);
            int indexU = row * (width >> 1) + i;
            yRows[indexY] = yuvSeg[0];
            yRows[indexY + 1] = yuvSeg[2];
            uRows[indexU] = yuvSeg[1];
            vRows[indexU] = yuvSeg[3];
            yuvSeg += 4;
        }
    }
}

void Yuv422IToJpegEncoder::configSamplingFactors(jpeg_compress_struct *cinfo) {
    // cb and cr are horizontally downsampled and vertically downsampled as
    // well.
    cinfo->comp_info[0].h_samp_factor = 2;
    cinfo->comp_info[0].v_samp_factor = 2;
    cinfo->comp_info[1].h_samp_factor = 1;
    cinfo->comp_info[1].v_samp_factor = 2;
    cinfo->comp_info[2].h_samp_factor = 1;
    cinfo->comp_info[2].v_samp_factor = 2;
}

// /////////////////////////////////////////////////////////////////////////////////////////////
Yuv422SpToJpegEncoder::Yuv422SpToJpegEncoder(int format) : YuvToJpegEncoder(format) {
    fNumPlanes = 1;
    color = 1;
}

void Yuv422SpToJpegEncoder::compress(jpeg_compress_struct *cinfo, uint8_t *yuv) {
    JSAMPROW y[16];
    JSAMPROW cb[16];
    JSAMPROW cr[16];
    JSAMPARRAY planes[3];

    planes[0] = y;
    planes[1] = cb;
    planes[2] = cr;

    int width = cinfo->image_width;
    int height = cinfo->image_height;
    uint8_t *yPlanar = yuv;
    uint8_t *vuPlanar = yuv + width * height;
    uint8_t *yRows = new uint8_t[16 * width];
    uint8_t *uRows = new uint8_t[16 * (width >> 1)];
    uint8_t *vRows = new uint8_t[16 * (width >> 1)];
    uint8_t *yuvOffset = yuv;
    int processLines = DEINTERLEAVE_LINES_ONE_TIME;

    int scanCount = 0;
    if (mDebug)
        ALOGI("%s: before scan line", __func__);

    // process 16 lines of Y and 16 lines of U/V each time.
    while (cinfo->next_scanline < cinfo->image_height) {
        if (cinfo->next_scanline + DEINTERLEAVE_LINES_ONE_TIME > cinfo->image_height)
            processLines = cinfo->image_height - cinfo->next_scanline;
        deinterleave(vuPlanar, uRows, vRows, cinfo->next_scanline, width, height, processLines);

        for (int i = 0; i < processLines; i++) {
            // y row
            y[i] = yPlanar + (cinfo->next_scanline + i) * width;

            // construct u row and v row
            // width is halved because of downsampling
            int offset = i * (width >> 1);
            cb[i] = uRows + offset;
            cr[i] = vRows + offset;
        }

        jpeg_write_raw_data(cinfo, planes, 16);
    }

    if (mDebug)
        ALOGI("%s: after scan line, count %d", __func__, scanCount);

    delete[] yRows;
    delete[] uRows;
    delete[] vRows;
}

void Yuv422SpToJpegEncoder::deinterleave(uint8_t *vuPlanar, uint8_t *uRows, uint8_t *vRows,
                                         int rowIndex, int width, int height, int processLines) {
    for (int row = 0; row < processLines; ++row) {
        uint8_t *uvSrc = vuPlanar + (rowIndex + row) * width;
        for (int i = 0; i < (width >> 1); ++i) {
            int indexU = row * (width >> 1) + i;
            uRows[indexU] = uvSrc[i * 2];
            vRows[indexU] = uvSrc[i * 2 + 1];
        }
    }
}

void Yuv422SpToJpegEncoder::configSamplingFactors(jpeg_compress_struct *cinfo) {
    // cb and cr are horizontally downsampled and vertically downsampled as
    // well.
    cinfo->comp_info[0].h_samp_factor = 2;
    cinfo->comp_info[0].v_samp_factor = 2;
    cinfo->comp_info[1].h_samp_factor = 1;
    cinfo->comp_info[1].v_samp_factor = 2;
    cinfo->comp_info[2].h_samp_factor = 1;
    cinfo->comp_info[2].v_samp_factor = 2;
}

int Yuv422SpToJpegEncoder::yuvResize(uint8_t *srcBuf, int srcWidth, int srcHeight, uint8_t *dstBuf,
                                     int dstWidth, int dstHeight) {
    return yuv422spResize(srcBuf, srcWidth, srcHeight, dstBuf, dstWidth, dstHeight, dstWidth);
}

void jpegBuilder_error_exit(j_common_ptr cinfo) {
    jpegBuilder_error_mgr *error = (jpegBuilder_error_mgr *)cinfo->err;

    (*error->output_message)(cinfo);

    /* Let the memory manager delete any temp files before we die */
    jpeg_destroy(cinfo);

    longjmp(error->fJmpBuf, -1);
}

static void jpegBuilder_init_destination(j_compress_ptr cinfo) {
    jpegBuilder_destination_mgr *dest = (jpegBuilder_destination_mgr *)cinfo->dest;

    dest->next_output_byte = dest->buf;
    dest->free_in_buffer = dest->bufsize;
    dest->jpegsize = 0;
}

static boolean jpegBuilder_empty_output_buffer(j_compress_ptr cinfo) {
    jpegBuilder_destination_mgr *dest = (jpegBuilder_destination_mgr *)cinfo->dest;

    dest->next_output_byte = dest->buf;
    dest->free_in_buffer = dest->bufsize;
    return TRUE; // ?
}

static void jpegBuilder_term_destination(j_compress_ptr cinfo) {
    jpegBuilder_destination_mgr *dest = (jpegBuilder_destination_mgr *)cinfo->dest;

    dest->jpegsize = dest->bufsize - dest->free_in_buffer;
}

jpegBuilder_destination_mgr::jpegBuilder_destination_mgr(uint8_t *input, int size) {
    this->init_destination = jpegBuilder_init_destination;
    this->empty_output_buffer = jpegBuilder_empty_output_buffer;
    this->term_destination = jpegBuilder_term_destination;

    this->buf = input;
    this->bufsize = size;

    jpegsize = 0;
}
