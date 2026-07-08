/*
 *  Copyright 2025-2026 NXP.
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

#ifndef LOG_TAG
#define LOG_TAG "ImageTest"
#endif

#include <graphics.h>
#include <hardware/gralloc.h>
#include <utils/Log.h>

#include "ImageProcessFactory.h"
#include "ImageUtils.h"
#include "linux/dma-buf-imx.h"

using namespace android;
using namespace fsl;

#define TEST_WIDTH 320
#define TEST_HEIGHT 240
#define TEST_LOOP 100

// g_ImgProc removed, use local instance instead

// Current, use physical addresss in opencl, so need pass un-cacheable usage for src/dst buffer.
// This function test INVALIDATE_CACHE (HW write, CPU read).
// Expected: cmpOkCount1 < 100, cmpOkCount2 == 100.
// dstBuf allocate as cacheable, after ConvertImage(), since un-cacheable usage for dst buffer,
// only memory is changed. So memcmp() may return non-zero. After INVALIDATE_CACHE, memcmp() return
// ok.
static int InvalideCacheTest(std::shared_ptr<ImageProcessDevice> device) {
    int ret = 0;
    ImxImageBuffer srcBuf;
    ImxImageBuffer dstBuf;

    memset(&srcBuf, 0, sizeof(srcBuf));
    memset(&dstBuf, 0, sizeof(dstBuf));

    // alloc src
    ret = AllocPhyBuffer(TEST_WIDTH, TEST_HEIGHT, HAL_PIXEL_FORMAT_YCbCr_422_I, srcBuf, false, true,
                         true);
    if (ret) {
        ALOGE("%s: AllocPhyBuffer for src failed, ret %d", __func__, ret);
        return -1;
    }
    ALOGI("%s: AllocPhyBuffer src ok, mSize %zu, mFormatSize %zu, mUsage 0x%lx",
          __func__, srcBuf.mSize, srcBuf.mFormatSize, srcBuf.mUsage);

    // alloc dst
    ret = AllocPhyBuffer(TEST_WIDTH, TEST_HEIGHT, HAL_PIXEL_FORMAT_YCbCr_422_I, dstBuf, true, true,
                         true);
    if (ret) {
        FreePhyBuffer(srcBuf.buffer);
        ALOGE("%s: AllocPhyBuffer for dst failed, ret %d", __func__, ret);
        return -1;
    }
    ALOGI("%s: AllocPhyBuffer dst ok, mSize %zu, mFormatSize %zu, mUsage 0x%lx",
          __func__, dstBuf.mSize, dstBuf.mFormatSize, dstBuf.mUsage);
    // pass un-cacheable usage to opencl
    dstBuf.mUsage = 0;

    int cmpOkCount1 = 0;
    int cmpOkCount2 = 0;

    for (int loop = 0; loop < TEST_LOOP; loop++) {
        memset(srcBuf.mVirtAddr, loop, srcBuf.mSize);

        ret = device->convert(dstBuf, srcBuf);
        if (ret) {
            ALOGE("%s: ConvertImage failed, ret %d", __func__, ret);
            break;
        }

        ret = memcmp(srcBuf.mVirtAddr, dstBuf.mVirtAddr, dstBuf.mSize);
        ALOGV("%s: compare after ConvertImage, ret %d", __func__, ret);
        if (ret == 0)
            cmpOkCount1++;

        SyncBuffer(dstBuf.mFd, INVALIDATE_CACHE);

        ret = memcmp(srcBuf.mVirtAddr, dstBuf.mVirtAddr, dstBuf.mSize);
        ALOGV("%s: compare after sync dstBuf, ret %d", __func__, ret);
        if (ret == 0)
            cmpOkCount2++;
    }

    ALOGI("%s: cmpOkCount1 %d, cmpOkCount2 %d", __func__, cmpOkCount1, cmpOkCount2);

    FreePhyBuffer(srcBuf.buffer);
    FreePhyBuffer(dstBuf.buffer);

    return 0;
}

// Current, use physical addresss in libimageprocess.so, so need pass un-cacheable usage for src/dst
// buffer. This function test FLUSH_CACHE (CPU write, HW read).
// Expected: cmpOkCount1 < 100, cmpOkCount2 == 100.
// srcBuf allocate as cacheable, after memset(), if not flush cache, HW(G3D)
// will probably not read expected value from memory.
static int FlushCacheTest(std::shared_ptr<ImageProcessDevice> device) {
    int ret = 0;
    ImxImageBuffer srcBuf;
    ImxImageBuffer dstBuf;

    memset(&srcBuf, 0, sizeof(srcBuf));
    memset(&dstBuf, 0, sizeof(dstBuf));

    // alloc src
    ret = AllocPhyBuffer(TEST_WIDTH, TEST_HEIGHT, HAL_PIXEL_FORMAT_YCbCr_422_I, srcBuf, true, true,
                         true);
    if (ret) {
        ALOGE("%s: AllocPhyBuffer for src failed, ret %d", __func__, ret);
        return -1;
    }
    ALOGI("%s: AllocPhyBuffer src ok, mSize %zu, mFormatSize %zu, mUsage 0x%lx",
          __func__, srcBuf.mSize, srcBuf.mFormatSize, srcBuf.mUsage);
    // pass un-cacheable usage to opencl
    srcBuf.mUsage = 0;

    // alloc dst
    ret = AllocPhyBuffer(TEST_WIDTH, TEST_HEIGHT, HAL_PIXEL_FORMAT_YCbCr_422_I, dstBuf, false, true,
                         true);
    if (ret) {
        FreePhyBuffer(srcBuf.buffer);
        ALOGE("%s: AllocPhyBuffer for dst failed, ret %d", __func__, ret);
        return -1;
    }
    ALOGI("%s: AllocPhyBuffer dst ok, mSize %zu, mFormatSize %zu, mUsage 0x%lx",
          __func__, dstBuf.mSize, dstBuf.mFormatSize, dstBuf.mUsage);

    int cmpOkCount1 = 0;
    int cmpOkCount2 = 0;

    for (int loop = 0; loop < TEST_LOOP; loop++) {
        memset(srcBuf.mVirtAddr, loop, srcBuf.mSize);

        ret = device->convert(dstBuf, srcBuf);
        if (ret) {
            ALOGE("%s: ConvertImage failed, ret %d", __func__, ret);
            break;
        }

        ret = memcmp(srcBuf.mVirtAddr, dstBuf.mVirtAddr, dstBuf.mSize);
        ALOGV("%s: ConvertImage, then compare, ret %d", __func__, ret);
        if (ret == 0)
            cmpOkCount1++;
    }

    for (int loop = 0; loop < TEST_LOOP; loop++) {
        memset(srcBuf.mVirtAddr, loop, srcBuf.mSize);
        SyncBuffer(srcBuf.mFd, FLUSH_CACHE);

        ret = device->convert(dstBuf, srcBuf);
        if (ret) {
            ALOGE("%s: ConvertImage failed, ret %d", __func__, ret);
            break;
        }

        ret = memcmp(srcBuf.mVirtAddr, dstBuf.mVirtAddr, dstBuf.mSize);
        ALOGV("%s: flush src buffer, ConvertImage, then compare, ret %d", __func__, ret);
        if (ret == 0)
            cmpOkCount2++;
    }

    ALOGI("%s: cmpOkCount1 %d, cmpOkCount2 %d", __func__, cmpOkCount1, cmpOkCount2);

    FreePhyBuffer(srcBuf.buffer);
    FreePhyBuffer(dstBuf.buffer);

    return 0;
}

int main(int argc, char **argv) {
    auto& factory = ImageProcessFactory::getInstance();
    auto device = factory.createDeviceByEngine(ENG_OCLCVT);
    if (device == nullptr) {
        ALOGE("%s: createDeviceByEngine failed", __func__);
        return -1;
    }
    InvalideCacheTest(device);
    FlushCacheTest(device);

    return 0;
}
