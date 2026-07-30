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
#define LOG_TAG "G2dDevice"

#include "G2dDevice.h"

#include <cutils/log.h>
#include <sys/stat.h>
#include <cutils/properties.h>
#include <dlfcn.h>
#include <mutex>
#include <hardware/gralloc.h>
#include <system/graphics.h>
#include <vndksupport/linker.h>

#include "gralloc_handle.h"

#define GPUHELPER "libgpuhelper.so"
#define G2DENGINE "libg2d"
#define DEWARP_COORD_FILE "/vendor/etc/configs/ox03c_absolute_32bpp_dewarp_file-1920x1280.bin"

namespace fsl {

G2dDevice::G2dDevice(ImxEngine engine)
      : mEngine(engine),
        mInitialized(false),
        mbVIVG2D(false),
        mG2dModule(NULL),
        mG2dHandle(NULL),
        mOpenEngine(NULL),
        mCloseEngine(NULL),
        mFinishEngine(NULL),
        mCopyEngine(NULL),
        mBlitEngine(NULL),
        mHelperHandle(NULL),
        mLockSurface(NULL),
        mUnlockSurface(NULL),
        mQueryFeature(NULL),
        mSetWarpCord(NULL),
        mEnableEngine(NULL),
        mDisableEngine(NULL),
        mAlloc(NULL),
        mFree(NULL),
        mGetCoordFromDct(NULL) {
    memset(&mDewarpCtx, 0, sizeof(mDewarpCtx));
    mInitialized = init();
}

G2dDevice::~G2dDevice() {
    // Free the dewarp coordinate buffer allocated by PrepareDewarpBinary
    if (mFree && mDewarpCtx.g2d_coord_buf) {
        mFree(mDewarpCtx.g2d_coord_buf);
        mDewarpCtx.g2d_coord_buf = NULL;
        mDewarpCtx.enable = false;
    }

    if (mG2dHandle != NULL) {
        closeEngine(mG2dHandle);
    }

    if (mG2dModule != NULL) {
        dlclose(mG2dModule);
    }

    if (mHelperHandle != NULL) {
        dlclose(mHelperHandle);
    }
}

const char* G2dDevice::getName() const {
    return (mEngine == ENG_DPU) ? "dpu" : "g2d";
}

bool G2dDevice::init() {
    char path[PATH_MAX] = {0};
    char g2dlibName[PATH_MAX] = {0};

    if (!getDefaultG2DLib(g2dlibName, PATH_MAX, &mbVIVG2D)) {
        ALOGI("G2dDevice: No g2d lib available");
        return false;
    }

    getModulePath(path, g2dlibName);
    mG2dModule = dlopen(path, RTLD_NOW);
    if (mG2dModule == NULL) {
        ALOGE("G2dDevice: dlopen %s failed: %s", path, dlerror());
        return false;
    }

    mOpenEngine = (hwc_func1)dlsym(mG2dModule, "g2d_open");
    mCloseEngine = (hwc_func1)dlsym(mG2dModule, "g2d_close");
    mFinishEngine = (hwc_func1)dlsym(mG2dModule, "g2d_finish");
    mCopyEngine = (hwc_func4)dlsym(mG2dModule, "g2d_copy");
    mBlitEngine = (hwc_func3)dlsym(mG2dModule, "g2d_blit");
    mQueryFeature = (hwc_query)dlsym(mG2dModule, "g2d_query_feature");
    mSetWarpCord = (hwc_func2)dlsym(mG2dModule, "g2d_set_warp_coordinates");
    mEnableEngine = (hwc_enable)dlsym(mG2dModule, "g2d_enable");
    mDisableEngine = (hwc_disable)dlsym(mG2dModule, "g2d_disable");
    mAlloc = (hwc_alloc)dlsym(mG2dModule, "g2d_alloc");
    mFree = (hwc_free)dlsym(mG2dModule, "g2d_free");
    mGetCoordFromDct =
            (hwc_get_coord_from_dct)dlsym(mG2dModule, "g2d_get_warp_coordinates_from_dct_file");

    int ret = mOpenEngine(&mG2dHandle);
    if (ret != 0) {
        mG2dHandle = NULL;
        ALOGE("G2dDevice: g2d_open failed");
        return false;
    }

    // Prepare the dewarp coordinate binary only for the (mbcam) layout.
    // a ~9.8MB physically contiguous buffer was used.
    char cameraLayout[PROPERTY_VALUE_MAX] = {0};
    property_get("ro.boot.camera.layout", cameraLayout, "");
    bool layoutMBCam = (strcmp(cameraLayout, "mbcam") == 0);

    // Check dewarp support
    if (layoutMBCam && mG2dHandle && mQueryFeature) {
        int can_warp = 0;
        mQueryFeature(mG2dHandle, G2D_WARP_DEWARP, &can_warp);
        if (can_warp) {
            PrepareDewarpBinary();
        }
    }

    // Load GPU helper for VIV G2D lockSurface
    if (mEngine == ENG_G2D && mbVIVG2D) {
        mHelperHandle = android_load_sphal_library(GPUHELPER, RTLD_LOCAL | RTLD_NOW);
        if (mHelperHandle != NULL) {
            mLockSurface = (hwc_func1)dlsym(mHelperHandle, "hwc_lockSurface");
            mUnlockSurface = (hwc_func1)dlsym(mHelperHandle, "hwc_unlockSurface");
        } else {
            ALOGE("G2dDevice: fail to open libgpuhelper.so");
        }
    }

    ALOGI("G2dDevice(%s): initialized, mbVIVG2D=%d", getName(), mbVIVG2D);
    return true;
}

bool G2dDevice::getDefaultG2DLib(char* libName, int size, bool* pVIVG2D) {
    // Compute the result once and cache all outputs using std::call_once.
    static char cachedLibName[PATH_MAX] = {0};
    static bool cachedVIVG2D = false;
    static bool cachedResult = false;
    static std::once_flag sOnceFlag;
    std::call_once(sOnceFlag, []() {
        char value[PROPERTY_VALUE_MAX] = {0};
        property_get("vendor.imx.default-g2d", value, "");
        if (strcmp(value, "") != 0) {
            strncpy(cachedLibName, G2DENGINE, strlen(G2DENGINE));
            strcat(cachedLibName, "-");
            strcat(cachedLibName, value);
            strcat(cachedLibName, ".so");
            cachedVIVG2D = (strcmp(value, "viv") == 0);
            cachedResult = true;
        }
    });

    if (!cachedResult)
        return false;

    if ((libName == NULL) || (size < (int)strlen(cachedLibName) + 1))
        return false;

    strncpy(libName, cachedLibName, size - 1);
    libName[size - 1] = '\0';

    if (pVIVG2D != NULL)
        *pVIVG2D = cachedVIVG2D;

    return true;
}

int G2dDevice::openEngine(void** handle) {
    if (mOpenEngine == NULL)
        return -EINVAL;
    return (*mOpenEngine)((void*)handle);
}

int G2dDevice::closeEngine(void* handle) {
    if (mCloseEngine == NULL)
        return -EINVAL;
    return (*mCloseEngine)(handle);
}

void* G2dDevice::getHandle() {
    return mG2dHandle;
}

void G2dDevice::LockG2dAddr(ImxImageBuffer& imxBuf) {
    if (imxBuf.buffer == NULL) {
        ALOGE("%s: buffer handle is invalid", __func__);
        return;
    }
    auto handle = imxBuf.buffer;
    if (mLockSurface)
        (*mLockSurface)(const_cast<native_handle_t*>(handle));
    imxBuf.mPhyAddr = gralloc_handle_phys(handle);
}

void G2dDevice::UnLockG2dAddr(ImxImageBuffer& imxBuf) {
    if (mUnlockSurface)
        (*mUnlockSurface)((void*)imxBuf.buffer);
}

int G2dDevice::convertPixelFormatToG2DFormat(int format) {
    int nFormat = 0;
    switch (format) {
        case HAL_PIXEL_FORMAT_YCbCr_420_888:
        case HAL_PIXEL_FORMAT_YCbCr_420_SP:
            nFormat = G2D_NV12;
            break;
        case HAL_PIXEL_FORMAT_YCbCr_422_I:
            nFormat = G2D_YUYV;
            break;
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
            nFormat = G2D_NV21;
            break;
        case HAL_PIXEL_FORMAT_RGB_888:
            nFormat = G2D_RGB888;
            break;
        default:
            ALOGE("%s:%d, Error: format:0x%x not supported!", __func__, __LINE__, format);
            break;
    }
    return nFormat;
}

static bool isFormatSupportedByG2D(uint32_t srcFormat, uint32_t dstFormat) {

    if (srcFormat == HAL_PIXEL_FORMAT_RAW16)
        return false;

    if (dstFormat == HAL_PIXEL_FORMAT_YCbCr_420_888)
        return true;
    if (dstFormat == HAL_PIXEL_FORMAT_YCrCb_420_SP && srcFormat == HAL_PIXEL_FORMAT_YCbCr_422_I)
        return true;
    if (srcFormat == HAL_PIXEL_FORMAT_YCbCr_422_I && dstFormat == HAL_PIXEL_FORMAT_YCbCr_422_I)
        return true;
    if (srcFormat == HAL_PIXEL_FORMAT_RGB_888 && dstFormat == HAL_PIXEL_FORMAT_RGB_888)
        return true;

    return false;
}

float G2dDevice::g2d_sniff(uint32_t srcFormat, uint32_t dstFormat, uint32_t srcWidth,
                            uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight) {
    (void)srcWidth;
    (void)srcHeight;
    (void)dstWidth;
    (void)dstHeight;

    char libName[PATH_MAX] = {0};
    if (!getDefaultG2DLib(libName, PATH_MAX, NULL))
        return CONFIDENCE_NONE;

    if (!isFormatSupportedByG2D(srcFormat, dstFormat))
        return CONFIDENCE_NONE;

    return CONFIDENCE_G2D;
}

float G2dDevice::dpu_sniff(uint32_t srcFormat, uint32_t dstFormat, uint32_t srcWidth,
                            uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight) {
    (void)srcWidth;
    (void)srcHeight;
    (void)dstWidth;
    (void)dstHeight;

    char libName[PATH_MAX] = {0};
    if (!getDefaultG2DLib(libName, PATH_MAX, NULL))
        return CONFIDENCE_NONE;

    if (!isFormatSupportedByG2D(srcFormat, dstFormat))
        return CONFIDENCE_NONE;

    return CONFIDENCE_DPU;
}

int G2dDevice::convert(ImxImageBuffer& dstBuf, ImxImageBuffer& srcBuf) {
    if (!mInitialized || mG2dHandle == NULL)
        return -EINVAL;

    srcBuf.mFormat = unifyNV12Format(srcBuf.mFormat);
    dstBuf.mFormat = unifyNV12Format(dstBuf.mFormat);

    int ret = 0;

    if (mBlitEngine && (mEngine == ENG_G2D) && mbVIVG2D) {
        LockG2dAddr(srcBuf);
        LockG2dAddr(dstBuf);
    }

    bool useCopy = (srcBuf.mFormat == dstBuf.mFormat) && (srcBuf.mWidth == dstBuf.mWidth) &&
                   (srcBuf.mHeight == dstBuf.mHeight) && (srcBuf.mZoomRatio <= 1.0) &&
                   (srcBuf.mStride == dstBuf.mStride) &&
                   (srcBuf.mHeightSpan == dstBuf.mHeightSpan) && (srcBuf.mDewarp == false);

    if (mDebug) {
        ALOGI("G2dDevice::%s: engine=%s"
              " src(fmt=0x%x, %ux%u stride=%u hspan=%u zoom=%.2f dewarp=%d)"
              " dst(fmt=0x%x, %ux%u stride=%u hspan=%u) -> %s",
              __func__, getName(), srcBuf.mFormat, srcBuf.mWidth, srcBuf.mHeight, srcBuf.mStride,
              srcBuf.mHeightSpan, srcBuf.mZoomRatio, (int)srcBuf.mDewarp, dstBuf.mFormat,
              dstBuf.mWidth, dstBuf.mHeight, dstBuf.mStride, dstBuf.mHeightSpan,
              useCopy ? "copy" : "blit");
    }

    if (useCopy) {
        ret = ConvertByG2DCopy(dstBuf, srcBuf);
    } else {
        ret = ConvertByG2DBlit(dstBuf, srcBuf);
    }

    if (mBlitEngine && (mEngine == ENG_G2D) && mbVIVG2D) {
        UnLockG2dAddr(srcBuf);
        UnLockG2dAddr(dstBuf);
    }

    return ret;
}

int G2dDevice::ConvertByG2DCopy(ImxImageBuffer& dstBuf, ImxImageBuffer& srcBuf) {
    if (mCopyEngine == NULL)
        return -EINVAL;

    int dstFormat = convertPixelFormatToV4L2Format(dstBuf.mFormat);
    int srcFormat = convertPixelFormatToV4L2Format(srcBuf.mFormat);
    if ((dstBuf.mFormat != srcBuf.mFormat) && (dstFormat != srcFormat))
        return -EINVAL;

    if ((srcBuf.mPhyAddr == 0) || (srcBuf.mVirtAddr == NULL))
        GetBufferInfoFromHandle(srcBuf.buffer, srcBuf, true, true);
    if ((dstBuf.mPhyAddr == 0) || (dstBuf.mVirtAddr == NULL))
        GetBufferInfoFromHandle(dstBuf.buffer, dstBuf, true, true);

    void* g2dHandle = getHandle();
    int size = (srcBuf.mSize > dstBuf.mSize) ? dstBuf.mSize : srcBuf.mSize;

    struct g2d_buf s_buf, d_buf;
    s_buf.buf_paddr = srcBuf.mPhyAddr;
    s_buf.buf_vaddr = srcBuf.mVirtAddr;
    d_buf.buf_paddr = dstBuf.mPhyAddr;
    d_buf.buf_vaddr = dstBuf.mVirtAddr;

    if (mDebug) {
        ALOGI("G2dDevice::%s: src phyAddr=0x%lx virtAddr=%p dst phyAddr=0x%lx virtAddr=%p size=%d",
              __func__, (unsigned long)srcBuf.mPhyAddr, srcBuf.mVirtAddr,
              (unsigned long)dstBuf.mPhyAddr, dstBuf.mVirtAddr, size);
    }

    Mutex::Autolock _l(mG2dLock);
    int ret = mCopyEngine(g2dHandle, (void*)&d_buf, (void*)&s_buf, (void*)(intptr_t)size);
    if (ret == 0) {
        mFinishEngine(g2dHandle);
    }

    if (mDebug) {
        ALOGI("G2dDevice::%s: g2d_copy ret=%d", __func__, ret);
    }

    return ret;
}

int G2dDevice::ConvertByG2DBlit(ImxImageBuffer& dstBuf, ImxImageBuffer& srcBuf) {
    if (mBlitEngine == NULL)
        return -EINVAL;

    // Check supported format combinations
    if (!((dstBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_888) ||
          ((dstBuf.mFormat == HAL_PIXEL_FORMAT_YCrCb_420_SP) &&
           (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_422_I)) ||
          ((srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_422_I) &&
           (dstBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_422_I)) ||
          ((srcBuf.mFormat == HAL_PIXEL_FORMAT_RGB_888) &&
           (dstBuf.mFormat == HAL_PIXEL_FORMAT_RGB_888)))) {
        ALOGE("G2dDevice::%s: blit format not supported, src=0x%x, dst_format=0x%x", 
            __func__, srcBuf.mFormat, dstBuf.mFormat);
        return -EINVAL;
    }

    int ret;
    void* g2dHandle = getHandle();
    struct g2d_surface s_surface, d_surface;

    if (srcBuf.mPhyAddr == 0)
        GetBufferInfoFromHandle(srcBuf.buffer, srcBuf, true, false);
    if (dstBuf.mPhyAddr == 0)
        GetBufferInfoFromHandle(dstBuf.buffer, dstBuf, true, false);

    // zoom feature
    uint32_t crop_left = 0;
    uint32_t crop_top = 0;
    uint32_t crop_width = srcBuf.mWidth;
    uint32_t crop_height = srcBuf.mHeight;

    if (srcBuf.mZoomRatio > 1.0) {
        crop_width = srcBuf.mWidth / srcBuf.mZoomRatio;
        crop_height = srcBuf.mHeight / srcBuf.mZoomRatio;
        crop_left = (srcBuf.mWidth - crop_width) / 2;
        crop_top = (srcBuf.mHeight - crop_height) / 2;
    }

    s_surface.format = (g2d_format)convertPixelFormatToG2DFormat(srcBuf.mFormat);
    s_surface.planes[0] = (long)srcBuf.mPhyAddr;
    s_surface.left = crop_left;
    s_surface.top = crop_top;
    s_surface.right = crop_left + crop_width;
    s_surface.bottom = crop_top + crop_height;
    s_surface.stride = srcBuf.mStride;
    s_surface.width = srcBuf.mWidth;
    s_surface.height = srcBuf.mHeightSpan;
    s_surface.rot = G2D_ROTATION_0;

    if (mDebug) {
        ALOGD("G2dDevice::%s: src surface fmt=%d phyAddr=0x%lx"
              " crop(%u,%u)-(%u,%u) stride=%u width=%u height=%u dewarp=%d",
              __func__, s_surface.format, (unsigned long)srcBuf.mPhyAddr,
              crop_left, crop_top, crop_left + crop_width, crop_top + crop_height,
              srcBuf.mStride, srcBuf.mWidth, srcBuf.mHeightSpan, (int)srcBuf.mDewarp);
    }

    if (srcBuf.mDewarp && (!mDewarpCtx.enable)) {
        ALOGE("%s: want to dewarp but mDewarpCtx.enable false", __func__);
        return -EINVAL;
    }

    if ((srcBuf.mFormat == dstBuf.mFormat) ||
        (srcBuf.mZoomRatio <= 1.0 &&
         (srcBuf.mWidth == dstBuf.mWidth && srcBuf.mHeight == dstBuf.mHeight))) {
        d_surface.format = (g2d_format)convertPixelFormatToG2DFormat(dstBuf.mFormat);
        d_surface.planes[0] = (long)dstBuf.mPhyAddr;
        d_surface.planes[1] = (long)dstBuf.mPhyAddr + dstBuf.mStride * dstBuf.mHeight;
        d_surface.left = 0;
        d_surface.top = 0;
        d_surface.right = dstBuf.mWidth;
        d_surface.bottom = dstBuf.mHeight;
        d_surface.stride = dstBuf.mStride;
        d_surface.width = dstBuf.mWidth;
        d_surface.height = dstBuf.mHeight;
        d_surface.rot = G2D_ROTATION_0;

        Mutex::Autolock _l(mG2dLock);
        if (srcBuf.mDewarp && mDewarpCtx.enable) {
            mEnableEngine(mG2dHandle, G2D_WARPING);
            mSetWarpCord(mG2dHandle, &mDewarpCtx.coord);
        }

        if (mDebug) {
            ALOGD("G2dDevice::%s: single-blit dst surface fmt=%d phyAddr=0x%lx"
                  " rect(0,0)-(%u,%u) stride=%u width=%u height=%u",
                  __func__, d_surface.format, (unsigned long)dstBuf.mPhyAddr,
                  dstBuf.mWidth, dstBuf.mHeight, dstBuf.mStride,
                  dstBuf.mWidth, dstBuf.mHeight);
        }

        ret = mBlitEngine(g2dHandle, (void*)&s_surface, (void*)&d_surface);
        if (mDebug) {
            ALOGD("G2dDevice::%s: single-blit ret=%d", __func__, ret);
        }
        if (ret)
            return ret;

        if (srcBuf.mDewarp && mDewarpCtx.enable) {
            mDisableEngine(mG2dHandle, G2D_WARPING);
        }

        mFinishEngine(g2dHandle);
    } else {
        ImxImageBuffer resizeBuf;
        memset(&resizeBuf, 0, sizeof(resizeBuf));
        struct g2d_surface tmp_surface;

        ret = AllocPhyBuffer(dstBuf.mStride, dstBuf.mHeight, srcBuf.mFormat, resizeBuf, true, true,
                             false);
        if (ret) {
            ALOGE("%s:%d AllocPhyBuffer failed", __func__, __LINE__);
            return BAD_VALUE;
        }

        tmp_surface.format = (g2d_format)convertPixelFormatToG2DFormat(srcBuf.mFormat);
        tmp_surface.planes[0] = (long)resizeBuf.mPhyAddr;
        tmp_surface.planes[1] = (long)resizeBuf.mPhyAddr + dstBuf.mStride * dstBuf.mHeight;
        tmp_surface.left = 0;
        tmp_surface.top = 0;
        tmp_surface.right = dstBuf.mWidth;
        tmp_surface.bottom = dstBuf.mHeight;
        tmp_surface.stride = dstBuf.mStride;
        tmp_surface.width = dstBuf.mWidth;
        tmp_surface.height = dstBuf.mHeight;
        tmp_surface.rot = G2D_ROTATION_0;

        if (mDebug) {
            ALOGD("G2dDevice::%s: two-step blit step1 resize src->tmp"
                  " tmp phyAddr=0x%lx fmt=%d %ux%u stride=%u",
                  __func__, (unsigned long)resizeBuf.mPhyAddr, tmp_surface.format,
                  dstBuf.mWidth, dstBuf.mHeight, dstBuf.mStride);
        }

        Mutex::Autolock _l(mG2dLock);
        ret = mBlitEngine(g2dHandle, (void*)&s_surface, (void*)&tmp_surface);
        if (mDebug) {
            ALOGD("G2dDevice::%s: two-step blit step1 ret=%d", __func__, ret);
        }
        if (ret) {
            FreePhyBuffer(resizeBuf.buffer);
            return ret;
        }
        mFinishEngine(g2dHandle);

        d_surface.format = (g2d_format)convertPixelFormatToG2DFormat(dstBuf.mFormat);
        d_surface.planes[0] = (long)dstBuf.mPhyAddr;
        d_surface.planes[1] = (long)dstBuf.mPhyAddr + dstBuf.mStride * dstBuf.mHeight;
        d_surface.left = 0;
        d_surface.top = 0;
        d_surface.right = dstBuf.mWidth;
        d_surface.bottom = dstBuf.mHeight;
        d_surface.stride = dstBuf.mStride;
        d_surface.width = dstBuf.mWidth;
        d_surface.height = dstBuf.mHeight;
        d_surface.rot = G2D_ROTATION_0;

        if (mDebug) {
            ALOGD("G2dDevice::%s: two-step blit step2 CSC tmp->dst"
                  " dst phyAddr=0x%lx fmt=%d %ux%u stride=%u",
                  __func__, (unsigned long)dstBuf.mPhyAddr, d_surface.format,
                  dstBuf.mWidth, dstBuf.mHeight, dstBuf.mStride);
        }

        ret = mBlitEngine(g2dHandle, (void*)&tmp_surface, (void*)&d_surface);
        if (mDebug) {
            ALOGD("G2dDevice::%s: two-step blit step2 ret=%d", __func__, ret);
        }
        if (ret) {
            FreePhyBuffer(resizeBuf.buffer);
            return ret;
        }
        mFinishEngine(g2dHandle);
        FreePhyBuffer(resizeBuf.buffer);
    }

    return ret;
}

int G2dDevice::PrepareDewarpBinary() {
    int ret = 0;
    struct stat statbuf;

    ret = stat(DEWARP_COORD_FILE, &statbuf);
    if (ret) {
        ALOGI("%s: no dewarp file %s", __func__, DEWARP_COORD_FILE);
        return -1;
    }

    ALOGI("%s: %s size is %ld", __func__, DEWARP_COORD_FILE, statbuf.st_size);
    mDewarpCtx.g2d_coord_buf = mAlloc(statbuf.st_size, 0);
    if (mDewarpCtx.g2d_coord_buf == NULL) {
        ALOGI("%s: mAlloc size %ld failed", __func__, statbuf.st_size);
        return -1;
    }

    ret = mGetCoordFromDct(mG2dHandle, DEWARP_COORD_FILE, mDewarpCtx.g2d_coord_buf,
                           &mDewarpCtx.coord);
    if (ret) {
        ALOGI("%s: g2d_get_warp_coordinates_from_dct_file failed, ret %d", __func__, ret);
        return ret;
    }

    ALOGI("%s: coord para: addr 0x%lx, format %d, bpp %d, width %d, height %d", __func__,
          mDewarpCtx.coord.addr, mDewarpCtx.coord.format, mDewarpCtx.coord.bpp,
          mDewarpCtx.coord.width, mDewarpCtx.coord.height);

    mDewarpCtx.enable = true;
    return 0;
}

} // namespace fsl
