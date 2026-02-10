/*
 * Copyright 2023-2026 NXP.
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
#define LOG_TAG "ImageProcess"

#include "ImageProcess.h"

#include <cutils/log.h>
#include <cutils/properties.h>
#include <dlfcn.h>
#include <g2d.h>
#include <hardware/gralloc.h>
#include <libyuv.h>
#include <libyuv/convert.h>
#include <linux/ipu.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <system/graphics.h>
#include <unistd.h>
#include <vndksupport/linker.h>

#include "gralloc_handle.h"

extern "C" {
#include <linux/pxp_device.h>
}

#include <pthread.h>

#if defined(__LP64__)
#define LIB_PATH1 "/system/lib64"
#define LIB_PATH2 "/vendor/lib64"
#else
#define LIB_PATH1 "/system/lib"
#define LIB_PATH2 "/vendor/lib"
#endif

#define GPUHELPER "libgpuhelper.so"
#define CLENGINE "libg2d-opencl.so"
#define G2DENGINE "libg2d"
#define IMX_OCL_CONVERTER "lib_imx_opencl_converter.so"

#define DEWARP_COORD_FILE "/vendor/etc/configs/ox03c_absolute_32bpp_dewarp_file-1920x1280.bin"

namespace fsl {

ImageProcess *ImageProcess::sInstance(0);
Mutex ImageProcess::sLock(Mutex::PRIVATE);

static bool IsCscSupportByCPU(int srcFormat, int dstFormat) {
    // yuyv -> nv12
    if (((dstFormat == HAL_PIXEL_FORMAT_YCbCr_420_888) ||
         (dstFormat == HAL_PIXEL_FORMAT_YCbCr_420_SP)) &&
        (srcFormat == HAL_PIXEL_FORMAT_YCbCr_422_I))
        return true;

    // nv12 -> nv21
    if ((srcFormat == HAL_PIXEL_FORMAT_YCbCr_420_SP) &&
        (dstFormat == HAL_PIXEL_FORMAT_YCrCb_420_SP))
        return true;

    // nv12 -> yv12
    if (((srcFormat == HAL_PIXEL_FORMAT_YCbCr_420_SP) ||
        (srcFormat == HAL_PIXEL_FORMAT_YCbCr_420_888)) && (dstFormat == HAL_PIXEL_FORMAT_YV12))
        return true;

    return false;
}

static bool IsCscSupportByG3D(int srcFomat, int dstFormat) {
    // yuyv -> nv12, nv16 -> nv12
    if (((dstFormat == HAL_PIXEL_FORMAT_YCbCr_420_888) ||
         (dstFormat == HAL_PIXEL_FORMAT_YCbCr_420_SP)) &&
        ((srcFomat == HAL_PIXEL_FORMAT_YCbCr_422_I) ||
         (srcFomat == HAL_PIXEL_FORMAT_YCbCr_422_SP)))
        return true;

    return false;
}

bool ImageProcess::getDefaultG2DLib(char *libName, int size) {
    char value[PROPERTY_VALUE_MAX];

    if ((libName == NULL) || (size < (int)strlen(G2DENGINE) + (int)strlen(".so")))
        return false;

    memset(libName, 0, size);
    property_get("vendor.imx.default-g2d", value, "");
    if (strcmp(value, "") == 0) {
        ALOGI("No g2d lib available to be used!");
        return false;
    } else {
        strncpy(libName, G2DENGINE, strlen(G2DENGINE));
        strcat(libName, "-");
        strcat(libName, value);
        strcat(libName, ".so");
    }

    if (strcmp(value, "viv") == 0)
        mbVIVG2D = true;

    ALOGI("Default g2d lib: %s, mbVIVG2D %d", libName, mbVIVG2D);
    return true;
}

void ImageProcess::FreeOclHandle(void *handle) {
    OCL_HANDLE hOcl = (OCL_HANDLE)handle;
    ALOGI("%s: hOcl %p", __func__, hOcl);
    ImageProcess::getInstance()->m_ocl_close(hOcl);
    return;
}

ImageProcess *ImageProcess::getInstance() {
    Mutex::Autolock _l(sLock);
    if (sInstance != NULL) {
        return sInstance;
    }

    sInstance = new ImageProcess();
    return sInstance;
}

ImageProcess::ImageProcess()
      : mIpuFd(-1), mPxpFd(-1), mChannel(-1), mG2dModule(NULL), mCLModule(NULL), mbVIVG2D(false) {
    /*
     * imx6dl support ENG_IPU device and ENG_PXP device.
     * imx6q and imx6qp support ENG_IPU device.
     * imx6sx and imx6sl support ENG_PXP device.
     * ENG_IPU can't handle NV21 format, so open ENG_PXP on some platform to handle it.
     */
    mIpuFd = open("/dev/mxc_ipu", O_RDWR, 0);
    mPxpFd = open("/dev/pxp_device", O_RDWR, 0);

    // When open pxp device, need allocate a channel at the same time.
    int32_t ret = -1;
    if (mPxpFd > 0) {
        ret = ioctl(mPxpFd, PXP_IOC_GET_CHAN, &mChannel);
        if (ret < 0) {
            ALOGE("%s:%d, PXP_IOC_GET_CHAN failed %d", __func__, __LINE__, ret);
            close(mPxpFd);
            mPxpFd = -1;
        }
    }
    ALOGI("mChannel %d", mChannel);

    char path[PATH_MAX] = {0};
    char g2dlibName[PATH_MAX] = {0};
    if (getDefaultG2DLib(g2dlibName, PATH_MAX)) {
        getModule(path, g2dlibName);
        mG2dModule = dlopen(path, RTLD_NOW);
    }

    if (mG2dModule == NULL) {
        mOpenEngine = NULL;
        mCloseEngine = NULL;
        mFinishEngine = NULL;
        mCopyEngine = NULL;
        mBlitEngine = NULL;
        mG2dHandle = NULL;
    } else {
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

        ret = mOpenEngine(&mG2dHandle);
        if (ret != 0) {
            mG2dHandle = NULL;
        }
    }

    memset(&mDewarpCtx, 0, sizeof(mDewarpCtx));
    if (mG2dHandle) {
        int can_warp = 0;
        mQueryFeature(mG2dHandle, G2D_WARP_DEWARP, &can_warp);
        if (can_warp) {
            PrepareDewarpBinary();
        }
    }

    mHelperHandle = android_load_sphal_library(GPUHELPER, RTLD_LOCAL | RTLD_NOW);
    if (mHelperHandle == NULL) {
        ALOGE("fail to open libgpuhelper.so");
        mLockSurface = NULL;
        mUnlockSurface = NULL;
    } else {
        mLockSurface = (hwc_func1)dlsym(mHelperHandle, "hwc_lockSurface");
        mUnlockSurface = (hwc_func1)dlsym(mHelperHandle, "hwc_unlockSurface");
    }

    memset(path, 0, sizeof(path));
    getModule(path, CLENGINE);
    mCLModule = dlopen(path, RTLD_NOW);
    if (mCLModule == NULL) {
        mCLOpen = NULL;
        mCLClose = NULL;
        mCLFlush = NULL;
        mCLFinish = NULL;
        mCLBlit = NULL;
        mCLCopy = NULL;
        mCLHandle = NULL;
    } else {
        mCLOpen = (hwc_func1)dlsym(mCLModule, "cl_g2d_open");
        mCLClose = (hwc_func1)dlsym(mCLModule, "cl_g2d_close");
        mCLFlush = (hwc_func1)dlsym(mCLModule, "cl_g2d_flush");
        mCLFinish = (hwc_func1)dlsym(mCLModule, "cl_g2d_finish");
        mCLBlit = (hwc_func3)dlsym(mCLModule, "cl_g2d_blit");
        mCLCopy = (hwc_func4)dlsym(mCLModule, "cl_g2d_copy");
        ret = (*mCLOpen)((void *)&mCLHandle);
        if (ret != 0) {
            mCLHandle = NULL;
        }
    }

    if (mCLHandle != NULL) {
        ALOGW("opencl g2d device is used!\n");
    }

    property_get("ro.boot.soc_type", mSocType, "");
    ALOGI("%s: mSocType :%s \n", __FUNCTION__, mSocType);

    char value[PROPERTY_VALUE_MAX];
    property_get("ro.boot.camera.layout", value, "");
    mLayoutMBCam = (strcmp(value, "mbcam") == 0) ? true : false;
    ALOGI("%s: mLayoutMBCam %d", __func__, mLayoutMBCam);

    mOclBufferType = OCL_MEM_TYPE_GPU;
    memset(path, 0, sizeof(path));
    getModule(path, IMX_OCL_CONVERTER);
    mImxOclCvtModule = dlopen(path, RTLD_NOW);
    if (mImxOclCvtModule == NULL) {
        ALOGW("%s:, dlopen %s failed", __func__, path);
        mHOcl = NULL;
        m_ocl_open = NULL;
        m_ocl_setParam = NULL;
        m_ocl_getParam = NULL;
        m_ocl_convert = NULL;
        m_ocl_close = NULL;
    } else {
        m_ocl_open = (ocl_open)dlsym(mImxOclCvtModule, "OCL_Open");
        m_ocl_setParam = (ocl_setParam)dlsym(mImxOclCvtModule, "OCL_SetParam");
        m_ocl_getParam = (ocl_getParam)dlsym(mImxOclCvtModule, "OCL_GetParam");
        m_ocl_convert = (ocl_convert)dlsym(mImxOclCvtModule, "OCL_Convert");
        m_ocl_close = (ocl_close)dlsym(mImxOclCvtModule, "OCL_Close");

        // All cases except mbcam, use the ocl handle created in ctor.
        // Since m_ocl_open takes around 200ms, so save time in ConvertImageByOclCvt().
        if (mLayoutMBCam == false) {
            ret = (*m_ocl_open)(OCL_OPEN_FLAG_PROFILE, &mHOcl);
            if (ret != 0) {
                mHOcl = NULL;
                ALOGW("%s: m_ocl_open failed, ret %d", __func__, ret);
            }
            ALOGI("%s: mHOcl %p", __func__, mHOcl);
        } else {
            mHOcl = NULL;
        }

        if (!strncmp(mSocType, "imx9", 4))
            mOclBufferType = OCL_MEM_TYPE_DEVICE;

        pthread_key_create(&m_ocl_key, FreeOclHandle);
    }

    memset(&mWarpBuffer, 0, sizeof(mWarpBuffer));
    memset(&m_warp_param, 0, sizeof(m_warp_param));


    if (mImxOclCvtModule && mLayoutMBCam) {
        ret = parse_warp_file(DEWARP_COORD_FILE, &m_warp_param);
        if (ret) {
            ALOGW("%s: parse_warp_file failed, ret %d", __func__, ret);
        } else {
            ALOGI("%s: parse_warp_file ok", __func__);
        }
    }
}

ImageProcess::~ImageProcess() {
    if (mHOcl) {
        m_ocl_close(mHOcl);
        mHOcl = NULL;
    }

    if (mImxOclCvtModule)
        pthread_key_delete(m_ocl_key);

    if (mImxOclCvtModule)
        dlclose(mImxOclCvtModule);

    if (mIpuFd > 0) {
        close(mIpuFd);
        mIpuFd = -1;
    }

    if (mPxpFd > 0) {
        close(mPxpFd);
        mPxpFd = -1;
    }

    if (mCLHandle != NULL) {
        (*mCLClose)(mCLHandle);
    }

    if (mCLModule != NULL) {
        dlclose(mCLModule);
    }

    if (mG2dHandle != NULL) {
        closeEngine(mG2dHandle);
    }

    if (mG2dModule != NULL) {
        dlclose(mG2dModule);
    }

    if (m_warp_param.buf.planes[0].vaddr) {
        if (mWarpBuffer.buffer)
            FreePhyBuffer(mWarpBuffer.buffer);
        else
            free((void *)m_warp_param.buf.planes[0].vaddr);
    }

    sInstance = NULL;
}

void *ImageProcess::getHandle() {
    return mG2dHandle;
}

int ImageProcess::openEngine(void **handle) {
    if (mOpenEngine == NULL) {
        return -EINVAL;
    }

    return (*mOpenEngine)((void *)handle);
}

int ImageProcess::closeEngine(void *handle) {
    if (mCloseEngine == NULL) {
        return -EINVAL;
    }

    return (*mCloseEngine)(handle);
}

void ImageProcess::getModule(char *path, const char *name) {
    snprintf(path, PATH_MAX, "%s/%s", LIB_PATH1, name);
    if (access(path, R_OK) == 0)
        return;
    snprintf(path, PATH_MAX, "%s/%s", LIB_PATH2, name);
    if (access(path, R_OK) == 0)
        return;
    return;
}

int ImageProcess::ConvertImage(ImxImageBuffer &dstBuf, ImxImageBuffer &srcBuf, ImxEngine engine,
                               bool debug) {
    int ret = 0;

    if (engine == ENG_BYPASS)
        return 0;

    if (!((engine == ENG_NOTCARE) || (engine >= ENG_MIN && engine < ENG_NUM))) {
        ALOGE("%s: invalid engine %d", __func__, engine);
        return -EINVAL;
    }

    mDebug = debug;
    if (mDebug)
        ALOGI("%s: src: virt %p, phy 0x%lx, size %d, res %ux%u, format 0x%x, "
              "dst: virt %p, phy 0x%lx, size %d, res %ux%u, format 0x%x, engine %d, ZoomRatio %f, dewarp %d",
              __func__, srcBuf.mVirtAddr, srcBuf.mPhyAddr, (int)srcBuf.mSize, srcBuf.mWidth,
              srcBuf.mHeight, srcBuf.mFormat, dstBuf.mVirtAddr, dstBuf.mPhyAddr, (int)dstBuf.mSize,
              dstBuf.mWidth, dstBuf.mHeight, dstBuf.mFormat, engine, srcBuf.mZoomRatio,
              (int)srcBuf.mDewarp);

    // unify HAL_PIXEL_FORMAT_YCbCr_420_SP to HAL_PIXEL_FORMAT_YCBCR_420_888
    if (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_SP) {
        srcBuf.mFormat = HAL_PIXEL_FORMAT_YCBCR_420_888;
    }

    if (dstBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_SP) {
        dstBuf.mFormat = HAL_PIXEL_FORMAT_YCBCR_420_888;
    }

    // for 8mp, g2d not support nv12 scale, use software to scale, or will cause
    // testAllOutputYUVResolutions failed.
    // zoom in also need handled by cpu
    if (srcBuf.mFormat == dstBuf.mFormat && dstBuf.mFormat == HAL_PIXEL_FORMAT_YCBCR_420_888) {
        if ((srcBuf.mZoomRatio <= 1.0 && (srcBuf.mWidth != dstBuf.mWidth || srcBuf.mHeight != dstBuf.mHeight)) ||
            (srcBuf.mZoomRatio > 1.0 && srcBuf.mWidth == dstBuf.mWidth && srcBuf.mHeight == dstBuf.mHeight)) {
            if (strstr(mSocType, "imx8mp")) {
                engine = ENG_OCLCVT;
            }
        }
    }

    if (engine != ENG_NOTCARE) {
        ret = (this->*g_EngFuncList[engine])(dstBuf, srcBuf);
        if (ret != 0) {
            ALOGE("%s:  ConvertImage failed, engine %d, ret:%d", __func__, engine, ret);
        }
        return ret;
    }

    // If ENG_NOTCARE, go through all engines until convert ok.
    for (int i = ENG_MIN; i < ENG_NUM; i++) {
        ret = (this->*g_EngFuncList[i])(dstBuf, srcBuf);
        if (ret != 0) {
            ALOGE("%s:  ConvertImage failed, engine %d, ret:%d", __func__, i, ret);
        } else {
            ALOGV("%s:  ConvertImage success, engine %d, ret:%d", __func__, i, ret);
            return 0;
        }
    }

    return ret;
}

int convertPixelFormatToG2DFormat(int format) {
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

int ImageProcess::ConvertImageByPXP(ImxImageBuffer &dstBuf, ImxImageBuffer &srcBuf) {
    ALOGV("%s", __func__);
    // pxp exists.
    if (mPxpFd <= 0) {
        return -EINVAL;
    }

    struct pxp_config_data pxp_conf;
    struct pxp_layer_param *src_param = NULL, *out_param = NULL;
    int32_t ret = -1;

    memset(&pxp_conf, 0, sizeof(struct pxp_config_data));

    src_param = &(pxp_conf.s0_param);
    out_param = &(pxp_conf.out_param);

    /*
     * Initialize src parameters
     */
    src_param->paddr = srcBuf.mPhyAddr;

    src_param->width = srcBuf.mWidth;
    src_param->height = srcBuf.mHeight;
    src_param->color_key = -1;
    src_param->color_key_enable = 0;
    src_param->pixel_fmt = convertPixelFormatToV4L2Format(srcBuf.mFormat);
    src_param->stride = src_param->width;
    pxp_conf.proc_data.srect.top = 0;
    pxp_conf.proc_data.srect.left = 0;
    pxp_conf.proc_data.srect.width = srcBuf.mWidth;
    pxp_conf.proc_data.srect.height = srcBuf.mHeight;

    /*
     * Initialize out parameters
     */
    out_param->paddr = dstBuf.mPhyAddr;
    out_param->width = dstBuf.mWidth;
    out_param->height = dstBuf.mHeight;
    out_param->pixel_fmt = convertPixelFormatToV4L2Format(dstBuf.mFormat);
    out_param->stride = out_param->width;
    pxp_conf.handle = mChannel;
    pxp_conf.proc_data.drect.top = 0;
    pxp_conf.proc_data.drect.left = 0;
    pxp_conf.proc_data.drect.width = dstBuf.mWidth;
    pxp_conf.proc_data.drect.height = dstBuf.mHeight;

    if ((src_param->stride == 0) || (out_param->stride == 0)) {
        ALOGE("%s:%d, src stride %d, dst stride %d", __func__, __LINE__, src_param->stride,
              out_param->stride);
        return -EINVAL;
    }

    // The fb dirver treat r as bit[0:7], but ENG_PXP convert r to bit[24:31].
    // For preview, do some trick to set format as G2D_YVYU.
    if ((src_param->pixel_fmt == PXP_PIX_FMT_YUYV) &&
        (out_param->pixel_fmt == PXP_PIX_FMT_RGBA32)) {
        src_param->pixel_fmt = PXP_PIX_FMT_YVYU;
        out_param->pixel_fmt = PXP_PIX_FMT_ARGB32;
    }

    ALOGV("src: %ux%u, 0x%x, phy 0x%lx, v4l2 0x%x, stride %d", srcBuf.mWidth, srcBuf.mHeight,
          srcBuf.mFormat, srcBuf.mPhyAddr, src_param->pixel_fmt, src_param->stride);
    ALOGV("dst: %ux%u, 0x%x, phy 0x%lx, v4l2 0x%x, stride %d", dstBuf.mWidth, dstBuf.mHeight,
          dstBuf.mFormat, dstBuf.mPhyAddr, out_param->pixel_fmt, out_param->stride);

    ret = ioctl(mPxpFd, PXP_IOC_CONFIG_CHAN, &pxp_conf);
    if (ret < 0) {
        ALOGE("%s:%d, PXP_IOC_CONFIG_CHAN failed %d", __func__, __LINE__, ret);
        return ret;
    }

    ret = ioctl(mPxpFd, PXP_IOC_START_CHAN, &(pxp_conf.handle));
    if (ret < 0) {
        ALOGE("%s:%d, PXP_IOC_START_CHAN failed %d", __func__, __LINE__, ret);
        return ret;
    }

    ret = ioctl(mPxpFd, PXP_IOC_WAIT4CMPLT, &pxp_conf);
    if (ret < 0) {
        ALOGE("%s:%d, PXP_IOC_WAIT4CMPLT failed %d", __func__, __LINE__, ret);
    }

    return ret;
}

int ImageProcess::ConvertImageByIPU(ImxImageBuffer &dstBuf, ImxImageBuffer &srcBuf) {
    ALOGV("%s", __func__);
    if (mIpuFd <= 0) {
        return -EINVAL;
    }

    if (dstBuf.mFormat == HAL_PIXEL_FORMAT_YCrCb_420_SP) {
        return -EINVAL;
    }

    struct ipu_task mTask;
    memset(&mTask, 0, sizeof(mTask));

    mTask.input.width = srcBuf.mWidth;
    mTask.input.height = srcBuf.mHeight;
    mTask.input.crop.pos.x = 0;
    mTask.input.crop.pos.y = 0;
    mTask.input.crop.w = srcBuf.mWidth;
    mTask.input.crop.h = srcBuf.mHeight;
    mTask.input.format = convertPixelFormatToV4L2Format(srcBuf.mFormat);
    mTask.input.paddr = srcBuf.mPhyAddr;

    mTask.output.format = convertPixelFormatToV4L2Format(dstBuf.mFormat);
    mTask.output.width = dstBuf.mWidth;
    mTask.output.height = dstBuf.mHeight;
    mTask.output.crop.pos.x = 0;
    mTask.output.crop.pos.y = 0;
    mTask.output.crop.w = dstBuf.mWidth;
    mTask.output.crop.h = dstBuf.mHeight;
    mTask.output.rotate = 0;
    mTask.output.paddr = dstBuf.mPhyAddr;

    int32_t ret = IPU_CHECK_ERR_INPUT_CROP;
    while (ret != IPU_CHECK_OK && ret > IPU_CHECK_ERR_MIN) {
        ret = ioctl(mIpuFd, IPU_CHECK_TASK, &mTask);
        ALOGV("%s:%d, IPU_CHECK_TASK ret=%d", __func__, __LINE__, ret);
        switch (ret) {
            case IPU_CHECK_OK:
                break;
            case IPU_CHECK_ERR_SPLIT_INPUTW_OVER:
                mTask.input.crop.w -= 8;
                break;
            case IPU_CHECK_ERR_SPLIT_INPUTH_OVER:
                mTask.input.crop.h -= 8;
                break;
            case IPU_CHECK_ERR_SPLIT_OUTPUTW_OVER:
                mTask.output.crop.w -= 8;
                break;
            case IPU_CHECK_ERR_SPLIT_OUTPUTH_OVER:
                mTask.output.crop.h -= 8;
                ;
                break;
            default:
                ALOGE("%s:%d, IPU_CHECK_TASK ret=%d", __func__, __LINE__, ret);
                return ret;
        }
    }

    ret = ioctl(mIpuFd, IPU_QUEUE_TASK, &mTask);
    if (ret < 0) {
        ALOGE("%s:%d, IPU_QUEUE_TASK failed %d", __func__, __LINE__, ret);
    }

    return ret;
}

int ImageProcess::ConvertImageByG2DCopy(ImxImageBuffer &dstBuf, ImxImageBuffer &srcBuf) {
    // gpu 2d exists.
    if (mCopyEngine == NULL) {
        return -EINVAL;
    }

    int dstFormat = convertPixelFormatToV4L2Format(dstBuf.mFormat);
    int srcFormat = convertPixelFormatToV4L2Format(srcBuf.mFormat);
    // can't do csc for YUV.
    if ((dstBuf.mFormat != srcBuf.mFormat) && (dstFormat != srcFormat)) {
        return -EINVAL;
    }

    void *g2dHandle = getHandle();
    int size = (srcBuf.mSize > dstBuf.mSize) ? dstBuf.mSize : srcBuf.mSize;

    struct g2d_buf s_buf, d_buf;
    s_buf.buf_paddr = srcBuf.mPhyAddr;
    s_buf.buf_vaddr = srcBuf.mVirtAddr;
    d_buf.buf_paddr = dstBuf.mPhyAddr;
    d_buf.buf_vaddr = dstBuf.mVirtAddr;

    Mutex::Autolock _l(mG2dLock);
    int ret = mCopyEngine(g2dHandle, (void *)&d_buf, (void *)&s_buf, (void *)(intptr_t)size);
    if (ret == 0) {
        mFinishEngine(g2dHandle);
    }

    return ret;
}

static int AllocPhyBufferByFmtRes(ImxImageBuffer &imgBuf, uint32_t format, uint32_t width,
                                  uint32_t height, uint32_t stride, bool bCached = true) {
    int ret = AllocPhyBuffer(stride, height, format, imgBuf, bCached);
    if (ret) {
        ALOGE("%s: AllocPhyBuffer failed, formatSize %d, allocSize %d", __func__,
              (int)imgBuf.mFormatSize, (int)imgBuf.mSize);
        return ret;
    }

    imgBuf.mHeightSpan = height;

    return 0;
}

int ImageProcess::ConvertImageByG2DBlit(ImxImageBuffer &dstBuf, ImxImageBuffer &srcBuf) {
    if (mBlitEngine == NULL) {
        return -EINVAL;
    }

    // can't do csc for some formats.
    if (!((dstBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_888) ||
           (dstBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_SP) ||
           ((dstBuf.mFormat == HAL_PIXEL_FORMAT_YCrCb_420_SP) &&
            (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_422_I)) ||
           ((srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_422_I) &&
            (dstBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_422_I)) ||
           ((srcBuf.mFormat == HAL_PIXEL_FORMAT_RGB_888) &&
            (dstBuf.mFormat == HAL_PIXEL_FORMAT_RGB_888)))) {
        return -EINVAL;
    }

    int ret;
    void *g2dHandle = getHandle();
    struct g2d_buf s_buf, d_buf;
    struct g2d_surface s_surface, d_surface;

    s_buf.buf_paddr = srcBuf.mPhyAddr;
    s_buf.buf_vaddr = srcBuf.mVirtAddr;
    d_buf.buf_paddr = dstBuf.mPhyAddr;
    d_buf.buf_vaddr = dstBuf.mVirtAddr;

    // zoom feature
    uint32_t crop_left = 0;
    uint32_t crop_top = 0;
    uint32_t crop_width = srcBuf.mWidth;
    uint32_t crop_height = srcBuf.mHeight;

    // currently, just suppport zoom in.
    if (srcBuf.mZoomRatio > 1.0) {
        crop_width = srcBuf.mWidth / srcBuf.mZoomRatio;
        crop_height = srcBuf.mHeight / srcBuf.mZoomRatio;
        crop_left = (srcBuf.mWidth - crop_width) / 2;
        crop_top = (srcBuf.mHeight - crop_height) / 2;
    }

    s_surface.format = (g2d_format)convertPixelFormatToG2DFormat(srcBuf.mFormat);
    s_surface.planes[0] = (long)s_buf.buf_paddr;
    s_surface.left = crop_left;
    s_surface.top = crop_top;
    s_surface.right = crop_left + crop_width;
    s_surface.bottom = crop_top + crop_height;
    s_surface.stride = srcBuf.mStride;
    s_surface.width = srcBuf.mWidth;
    s_surface.height = srcBuf.mHeight;
    s_surface.rot = G2D_ROTATION_0;

    ALOGV("%s: crop from (%d, %d), size %dx%d, srcBuf.mFormatSize %d, mZoomRatio %f", __func__,
          crop_left, crop_top, crop_width, crop_height, (int)srcBuf.mFormatSize, srcBuf.mZoomRatio);

    if (srcBuf.mDewarp && (!mDewarpCtx.enable)) {
        ALOGW("%s: want to dewarp but mDewarpCtx.enable false", __func__);
        return -EINVAL;
    }

    if ((srcBuf.mFormat == dstBuf.mFormat) ||
        (srcBuf.mZoomRatio <= 1.0 &&
         (srcBuf.mWidth == dstBuf.mWidth &&
          srcBuf.mHeight == dstBuf.mHeight))) { // just scale or just csc or just dewarp
        d_surface.format = (g2d_format)convertPixelFormatToG2DFormat(dstBuf.mFormat);
        d_surface.planes[0] = (long)d_buf.buf_paddr;
        d_surface.planes[1] = (long)d_buf.buf_paddr + dstBuf.mStride * dstBuf.mHeight;
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

        ret = mBlitEngine(g2dHandle, (void *)&s_surface, (void *)&d_surface);
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

        ret = AllocPhyBufferByFmtRes(resizeBuf, srcBuf.mFormat, dstBuf.mWidth, dstBuf.mHeight,
                                     dstBuf.mStride);
        if (ret) {
            ALOGE("%s:%d AllocPhyBufferByFmtRes failed", __func__, __LINE__);
            return BAD_VALUE;
        }
        ALOGV("%s: resizeBuf.mFormatSize %d, resizeBuf.mSize %d", __func__,
              (int)resizeBuf.mFormatSize, (int)resizeBuf.mSize);

        // first scale on same format as source
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

        Mutex::Autolock _l(mG2dLock);
        ret = mBlitEngine(g2dHandle, (void *)&s_surface, (void *)&tmp_surface);
        if (ret) {
            FreePhyBuffer(resizeBuf.buffer);
            return ret;
        }

        mFinishEngine(g2dHandle);

        // then csc to dst format
        d_surface.format = (g2d_format)convertPixelFormatToG2DFormat(dstBuf.mFormat);
        d_surface.planes[0] = (long)d_buf.buf_paddr;
        d_surface.planes[1] = (long)d_buf.buf_paddr + dstBuf.mStride * dstBuf.mHeight;
        d_surface.left = 0;
        d_surface.top = 0;
        d_surface.right = dstBuf.mWidth;
        d_surface.bottom = dstBuf.mHeight;
        d_surface.stride = dstBuf.mStride;
        d_surface.width = dstBuf.mWidth;
        d_surface.height = dstBuf.mHeight;
        d_surface.rot = G2D_ROTATION_0;

        ret = mBlitEngine(g2dHandle, (void *)&tmp_surface, (void *)&d_surface);
        if (ret) {
            FreePhyBuffer(resizeBuf.buffer);
            return ret;
        }

        mFinishEngine(g2dHandle);
        FreePhyBuffer(resizeBuf.buffer);
    }

    return ret;
}

void ImageProcess::LockG2dAddr(ImxImageBuffer &imxBuf) {
    if (imxBuf.buffer == NULL) {
        ALOGE("%s: mFd %d, buffer handle is invalid", __func__, imxBuf.mFd);
        return;
    }

    auto handle = imxBuf.buffer;
    if (mLockSurface)
        (*mLockSurface)(const_cast<native_handle_t *>(handle));

    imxBuf.mPhyAddr = gralloc_handle_phys(handle);

    return;
}

void ImageProcess::UnLockG2dAddr(ImxImageBuffer &imxBuf) {
    if (mUnlockSurface)
        (*mUnlockSurface)((void *)imxBuf.buffer);

    return;
}

buffer_handle_t ImageProcess::createBufferHandle(ImxImageBuffer &imxBuf) {
    void *mem = native_handle_create(GRALLOC_HANDLE_NUM_FDS, GRALLOC_HANDLE_NUM_INTS);
    if (mem == nullptr) {
        ALOGE("%s: gralloc_handle allocation failed", __func__);
        return nullptr;
    }

    buffer_handle_t handle =
            new (mem) gralloc_handle(imxBuf.mFd, imxBuf.mSize, 0, HAL_PIXEL_FORMAT_RGBA_8888,
                                     imxBuf.mSize / 4, 1, 1, imxBuf.mSize / 4);
    gralloc_handle_set_phys(handle, imxBuf.mPhyAddr);
    gralloc_handle_set_base(handle, (uint64_t)imxBuf.mVirtAddr);

    return handle;
}

void ImageProcess::destroyBufferHandle(buffer_handle_t buffer) {
    if (buffer)
        free((void *)buffer);
}

int ImageProcess::ConvertImageByG2D(ImxImageBuffer &dstBuf, ImxImageBuffer &srcBuf,
                                   ImxEngine engine) {
    int ret = 0;

    if (mG2dHandle == NULL)
        return BAD_VALUE;

    if (mBlitEngine && (engine == ENG_G2D) && mbVIVG2D) {
        LockG2dAddr(srcBuf);
        LockG2dAddr(dstBuf);
    }

    if ((srcBuf.mFormat == dstBuf.mFormat) && (srcBuf.mWidth == dstBuf.mWidth) &&
        (srcBuf.mHeight == dstBuf.mHeight) && (srcBuf.mZoomRatio <= 1.0) &&
        (srcBuf.mDewarp == false)) {
        ret = ConvertImageByG2DCopy(dstBuf, srcBuf);
    } else {
        ret = ConvertImageByG2DBlit(dstBuf, srcBuf);
    }

    if (mBlitEngine && (engine == ENG_G2D) && mbVIVG2D) {
        UnLockG2dAddr(srcBuf);
        UnLockG2dAddr(dstBuf);
    }

    return ret;
}

int ImageProcess::ConvertImageByDPU(ImxImageBuffer &dstBuf, ImxImageBuffer &srcBuf) {
    return ConvertImageByG2D(dstBuf, srcBuf, ENG_DPU);
}

int ImageProcess::ConvertImageByGPU_2D(ImxImageBuffer &dstBuf, ImxImageBuffer &srcBuf) {
    return ConvertImageByG2D(dstBuf, srcBuf, ENG_G2D);
}

int ImageProcess::convertNV12toNV21(ImxImageBuffer &dstBuf, ImxImageBuffer &srcBuf) {
    int Ysize = 0, UVsize = 0;
    uint8_t *srcIn, *dstOut;
    uint32_t *UVout;
    int size = (srcBuf.mSize > dstBuf.mSize) ? dstBuf.mSize : srcBuf.mSize;

    Ysize = srcBuf.mWidth * srcBuf.mHeight;
    UVsize = srcBuf.mWidth * srcBuf.mHeight >> 2;
    srcIn = (uint8_t *)srcBuf.mVirtAddr;
    dstOut = (uint8_t *)dstBuf.mVirtAddr;
    UVout = (uint32_t *)(dstOut + Ysize);

    if (mCopyEngine != NULL) {
        void *g2dHandle = getHandle();
        struct g2d_buf s_buf, d_buf;
        s_buf.buf_paddr = srcBuf.mPhyAddr;
        s_buf.buf_vaddr = srcBuf.mVirtAddr;
        d_buf.buf_paddr = dstBuf.mPhyAddr;
        d_buf.buf_vaddr = dstBuf.mVirtAddr;
        mCopyEngine(g2dHandle, (void *)&d_buf, (void *)&s_buf, (void *)(intptr_t)size);
        mFinishEngine(g2dHandle);
    } else {
        memcpy(dstOut, srcIn, size);
    }

    Revert16BitEndian((uint8_t *)UVout, (uint8_t *)UVout, UVsize / 2);

    return 0;
}

void ImageProcess::convertNV12toYV12(uint8_t *inputBuffer, uint8_t *outputBuffer, int width,
                                     int height) {
    int size = width * height;
    // Y
    memcpy(outputBuffer, inputBuffer, size);
    // V
    uint8_t *ptrV1 = outputBuffer + size;
    uint8_t *ptrV2 = inputBuffer + size + 1;
    // U
    uint8_t *ptrU1 = outputBuffer + size * 5 / 4;
    uint8_t *ptrU2 = inputBuffer + size;
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

int ImageProcess::ConvertImageByGPU_3D(ImxImageBuffer &dstBuf, ImxImageBuffer &srcBuf) {
    // opencl g2d exists.
    if (mCLHandle == NULL) {
        return -EINVAL;
    }

    int ret = 0;
    ImxImageBuffer resizeBuf;
    memset(&resizeBuf, 0, sizeof(resizeBuf));
    bool bResize = false;

    // Set input/output cache attrib based on usage.
    bool bInputCached =
            srcBuf.mUsage & (GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN);
    bool bOutputCached =
            dstBuf.mUsage & (GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN);

    ALOGV("%s:%d, bInputCached %d, srcUsage 0x%lx, bOutputCached %d, dstUsage 0x%lx, "
          "res src %ux%u, dst %ux%u, format src 0x%x, dst 0x%x, size %d",
          __FUNCTION__, __LINE__, bInputCached, srcBuf.mUsage, bOutputCached, dstBuf.mUsage,
          srcBuf.mWidth, srcBuf.mHeight, dstBuf.mWidth, dstBuf.mHeight, srcBuf.mFormat,
          dstBuf.mFormat, (int)srcBuf.mFormatSize);

    // case 1: same format, same resolution, copy
    if ((srcBuf.mFormat == dstBuf.mFormat) && (srcBuf.mWidth == dstBuf.mWidth) &&
        (srcBuf.mHeightSpan == dstBuf.mHeightSpan)) {
        Mutex::Autolock _l(mCLLock);

        cl_Copy(mCLHandle, (uint8_t *)dstBuf.mPhyAddr, (uint8_t *)srcBuf.mPhyAddr,
                srcBuf.mFormatSize, bInputCached, bOutputCached, true);

        (*mCLFlush)(mCLHandle);
        (*mCLFinish)(mCLHandle);

        return 0;
    }

    // case 2: same format, different resolution, resize
    if (srcBuf.mFormat == dstBuf.mFormat) {
        resizeWrapper(srcBuf, dstBuf, ENG_G3D);
        return 0;
    }

    // filter out unsupported CSC format
    if (false == IsCscSupportByG3D(srcBuf.mFormat, dstBuf.mFormat)) {
        ALOGE("%s:%d, G3D don't support format convert from 0x%x to 0x%x", __func__, __LINE__,
              srcBuf.mFormat, dstBuf.mFormat);
        return -EINVAL;
    }

    // case 3: diffrent format, different resolution
    // first resize, then go through case 4.
    if ((srcBuf.mWidth != dstBuf.mWidth) || (srcBuf.mHeight != dstBuf.mHeight)) {
        // for android.hardware.camera2.cts.ImageReaderTest#testAllOutputYUVResolutions[1] on 8mq,
        // need uncached buffer for resize->csc, Otherwise the image will have green lines.
        bool bCached = true;

        if (strstr(mSocType, "imx8mq")) {
            bCached = false;
        }

        ret = AllocPhyBufferByFmtRes(resizeBuf, srcBuf.mFormat, dstBuf.mWidth, dstBuf.mHeight,
                                     dstBuf.mStride, bCached);
        if (ret) {
            ALOGE("%s:%d AllocPhyBufferByFmtRes failed", __func__, __LINE__);
            return -EINVAL;
        }

        resizeWrapper(srcBuf, resizeBuf, ENG_G3D);
        SwitchImxBuf(srcBuf, resizeBuf);

        bResize = true;
        // if resized, input cache attrib may change, update the cache state
        bInputCached = srcBuf.mUsage & (GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN);
    }

    // case 4: diffrent format, same resolution
    {
        Mutex::Autolock _l(mCLLock);

        // on 8mq, if use virtual address, preview will freeze or black, use physical address here.
        cl_Csc(mCLHandle, (uint8_t *)srcBuf.mPhyAddr, (uint8_t *)dstBuf.mPhyAddr, dstBuf.mWidth,
               dstBuf.mHeight, srcBuf.mStride, dstBuf.mStride, srcBuf.mHeightSpan, bInputCached,
               bOutputCached, srcBuf.mFormat, dstBuf.mFormat, true);

        (*mCLFlush)(mCLHandle);
        (*mCLFinish)(mCLHandle);
    }

    if (bResize) {
        SwitchImxBuf(srcBuf, resizeBuf);
        FreePhyBuffer(resizeBuf.buffer);
    }

    return 0;
}

int ImageProcess::ConvertImageByCPU(ImxImageBuffer &dstBuf, ImxImageBuffer &srcBuf) {
    ImxImageBuffer resizeBuf;
    memset(&resizeBuf, 0, sizeof(resizeBuf));
    int ret;
    bool bResize = false;

    // case 1: same format, same resolution, copy
    if ((srcBuf.mFormat == dstBuf.mFormat) && (srcBuf.mWidth == dstBuf.mWidth) &&
        (srcBuf.mHeight == dstBuf.mHeight)) {
        if (srcBuf.mZoomRatio <= 1.0) {
            memcpy((uint8_t *)dstBuf.mVirtAddr, (uint8_t *)srcBuf.mVirtAddr,
                    dstBuf.mFormatSize);
        } else if (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCBCR_420_888) {
            // Handle zoom in for nv12
            int crop_width = srcBuf.mWidth / srcBuf.mZoomRatio;
            int crop_height = srcBuf.mHeight / srcBuf.mZoomRatio;

            resizeBuf.mFormatSize = srcBuf.mFormatSize;
            ret = AllocPhyBuffer(crop_width, crop_height, srcBuf.mFormat, resizeBuf);
            if (ret) {
                ALOGE("%s:%d AllocPhyBuffer failed", __func__, __LINE__);
                return BAD_VALUE;
            }

            // First Cut, then Scale
            decreaseNV12WithCut((uint8_t *)srcBuf.mVirtAddr, srcBuf.mWidth, srcBuf.mHeight,
                                (uint8_t *)resizeBuf.mVirtAddr, resizeBuf.mWidth,
                                resizeBuf.mHeight);

            uint8_t *pUVSrcStart =
                    (uint8_t *)resizeBuf.mVirtAddr + resizeBuf.mWidth * resizeBuf.mHeight;
            uint8_t *pUVDstStart = (uint8_t *)dstBuf.mVirtAddr + dstBuf.mWidth * dstBuf.mHeight;

            ret = libyuv::NV12Scale(static_cast<uint8_t *>(resizeBuf.mVirtAddr), resizeBuf.mWidth,
                                    static_cast<uint8_t *>(pUVSrcStart), resizeBuf.mWidth,
                                    resizeBuf.mWidth, resizeBuf.mHeight,
                                    static_cast<uint8_t *>(dstBuf.mVirtAddr), dstBuf.mWidth,
                                    static_cast<uint8_t *>(pUVDstStart), dstBuf.mWidth,
                                    dstBuf.mWidth, dstBuf.mHeight, libyuv::FilterMode::kFilterNone);

            FreePhyBuffer(resizeBuf.buffer);

            if (ret != 0) {
                ALOGE("%s: failed to scale buffer from %dx%d to %dx%d. Ret %d", __FUNCTION__,
                      resizeBuf.mWidth, resizeBuf.mHeight, dstBuf.mWidth, dstBuf.mHeight, ret);
                return ret;
            }
        }

        return 0;
    }

    // case 2: same format, different resolution, resize
    if (srcBuf.mFormat == dstBuf.mFormat) {
        ret = resizeWrapper(srcBuf, dstBuf, ENG_CPU);
        return ret;
    }

    // filter out unsupported CSC format
    if (false == IsCscSupportByCPU(srcBuf.mFormat, dstBuf.mFormat)) {
        ALOGE("%s:%d, Software don't support format convert from 0x%x to 0x%x", __func__, __LINE__,
              srcBuf.mFormat, dstBuf.mFormat);
        return -EINVAL;
    }

    // case 3: diffrent format, different resolution
    // first resize, then go through case 4.
    if ((srcBuf.mWidth != dstBuf.mWidth) || (srcBuf.mHeight != dstBuf.mHeight)) {
        ret = AllocPhyBufferByFmtRes(resizeBuf, srcBuf.mFormat, dstBuf.mWidth, dstBuf.mHeight,
                                     dstBuf.mStride);
        if (ret) {
            ALOGE("%s:%d AllocPhyBufferByFmtRes failed", __func__, __LINE__);
            return -EINVAL;
        }

        resizeWrapper(srcBuf, resizeBuf, ENG_CPU);
        SwitchImxBuf(srcBuf, resizeBuf);
        bResize = true;
    }

    // case 4: diffrent format, same resolution
    if (((dstBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_888) ||
         (dstBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_SP)) &&
        (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_422_I)) {
        convertYUYVtoNV12SP((uint8_t *)srcBuf.mVirtAddr, (uint8_t *)dstBuf.mVirtAddr, dstBuf.mWidth,
                            dstBuf.mHeight, srcBuf.mStride, dstBuf.mStride);
    } else if ((srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_SP) &&
               (dstBuf.mFormat == HAL_PIXEL_FORMAT_YCrCb_420_SP)) {
        convertNV12toNV21(dstBuf, srcBuf);
    } else if ((srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_SP ||
                (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_888)) &&
               (dstBuf.mFormat == HAL_PIXEL_FORMAT_YV12)) {
        convertNV12toYV12((uint8_t *)srcBuf.mVirtAddr, (uint8_t *)dstBuf.mVirtAddr, dstBuf.mWidth,
                          dstBuf.mHeight);
    } else {
        ALOGE("%s:%d should not enter here", __func__, __LINE__);
    }

    if (bResize) {
        SwitchImxBuf(srcBuf, resizeBuf);
        FreePhyBuffer(resizeBuf.buffer);
    }

    return 0;
}

void ImageProcess::cl_Copy(void *g2dHandle, uint8_t *output, uint8_t *input, uint32_t size,
                           bool bInputCached, bool bOutputCached, bool bUsePhyAddr) {
    struct cl_g2d_buf g2d_output_buf;
    struct cl_g2d_buf g2d_input_buf;

    g2d_output_buf.buf_paddr = (uint64_t)output;
    g2d_output_buf.buf_size = size;
    g2d_output_buf.use_phy = bUsePhyAddr;
    g2d_output_buf.usage = bOutputCached ? CL_G2D_CACHED_MEMORY : CL_G2D_UNCACHED_MEMORY;

    g2d_input_buf.buf_paddr = (uint64_t)input;
    g2d_input_buf.buf_size = size;
    g2d_input_buf.use_phy = bUsePhyAddr;
    g2d_input_buf.usage = bInputCached ? CL_G2D_CACHED_MEMORY : CL_G2D_UNCACHED_MEMORY;

    (*mCLCopy)(g2dHandle, &g2d_output_buf, &g2d_input_buf, (void *)(intptr_t)size);
}

void ImageProcess::cl_Csc(void *g2dHandle, uint8_t *inputBuffer, uint8_t *outputBuffer, int width,
                          int height, int srcStride, int dstStride, int srcHeightSpan,
                          bool bInputCached, bool bOutputCached, uint32_t inFmt, uint32_t outFmt,
                          bool bUsePhyAddr) {
    struct cl_g2d_surface src, dst;

    src.format = (cl_g2d_format)convertPixelFormatToCLFormat(inFmt);
    src.usage = bInputCached ? CL_G2D_CACHED_MEMORY : CL_G2D_UNCACHED_MEMORY;
    src.planes[0] = (long)inputBuffer;
    src.planes[1] = (long)inputBuffer + srcStride * srcHeightSpan;
    src.left = 0;
    src.top = 0;
    src.right = width;
    src.bottom = height;
    src.stride = srcStride;
    src.width = width;
    src.height = height;
    src.usePhyAddr = bUsePhyAddr;

    dst.format = (cl_g2d_format)convertPixelFormatToCLFormat(outFmt);
    dst.usage = bOutputCached ? CL_G2D_CACHED_MEMORY : CL_G2D_UNCACHED_MEMORY;
    dst.planes[0] = (long)outputBuffer;
    dst.planes[1] = (long)outputBuffer + dstStride * height;
    dst.left = 0;
    dst.top = 0;
    dst.right = width;
    dst.bottom = height;
    dst.stride = dstStride;
    dst.width = width;
    dst.height = height;
    dst.usePhyAddr = bUsePhyAddr;

    (*mCLBlit)(g2dHandle, (void *)&src, (void *)&dst);
}

OCL_HANDLE ImageProcess::GetOCLHandle(bool needDewarp) {
    if (mLayoutMBCam == false)
        return mHOcl;

    OCL_HANDLE hOcl = (OCL_HANDLE)pthread_getspecific(m_ocl_key);
    // first time get, create ocl handle.
    if (hOcl == NULL) {
        int ret = (*m_ocl_open)(OCL_OPEN_FLAG_PROFILE, &hOcl);
        if ((ret != 0) || (hOcl == NULL)) {
            ALOGW("%s: m_ocl_open failed, ret %d", __func__, ret);
            return NULL;
        }

        if ((m_warp_param.enable == 1) && needDewarp)
            m_ocl_setParam(hOcl, OCL_PARAM_INDEX_WARP_PARAM, &m_warp_param);

        ALOGI("%s: call pthread_setspecific, hOcl %p", __func__, hOcl);
        pthread_setspecific(m_ocl_key, (void *)hOcl);
    }

    return hOcl;
}

void ImageProcess::ImxImageBufferToOclBuffer(ImxImageBuffer &imxImgBuf, OCL_BUFFER &oclBuf,
                                             OCL_FORMAT &oclFmt) {
    int ret = 0;
    OCL_FORMAT_PLANE_INFO plane_info;

    memset(&plane_info, 0, sizeof(plane_info));
    plane_info.ocl_format = &oclFmt;

    OCL_HANDLE hOcl = GetOCLHandle();
    if (hOcl == NULL) {
        ALOGE("%s: unexpected hOcl NULL", __func__);
        return;
    }

    ret = m_ocl_getParam(hOcl, OCL_PARAM_INDEX_FORMAT_PLANE_INFO, &plane_info);
    if (ret) {
        ALOGE("%s: m_ocl_getParam OCL_PARAM_INDEX_FORMAT_PLANE_INFO failed, ret %d", __func__, ret);
        return;
    }

    oclBuf.mem_type = mOclBufferType;
    oclBuf.plane_num = plane_info.plane_num;
    int offset = 0;

    if(mOclBufferType == OCL_MEM_TYPE_GPU){

        for (int i = 0; i < oclBuf.plane_num; i++) {
            oclBuf.planes[i].paddr = (long long)imxImgBuf.mPhyAddr + (long long)offset;
            oclBuf.planes[i].size = plane_info.plane_size[i];
            offset += oclBuf.planes[i].size;
        }
    }else if(mOclBufferType == OCL_MEM_TYPE_DEVICE) {

        for (int i = 0; i < oclBuf.plane_num; i++) {
            oclBuf.planes[i].fd = (long long)imxImgBuf.mFd;
            oclBuf.planes[i].offset = (long long)offset;
            oclBuf.planes[i].size = plane_info.plane_size[i] + offset;
            offset += oclBuf.planes[i].size;
        }
    }else{

        for (int i = 0; i < oclBuf.plane_num; i++) {
            oclBuf.planes[i].vaddr = (long long)imxImgBuf.mVirtAddr + offset;
            oclBuf.planes[i].size = plane_info.plane_size[i];
            offset += oclBuf.planes[i].size;
        }
    }

    return;
}

static void HalPixelFormatToOclPixelFormat(uint32_t &halPixelFormat,
                                           OCL_PIXEL_FORMAT &oclPixelFormat) {
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
        default:
            ALOGW("==xx %s: unsupported halPixelFormat %d, set oclPixelFormat to OCL_FORMAT_YUYV",
                  __func__, halPixelFormat);
            oclPixelFormat = OCL_FORMAT_YUYV;
            break;
    }

    return;
}

static void ImxImageBufferToOclFormat(ImxImageBuffer &srcImgBuf, OCL_FORMAT &input_format,
                                      ImxImageBuffer &dstImgBuf, OCL_FORMAT &output_format) {
    OCL_PIXEL_FORMAT srcPixelFormat, dstPixelFormat;

    HalPixelFormatToOclPixelFormat(srcImgBuf.mFormat, srcPixelFormat);
    HalPixelFormatToOclPixelFormat(dstImgBuf.mFormat, dstPixelFormat);

    // Calculate scale ratios for each dimension
    int h_offset = 0;
    int v_offset = 0;
    int crop_width = srcImgBuf.mWidth;
    int crop_height = srcImgBuf.mHeightSpan;
    float h_scale_ratio = (float)srcImgBuf.mWidth / dstImgBuf.mWidth;
    float v_scale_ratio = (float)srcImgBuf.mHeightSpan / dstImgBuf.mHeight;

    // Use the smaller scale ratio to maintain aspect ratio (crop the other dimension)
    float scale_ratio = (h_scale_ratio < v_scale_ratio) ? h_scale_ratio : v_scale_ratio;

    if (scale_ratio > 1.0f) {
        // Calculate what the source dimensions should be to maintain aspect ratio
        crop_width = (int)(dstImgBuf.mWidth * scale_ratio);
        crop_height = (int)(dstImgBuf.mHeight * scale_ratio);

        // Ensure crop dimensions don't exceed source dimensions
        if (crop_width > (int)srcImgBuf.mWidth)
            crop_width = srcImgBuf.mWidth;
        if (crop_height > (int)srcImgBuf.mHeightSpan)
            crop_height = srcImgBuf.mHeightSpan;

        // Center the crop area
        h_offset = (srcImgBuf.mWidth - crop_width) / 2;
        v_offset = (srcImgBuf.mHeightSpan - crop_height) / 2;
    } else if (scale_ratio < 1.0f) {
        ALOGW("%s: cropping for upscale is not considered. srcWidth %u, dstWidth %u, srcHeightSpan %u, dstHeight %u",
              __func__, srcImgBuf.mWidth, dstImgBuf.mWidth, srcImgBuf.mHeightSpan,
              dstImgBuf.mHeight);
    }

    // Set source format, update input format to use the cropped area
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

    // Set destination format
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

int ImageProcess::ConvertImageByOclCvt(ImxImageBuffer &dstBuf, ImxImageBuffer &srcBuf) {
    int ret = 0;

    OCL_HANDLE hOcl = GetOCLHandle(srcBuf.mDewarp);
    if (hOcl == NULL) {
        ALOGE("%s: unexpected hOcl NULL", __func__);
        return BAD_VALUE;
    }

    /* set format */
    OCL_FORMAT input_format;
    OCL_FORMAT output_format;

    memset(&input_format, 0, sizeof(input_format));
    memset(&output_format, 0, sizeof(output_format));

    // Not 4 ox03c10 dewarp case, use the same 1 handle opened in constructor.
    if (hOcl == mHOcl) {
        mOclCvtLock.lock();
    }

    ImxImageBufferToOclFormat(srcBuf, input_format, dstBuf, output_format);

    ret = m_ocl_setParam(hOcl, OCL_PARAM_INDEX_INPUT_FORMAT, &input_format);
    if (ret) {
        ALOGE("%s: m_ocl_setParam OCL_PARAM_INDEX_INPUT_FORMAT failed, ret %d", __func__, ret);
        goto finish;
    }

    ret = m_ocl_setParam(hOcl, OCL_PARAM_INDEX_OUTPUT_FORMAT, &output_format);
    if (ret) {
        ALOGE("%s: m_ocl_setParam OCL_PARAM_INDEX_OUTPUT_FORMAT failed, ret %d", __func__, ret);
        goto finish;
    }

    /* set buffer */
    OCL_BUFFER inBuffer;
    OCL_BUFFER outBuffer;

    memset(&inBuffer, 0, sizeof(inBuffer));
    memset(&outBuffer, 0, sizeof(outBuffer));

    ImxImageBufferToOclBuffer(srcBuf, inBuffer, input_format);
    ImxImageBufferToOclBuffer(dstBuf, outBuffer, output_format);

    ret = m_ocl_convert(hOcl, &inBuffer, &outBuffer);
    if (ret) {
        ALOGE("%s: m_ocl_convert failed, ret %d", __func__, ret);
        goto finish;
    }

    OCL_RUN_TIME time;
    ret = m_ocl_getParam(hOcl, OCL_PARAM_INDEX_RUN_TIME, &time);
    if (mDebug)
        ALOGI("%s: m_ocl_convert, ret %d, src: res %dx%d, fmt %d, dst: res %dx%d, fmt %d, run_time: %d us, kernel_time: %d us\n",
              __func__, ret, input_format.width, input_format.height, input_format.format,
              output_format.width, output_format.height, output_format.format, time.run_time,
              time.kernel_time);

finish:
    if (hOcl == mHOcl) {
        mOclCvtLock.unlock();
    }

    return ret;
}

void ImageProcess::convertYUYVtoNV12SP(uint8_t *inputBuffer, uint8_t *outputBuffer, int width,
                                       int height, int srcStride, int dstStride) {
#define u32 unsigned int
#define u8 unsigned char

    u32 h, w;
    u32 nHeight = height;
    u32 nWidthDiv4 = srcStride / 4;

    u32 *pYSrcOffset = (u32 *)inputBuffer;
    u32 value = 0;
    u32 value2 = 0;

    u32 *pYDstOffset = (u32 *)outputBuffer;
    u32 *pUVDstOffset = (u32 *)(((u8 *)(outputBuffer)) + dstStride * height);

    for (h = 0; h < nHeight; h++) {
        if (!(h & 0x1)) {
            for (w = 0; w < nWidthDiv4; w++) {
                value = (*pYSrcOffset);
                value2 = (*(pYSrcOffset + 1));
                // use bitwise operation to get data from src to improve performance.
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

int ImageProcess::resizeWrapper(ImxImageBuffer &srcBuf, ImxImageBuffer &dstBuf, ImxEngine engine) {
    int ret = 0;
    ALOGV("enter resizeWrapper");

    if (srcBuf.mFormat != dstBuf.mFormat) {
        ALOGE("%s: format are differet, src 0x%x, dst 0x%x", __func__, srcBuf.mFormat,
              dstBuf.mFormat);
        return BAD_VALUE;
    }

    if ((srcBuf.mWidth == dstBuf.mWidth) && (srcBuf.mHeightSpan == dstBuf.mHeightSpan)) {
        ALOGE("%s: resolution are same, %dx%d", __func__, srcBuf.mWidth, srcBuf.mHeight);
        return BAD_VALUE;
    }

    if ((engine == ENG_G2D) || (engine == ENG_DPU))
        goto g2d_resize;

    if ((engine == ENG_G3D) || (engine == ENG_OCLCVT))
        goto g3d_resize;

    goto cpu_resize;

g2d_resize:
    ret = ConvertImageByG2D(dstBuf, srcBuf, engine);
    if (mDebug)
        ALOGI("%s: resize format 0x%x, res %dx%d to %dx%d by g2d, ret %d", __func__, srcBuf.mFormat,
              srcBuf.mWidth, srcBuf.mHeight, dstBuf.mWidth, dstBuf.mHeight, ret);
    if (ret == 0) {
        return 0;
    }

g3d_resize:
    ret = ConvertImageByOclCvt(dstBuf, srcBuf);
    if (mDebug)
        ALOGI("%s: resize format 0x%x, res %dx%d to %dx%d by oclcvt, ret %d", __func__,
              srcBuf.mFormat, srcBuf.mWidth, srcBuf.mHeight, dstBuf.mWidth, dstBuf.mHeight, ret);
    if (ret == 0) {
        return 0;
    }

cpu_resize:
    // cpu resize
    if (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCBCR_422_I)
        ret = yuv422iResize((uint8_t *)srcBuf.mVirtAddr, srcBuf.mWidth, srcBuf.mHeight,
                            (uint8_t *)dstBuf.mVirtAddr, dstBuf.mWidth, dstBuf.mHeight);
    else if (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCBCR_422_SP)
        ret = yuv422spResize((uint8_t *)srcBuf.mVirtAddr, srcBuf.mWidth, srcBuf.mHeight,
                             (uint8_t *)dstBuf.mVirtAddr, dstBuf.mWidth, dstBuf.mHeight,
                             dstBuf.mStride, srcBuf.mHeightSpan);
    else if ((srcBuf.mFormat == HAL_PIXEL_FORMAT_YCBCR_420_888) ||
             (srcBuf.mFormat == HAL_PIXEL_FORMAT_YCbCr_420_SP)) {
        ret = yuv420spResize((uint8_t *)srcBuf.mVirtAddr, srcBuf.mWidth, srcBuf.mHeight,
                             (uint8_t *)dstBuf.mVirtAddr, dstBuf.mWidth, dstBuf.mHeight);
    } else
        ALOGE("%s: resize by ENG_CPU, unsupported format 0x%x", __func__, srcBuf.mFormat);

    if (mDebug)
        ALOGI("%s: resize format 0x%x, res %dx%d to %dx%d by cpu, ret %d", __func__, srcBuf.mFormat,
              srcBuf.mWidth, srcBuf.mHeight, dstBuf.mWidth, dstBuf.mHeight, ret);

    return ret;
}

// ref https://jira.sw.nxp.com/browse/ATT-1051
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

// warp_param is already zeroed before call.
int ImageProcess::parse_warp_file(const char *file_name, OCL_WARP_PARAM *warp_param) {
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

    // open, read head
    ALOGI("%s: parse %s, WARP_FILE_HEAD_SIZE %u", __func__, file_name, WARP_FILE_HEAD_SIZE);
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
    ALOGI("%s: %s size %u bytes", __func__, file_name, file_size);

    if (file_size < WARP_FILE_HEAD_SIZE) {
        ALOGE("%s: %s size %u is less than WARP_FILE_HEAD_SIZE %u", __func__, file_name, file_size,
              WARP_FILE_HEAD_SIZE);
        ret = -1;
        goto fail;
    }

    ret = read(fd, &warpFileHead, WARP_FILE_HEAD_SIZE);
    if ((uint32_t)ret != WARP_FILE_HEAD_SIZE) {
        ALOGE("%s: %s read, expected %u, actually %u", __func__, file_name, WARP_FILE_HEAD_SIZE,
              (uint32_t)ret);
        ret = -1;
        goto fail;
    }

    ALOGI("%s: head_size %u, format_version %u, algorithm %u, width %u, height %u", __func__,
          warpFileHead.head_size, (uint32_t)warpFileHead.format_version, (uint32_t)warpFileHead.algorithm,
          warpFileHead.width, warpFileHead.height);

    // some verify
    if ((warpFileHead.format_version == 1) && (warpFileHead.head_size != WARP_FILE_HEAD_SIZE)) {
        ALOGI("%s: format_version 1 but head size(%u) != WARP_FILE_HEAD_SIZE(%u)", __func__,
              warpFileHead.head_size, WARP_FILE_HEAD_SIZE);
        ret = -1;
        goto fail;
    }

    if (warpFileHead.algorithm != WARP_PNT_32BPP) {
        ALOGI("%s: unsupported algorithm %u, expected %u", __func__, (uint32_t)warpFileHead.algorithm,
              WARP_PNT_32BPP);
        ret = -1;
        goto fail;
    }

    data_size = warpFileHead.width * warpFileHead.height * 4; // 32bpp
    if (data_size + warpFileHead.head_size != file_size) {
        ALOGI("%s: data_size(%u) + warpFileHead.head_size(%u) != file_size(%u)", __func__,
              data_size, warpFileHead.head_size, file_size);
        ret = -1;
        goto fail;
    }

    // alloc data buffer
    aligned_size = (data_size + 4095) & ~4095;
    if (mOclBufferType == OCL_MEM_TYPE_DEVICE) {
        ret = AllocPhyBuffer(aligned_size, 1, HAL_PIXEL_FORMAT_BLOB, mWarpBuffer, false);
        if (ret) {
            ALOGE("%s: AllocPhyBuffer for mWarpBuffer failed, size %u, aligned_size %u", __func__,
                  data_size, aligned_size);
            ret = -1;
            goto fail;
        }

        warp_param->buf.planes[0].fd = mWarpBuffer.mFd;
        warp_param->buf.planes[0].size = aligned_size;
        warp_param->buf.planes[0].vaddr = (long long)mWarpBuffer.mVirtAddr;
    } else {
        void *ptr = malloc(aligned_size);
        if (!ptr) {
            ALOGE("%s: read warp cooordinate file: aligned_alloc failed, size: %u, aligned_size %u",
                  __func__, data_size, aligned_size);
            ret = -1;
            goto fail;
        }

        warp_param->buf.planes[0].fd = -1;
        warp_param->buf.planes[0].size = aligned_size;
        warp_param->buf.planes[0].vaddr = (long long)ptr;
    }

    ALOGI("%s: fd %d, size %u, aligned_size %u, vaddr %p", __func__, warp_param->buf.planes[0].fd,
          data_size, aligned_size, (void *)warp_param->buf.planes[0].vaddr);

    if (data_size > (uint32_t)(warp_param->buf.planes[0].size)) {
        ALOGE("%s: data_size %u exceeds allocated buffer size %u", __func__,
              data_size, warp_param->buf.planes[0].size);
        ret = -1;
        goto fail;
    }

    // read data from file
    ret = read(fd, (void *)warp_param->buf.planes[0].vaddr, data_size);
    if ((uint32_t)ret != data_size) {
        ALOGE("%s: read warp data, expected %u, acutally %u", __func__, data_size, (uint32_t)ret);
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
        free((void *)warp_param->buf.planes[0].vaddr);

    close(fd);

    return ret;
}

int ImageProcess::PrepareDewarpBinary() {
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

    ALOGI("%s: coord para: addr 0x%lx, format %d, bpp %d, width %d, height %d, x %u, y %u, xx %u, xy %u, yx %u, yy %u\n",
          __func__, mDewarpCtx.coord.addr, mDewarpCtx.coord.format, mDewarpCtx.coord.bpp,
          mDewarpCtx.coord.width, mDewarpCtx.coord.height, mDewarpCtx.coord.arb_start_x,
          mDewarpCtx.coord.arb_start_y, mDewarpCtx.coord.arb_delta_xx,
          mDewarpCtx.coord.arb_delta_xy, mDewarpCtx.coord.arb_delta_yx,
          mDewarpCtx.coord.arb_delta_yy);

    mDewarpCtx.enable = true;

    return 0;
}

} // namespace fsl
