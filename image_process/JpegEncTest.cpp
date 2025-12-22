/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright 2025 NXP
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

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("usage: %s width height format\n", argv[0]);
        printf("usage: %s width height format inFile\n", argv[0]);
        printf("usage: %s width height format inFile outFile\n", argv[0]);
        printf("format: 420_sp: 259, 422_sp: 16; 422_i: 20\n");
        return 0;
    }

    uint32_t width = atoi(argv[1]);
    uint32_t height = atoi(argv[2]);
    uint32_t format = atoi(argv[3]);
    uint32_t inSize = 0;
    uint32_t outSize = 0;
    void *inBuf = NULL;
    void *outBuf = NULL;
    YuvToJpegEncoder *encoder = NULL;
    int ret = 0;
    int quality = 100;
    char *fileSrcName = NULL;
    char *fileDstName = NULL;
    FILE *pSrcFileStream = NULL;
    FILE *pDstFileStream = NULL;
    int jpegSize = 0;

    if ((format != HAL_PIXEL_FORMAT_YCbCr_420_SP) && (format != HAL_PIXEL_FORMAT_YCbCr_422_SP) &&
        (format != HAL_PIXEL_FORMAT_YCbCr_422_I)) {
        printf("Unsupported format %d, please run %s without arg to get supported formats\n",
               format, argv[0]);
        ret = -1;
        goto exit;
    }

    if (format == HAL_PIXEL_FORMAT_YCbCr_420_SP)
        inSize = width * height * 3 / 2;
    else
        inSize = width * height * 2;

    outSize = inSize;

    inBuf = calloc(1, inSize);
    if (inBuf == NULL) {
        printf("alloc inBuf size %u failed", inSize);
        ret = -1;
        goto exit;
    }

    outBuf = calloc(1, outSize);
    if (outBuf == NULL) {
        printf("alloc outBuf size %u failed", outSize);
        ret = -1;
        goto exit;
    }

    if (argc > 4) {
        fileSrcName = argv[4];
        pSrcFileStream = fopen(fileSrcName, "rb");
        if (pSrcFileStream == NULL) {
            printf("Failed to open file %s for reading\n", fileSrcName);
            ret = -1;
            goto exit;
        }

        ret = fread(inBuf, 1, inSize, pSrcFileStream);
        if (ret != (int)inSize) {
            printf("Failed to read file %s for %d bytes\n", fileSrcName, inSize);
            ret = -1;
            goto exit;
        }
    }

    encoder = YuvToJpegEncoder::create(format);
    if (encoder == NULL) {
        ALOGE("Failed to create jpeg encoder\n");
        ret = -1;
        goto exit;
    }

    ALOGI("before encode %d*%d YUYV\n", width, height);
    jpegSize = encoder->encode(inBuf, quality, outBuf, outSize, width, height, NULL, 0, true);
    ALOGI("after encode, jpeg size %d\n", jpegSize);

    if (argc > 5) {
        fileDstName = argv[5];
        pDstFileStream = fopen(fileDstName, "wb");
        if (pDstFileStream == NULL) {
            ALOGE("Failed to open file %s for writing\n", fileDstName);
            ret = -1;
            goto exit;
        }

        ret = fwrite(outBuf, 1, jpegSize, pDstFileStream);
        if (ret != jpegSize) {
            ALOGE("Failed to write file %s for %d bytes\n", fileDstName, jpegSize);
            ret = -1;
            goto exit;
        }
    }

exit:
    if (inBuf)
        free(inBuf);

    if (outBuf)
        free(outBuf);

    if (pSrcFileStream)
        fclose(pSrcFileStream);

    if (pDstFileStream)
        fclose(pDstFileStream);

    if (encoder)
        delete encoder;

    return ret;
}
