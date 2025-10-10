/*
 *  Copyright 2025 NXP
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
 *
 */
#include "OpenCLConverter.h"

#include <dlfcn.h>
#include <drm_fourcc.h>
#include <graphics_ext.h>
#include <log/log.h>
#include <vndksupport/linker.h>

#include "DeviceComposer.h"

namespace aidl::android::hardware::graphics::composer3::impl {
thread_local void *OclConverter::sHandle(0);

OclConverter::OclConverter() {
    mLibHandle = android_load_sphal_library(OCL_LIB_NAME, RTLD_LOCAL | RTLD_NOW);
    if (mLibHandle == nullptr) {
        mOpen = nullptr;
        mClose = nullptr;
        mSetParam = nullptr;
        mGetParam = nullptr;
        mConvert = nullptr;
    } else {
        mOpen = (tOCL_Open)dlsym(mLibHandle, "OCL_Open");
        mClose = (tOCL_Close)dlsym(mLibHandle, "OCL_Close");
        mSetParam = (tOCL_SetParam)dlsym(mLibHandle, "OCL_SetParam");
        mGetParam = (tOCL_GetParam)dlsym(mLibHandle, "OCL_GetParam");
        mConvert = (tOCL_Convert)dlsym(mLibHandle, "OCL_Convert");
    }
    if (!mOpen || !mClose || !mSetParam || !mGetParam || !mConvert) {
        ALOGE("%s: dlsym failed, err: %s", __func__, dlerror());
    }

#ifdef G2D_OCL_WITH_DMA_BUF
    mOclBufferType = OCL_MEM_TYPE_DEVICE;
#else
    mOclBufferType = OCL_MEM_TYPE_GPU;
#endif
}

OclConverter::~OclConverter() {
    if (sHandle) {
        mClose(sHandle);
        sHandle = nullptr;
    }

    if (mLibHandle) {
        dlclose(mLibHandle);
    }
}

void *OclConverter::getHandle() {
    if (sHandle != NULL)
        return sHandle;

    if (mOpen == NULL)
        return NULL;

    if (mOpen(OCL_OPEN_FLAG_DEFAULT, &sHandle) != OCL_SUCCESS) {
        ALOGE("%s: OpenCL open operation failed", __func__);
    } else if (NULL == sHandle) {
        ALOGE("%s: OpenCL open but return null handle!", __func__);
    }

    return sHandle;
}

bool OclConverter::isValid() {
    return (getHandle() != NULL && mConvert != NULL);
}

void OclConverter::G2dBufferToOclFormat(G2dBuffer &buff, OCL_FORMAT &oclFormat) {
    OCL_PIXEL_FORMAT oclPixelFormat;
    switch (buff.infoPtr->drm_format) {
        case DRM_FORMAT_NV12:
            oclPixelFormat = OCL_FORMAT_NV12;
            break;
        case DRM_FORMAT_YUYV:
            oclPixelFormat = OCL_FORMAT_YUYV;
            break;
        case DRM_FORMAT_NV16:
            oclPixelFormat = OCL_FORMAT_NV16;
            break;
        case DRM_FORMAT_NV15:
            if (buff.infoPtr->modifier != DRM_FORMAT_MOD_LINEAR)
                oclPixelFormat = OCL_FORMAT_NV15_TILED;
            else
                oclPixelFormat = OCL_FORMAT_NV15;
            break;
        case DRM_FORMAT_ABGR8888:
            oclPixelFormat = OCL_FORMAT_RGBA8888;
            break;
        default:
            ALOGW("%s: unsupported pixel format 0x%x, use OCL_FORMAT_YUYV by default", __func__,
                  buff.infoPtr->format);
            oclPixelFormat = OCL_FORMAT_YUYV;
            break;
    }

    oclFormat.format = oclPixelFormat;
    oclFormat.width = buff.infoPtr->width;
    oclFormat.height = buff.infoPtr->height;
    oclFormat.stride = buff.infoPtr->stride;
    oclFormat.sliceheight = buff.infoPtr->height;
    oclFormat.left = 0;
    oclFormat.top = 0;
    oclFormat.right = buff.infoPtr->width;
    oclFormat.bottom = buff.infoPtr->height;
    oclFormat.colorspace = OCL_COLORSPACE_BT709;

    return;
}

void OclConverter::G2dBufferToOclBuffer(G2dBuffer &buff, OCL_BUFFER &oclBuf, OCL_FORMAT &oclFmt) {
    int ret = 0;
    OCL_FORMAT_PLANE_INFO plane_info;

    memset(&plane_info, 0, sizeof(plane_info));
    plane_info.ocl_format = &oclFmt;

    ret = mGetParam(sHandle, OCL_PARAM_INDEX_FORMAT_PLANE_INFO, &plane_info);
    if (ret) {
        ALOGE("%s: mGetParam(PLANE_INFO) failed, ret %d", __func__, ret);
        return;
    }

    oclBuf.mem_type = mOclBufferType;
    oclBuf.plane_num = plane_info.plane_num;
    int offset = 0;

    if (mOclBufferType == OCL_MEM_TYPE_GPU) {
        for (int i = 0; i < oclBuf.plane_num; i++) {
            oclBuf.planes[i].paddr = (long long)buff.infoPtr->phys + (long long)offset;
            oclBuf.planes[i].size = plane_info.plane_size[i];
            offset += oclBuf.planes[i].size;
        }
    } else if (mOclBufferType == OCL_MEM_TYPE_DEVICE) {
        for (int i = 0; i < oclBuf.plane_num; i++) {
            oclBuf.planes[i].fd = (long long)buff.infoPtr->fd;
            oclBuf.planes[i].offset = (long long)offset;
            oclBuf.planes[i].size = plane_info.plane_size[i];
            offset += oclBuf.planes[i].size;
        }
    } else {
        for (int i = 0; i < oclBuf.plane_num; i++) {
            oclBuf.planes[i].vaddr = (long long)buff.infoPtr->base + offset;
            oclBuf.planes[i].size = plane_info.plane_size[i];
            offset += oclBuf.planes[i].size;
        }
    }

    return;
}

int OclConverter::openclConvert(G2dBuffer &srcBuf, G2dBuffer &dstBuf) {
    int status = G2D_STATUS_OK;
    int ret = 0;

    if ((srcBuf.hnd == nullptr) || (srcBuf.infoPtr == nullptr) || (dstBuf.hnd == nullptr) ||
        (dstBuf.infoPtr == nullptr)) {
        ALOGE("%s: invalid input parameters", __func__);
        return G2D_STATUS_FAIL;
    }

    OCL_FORMAT inFormat;
    OCL_FORMAT outFormat;
    memset(&inFormat, 0, sizeof(inFormat));
    memset(&outFormat, 0, sizeof(outFormat));

    G2dBufferToOclFormat(srcBuf, inFormat);
    G2dBufferToOclFormat(dstBuf, outFormat);

    ret = mSetParam(sHandle, OCL_PARAM_INDEX_INPUT_FORMAT, &inFormat);
    if (ret) {
        ALOGE("%s: mSetParam INPUT_FORMAT failed, ret %d", __func__, ret);
        return G2D_STATUS_FAIL;
    }

    ret = mSetParam(sHandle, OCL_PARAM_INDEX_OUTPUT_FORMAT, &outFormat);
    if (ret) {
        ALOGE("%s: mSetParam OUTPUT_FORMAT failed, ret %d", __func__, ret);
        return G2D_STATUS_FAIL;
    }

    /* set buffer */
    OCL_BUFFER inBuffer;
    OCL_BUFFER outBuffer;
    memset(&inBuffer, 0, sizeof(inBuffer));
    memset(&outBuffer, 0, sizeof(outBuffer));

    G2dBufferToOclBuffer(srcBuf, inBuffer, inFormat);
    G2dBufferToOclBuffer(dstBuf, outBuffer, outFormat);

    if (OCL_SUCCESS != mConvert(sHandle, &inBuffer, &outBuffer)) {
        ALOGE("%s: OpenCL convert operation failed!", __func__);
        status = G2D_STATUS_FAIL;
    }

    return status;
}
} // namespace aidl::android::hardware::graphics::composer3::impl
