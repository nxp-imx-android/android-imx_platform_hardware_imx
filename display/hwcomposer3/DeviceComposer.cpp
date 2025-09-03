/*
 * Copyright 2017-2025 NXP.
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
#include "DeviceComposer.h"

#include <cutils/properties.h>
#include <dlfcn.h>
#include <drm_fourcc.h>
#include <hardware/gralloc.h>
#include <inttypes.h>
#include <libyuv.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>
#include <ui/Region.h>
#include <vndksupport/linker.h>
#include <xf86drm.h>

#include "Common.h"
#include "Drm.h"

#define GPUHELPER "libgpuhelper.so"
#define G2DENGINE "libg2d"

namespace aidl::android::hardware::graphics::composer3::impl {

#if defined(DEBUG_NXP_HWC_G2D)
#define DEBUG_LOG_G2D ALOGI
#else
#define DEBUG_LOG_G2D(...) ((void)0)
#endif

Mutex DeviceComposer::sLock(Mutex::PRIVATE);
thread_local void* DeviceComposer::sHandle(0);

static bool getDefaultG2DLib(char* libName, uint32_t size) {
    char value[PROPERTY_VALUE_MAX];

    if ((libName == NULL) || (size < strlen(G2DENGINE) + strlen(".so")))
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
    ALOGI("Default g2d lib: %s", libName);
    return true;
}

DeviceComposer::DeviceComposer() {
    mHelperHandle = NULL;
    mG2dHandle = NULL;

    char g2dlibName[PATH_MAX] = {0};

    mG2dPrefered = Is2DCompositionUserPrefered();
    if (mG2dPrefered) {
        ALOGI("%s: Prefer to use g2d/dpu 2D composition!", __FUNCTION__);
    } else {
        ALOGI("%s: Prefer to use Opengl ES 3D composition!", __FUNCTION__);
    }

    mHelperHandle = android_load_sphal_library(GPUHELPER, RTLD_LOCAL | RTLD_NOW);
    if (mHelperHandle == NULL) {
        ALOGE("fail to open libgpuhelper.so");
        mGetAlignedSize = NULL;
        mGetFlipOffset = NULL;
        mGetTiling = NULL;
        mAlterFormat = NULL;
        mLockSurface = NULL;
        mUnlockSurface = NULL;
        mAlignTile = NULL;
        mGetTileStatus = NULL;
        mResolveTileStatus = NULL;
    } else {
        mGetAlignedSize = (hwc_func3)dlsym(mHelperHandle, "hwc_getAlignedSize");
        mGetFlipOffset = (hwc_func2)dlsym(mHelperHandle, "hwc_getFlipOffset");
        mGetTiling = (hwc_func2)dlsym(mHelperHandle, "hwc_getTiling");
        mAlterFormat = (hwc_func2)dlsym(mHelperHandle, "hwc_alterFormat");
        mLockSurface = (hwc_func1)dlsym(mHelperHandle, "hwc_lockSurface");
        mUnlockSurface = (hwc_func1)dlsym(mHelperHandle, "hwc_unlockSurface");
        mAlignTile = (hwc_func4)dlsym(mHelperHandle, "hwc_align_tile");
        mGetTileStatus = (hwc_func2)dlsym(mHelperHandle, "hwc_get_tileStatus");
        mResolveTileStatus = (hwc_func1)dlsym(mHelperHandle, "hwc_resolve_tileStatus");
    }

    if (!Is2DCompositionUserDisabled() && getDefaultG2DLib(g2dlibName, PATH_MAX)) {
        mG2dHandle = android_load_sphal_library(g2dlibName, RTLD_LOCAL | RTLD_NOW);
    }

    if (mG2dHandle == NULL) {
        ALOGI("can't find %s or user disabled, 2D composition is invalid", g2dlibName);
        mSetClipping = NULL;
        mBlitFunction = NULL;
        mOpenEngine = NULL;
        mCloseEngine = NULL;
        mClearFunction = NULL;
        mEnableFunction = NULL;
        mDisableFunction = NULL;
        mFinishEngine = NULL;
        mQueryFeature = NULL;
        mBuffInfoFromFd = NULL;
        mCreateFenceFd = NULL;
    } else {
        ALOGI("load %s library successfully!", g2dlibName);
        mSetClipping = (hwc_func5)dlsym(mG2dHandle, "g2d_set_clipping");
        mBlitFunction = (hwc_func3)dlsym(mG2dHandle, "g2d_blitEx");
        if (mBlitFunction == NULL) {
            mBlitFunction = (hwc_func3)dlsym(mG2dHandle, "g2d_blit");
        }
        mOpenEngine = (hwc_func1)dlsym(mG2dHandle, "g2d_open");
        mCloseEngine = (hwc_func1)dlsym(mG2dHandle, "g2d_close");
        mClearFunction = (hwc_func2)dlsym(mG2dHandle, "g2d_clear");
        mEnableFunction = (hwc_func2)dlsym(mG2dHandle, "g2d_enable");
        mDisableFunction = (hwc_func2)dlsym(mG2dHandle, "g2d_disable");
        mFinishEngine = (hwc_func1)dlsym(mG2dHandle, "g2d_finish");
        mQueryFeature = (hwc_func3)dlsym(mG2dHandle, "g2d_query_feature");
        mBuffInfoFromFd = (hwc_buf_func)dlsym(mG2dHandle, "g2d_buf_from_fd");
        mCreateFenceFd = (hwc_func1)dlsym(mG2dHandle, "g2d_create_fence_fd");
    }

    mSolidColorBuffer.hnd = NULL;
    memset(&mSolidColorBuffer.info, 0, sizeof(mSolidColorBuffer.info));
    mOclCvt = std::make_unique<OclConverter>();
    if (mOclCvt->isValid())
        ALOGI("%s: OpenCL lib load successfully", __FUNCTION__);
}

DeviceComposer::~DeviceComposer() {
    if (mSolidColorBuffer.hnd != NULL) {
        unlockSurface(mSolidColorBuffer);
        ::android::GraphicBufferAllocator::get().free(mSolidColorBuffer.hnd);
    }
    if (mG2dHandle != NULL) {
        dlclose(mG2dHandle);
    }
    if (mHelperHandle != NULL) {
        dlclose(mHelperHandle);
    }
    if (sHandle != NULL) {
        closeEngine(sHandle);
    }
}

void* DeviceComposer::getHandle() {
    if (sHandle != NULL) {
        return sHandle;
    }

    if (mOpenEngine == NULL) {
        return NULL;
    }

    openEngine(&sHandle);
    return sHandle;
}

bool DeviceComposer::isValid() {
    return (getHandle() != NULL && mBlitFunction != NULL);
}

int DeviceComposer::prepareDeviceFrameBuffer(uint32_t width, uint32_t height, uint32_t format,
                                             std::vector<buffer_handle_t>& buffers, uint32_t count,
                                             bool secure) {
    uint64_t usage;
    uint32_t bufferStride;
    buffer_handle_t bufferHandle;

    usage = GRALLOC_USAGE_HW_FB | GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER |
            GRALLOC_USAGE_HW_2D;
    if (secure)
        usage |= GRALLOC_USAGE_PROTECTED;

    for (uint32_t i = 0; i < count; i++) {
        auto status = ::android::GraphicBufferAllocator::get().allocate(width, height,
                                                                        static_cast<int>(format),
                                                                        /*layerCount=*/1, usage,
                                                                        &bufferHandle,
                                                                        &bufferStride, "NxpHwc");
        if (status != ::android::OK) {
            ALOGE("%s: failed to allocate buffer:%d x %d, format=%x, usage=%lx, ret=%d",
                  __FUNCTION__, width, height, format, usage, status);
            return status;
        }

        buffers.push_back(bufferHandle);
    }

    return 0;
}

int DeviceComposer::freeDeviceFrameBuffer(std::vector<buffer_handle_t>& buffers) {
    for (auto buf : buffers) {
        ::android::GraphicBufferAllocator::get().free(buf);
    }

    return 0;
}

int DeviceComposer::prepareG2dTempBuffer(G2dBuffer& srcBuffer, uint32_t newFormat,
                                         G2dBuffer* tempBuffer) {
    if ((srcBuffer.hnd == NULL) || (tempBuffer == nullptr)) {
        return -1;
    }
    if (newFormat == static_cast<uint32_t>(common::PixelFormat::UNSPECIFIED))
        newFormat = srcBuffer.info.format;

    if ((tempBuffer->hnd != NULL) &&
        (srcBuffer.info.width == tempBuffer->info.width &&
         srcBuffer.info.height == tempBuffer->info.height &&
         newFormat == tempBuffer->info.format)) {
        return 0;
    }

    if (tempBuffer->hnd != NULL) {
        unlockSurface(*tempBuffer);
        ::android::GraphicBufferAllocator::get().free(tempBuffer->hnd);
        tempBuffer->hnd = NULL;
    }

    uint32_t bufferStride;
    buffer_handle_t bufferHandle;
    auto status =
            ::android::GraphicBufferAllocator::get().allocate(srcBuffer.info.width,
                                                              srcBuffer.info.height, newFormat, 1,
                                                              srcBuffer.info.usage, &bufferHandle,
                                                              &bufferStride, "HwcG2dTempBuffer");
    if (status != ::android::OK) {
        ALOGE("%s: failed to allocate g2d temporary buffer", __FUNCTION__);
        return -1;
    }

    if (getInfoFromHandle(bufferHandle, &(tempBuffer->info)) != 0) {
        ALOGE("%s: failed to get buffer info of g2d temporary buffer", __FUNCTION__);
        return -1;
    }
    tempBuffer->hnd = bufferHandle;
    lockSurface(*tempBuffer); // each temporary buffer will lockSurface() when allocate

    return 1;
}

int DeviceComposer::prepareSolidColorBuffer(G2dBuffer& target) {
    auto ret = prepareG2dTempBuffer(target, static_cast<uint32_t>(common::PixelFormat::UNSPECIFIED),
                                    &mSolidColorBuffer);
    if (ret == -1) {
        ALOGE("%s: fail to prepare g2d solidcolor buffer", __FUNCTION__);
        return ret;
    } else if (ret == 1) { // new allocated buffer
        common::Rect rect;
        rect.left = rect.top = 0;
        rect.right = static_cast<int>(mSolidColorBuffer.info.width);
        rect.bottom = static_cast<int>(mSolidColorBuffer.info.height);
        clearRect(mSolidColorBuffer, rect);
    }

    return 0;
}

int DeviceComposer::freeSolidColorBuffer() {
    if (mSolidColorBuffer.hnd != NULL) {
        unlockSurface(mSolidColorBuffer);
        ::android::GraphicBufferAllocator::get().free(mSolidColorBuffer.hnd);
        mSolidColorBuffer.hnd = NULL;
        memset(&mSolidColorBuffer.info, 0, sizeof(mSolidColorBuffer.info));
    }

    return 0;
}

int DeviceComposer::finishComposite() {
    finishEngine(getHandle());
    return 0;
}

int DeviceComposer::clearRect(G2dBuffer& buff, common::Rect& rect) {
    if (buff.hnd == NULL || isRectEmpty(rect)) {
        return 0;
    }

    struct g2d_surfaceEx surfaceX;
    struct g2d_surface& surface = surfaceX.base;

    memset(&surfaceX, 0, sizeof(surfaceX));
    setG2dSurface(surfaceX, buff, rect);
    surface.clrcolor = 0xff << 24;
    clearFunction(getHandle(), &surface);

    DEBUG_LOG_G2D("clearRect: rect(l:%d,t:%d,r:%d,b:%d)", rect.left, rect.top, rect.right,
                  rect.bottom);
    return 0;
}

int DeviceComposer::clearWormHole(std::vector<Layer*>& layers, G2dBuffer& target) {
    DEBUG_LOG("%s: clear worm hole", __FUNCTION__);
    if (target.hnd == NULL) {
        ALOGE("%s: no effective render buffer", __FUNCTION__);
        return -EINVAL;
    }

    // calculate opaque region.
    int i = 0;
    ::android::Region opaque;
    for (auto layer : layers) {
        auto mode = layer->getBlendMode();
        auto type = layer->getCompositionType();
        auto color = layer->getColor();
        if ((mode == common::BlendMode::NONE) ||
            (i == 0 && mode == common::BlendMode::PREMULTIPLIED) ||
            ((i != 0) && (type == Composition::SOLID_COLOR) &&
             (std::fabs(color.a - 1.0f) < 1e-9))) {
            for (auto& rect : layer->getVisibleRegion()) {
                opaque.orSelf(::android::Rect(rect.left, rect.top, rect.right, rect.bottom));
            }
        }
        i++;
    }

    // calculate worm hole.
    ::android::Region screen(::android::Rect(target.info.width, target.info.height));
    screen.subtractSelf(opaque);
    const ::android::Rect* holes = NULL;
    size_t numRect = 0;
    holes = screen.getArray(&numRect);
    // clear worm hole.
    struct g2d_surfaceEx surfaceX;
    memset(&surfaceX, 0, sizeof(surfaceX));
    struct g2d_surface& surface = surfaceX.base;
    DEBUG_LOG_G2D("%s: clear %zu worm holes", __FUNCTION__, numRect);
    int clrcolor = 0x00 << 24; // make alpha be 0(transparent) for DRM_FORMAT_ABGR8888 like format.
    for (size_t i = 0; i < numRect; i++) {
        if (holes[i].isEmpty()) {
            continue;
        }

        common::Rect rect;
        rect.left = holes[i].left;
        rect.top = holes[i].top;
        rect.right = holes[i].right;
        rect.bottom = holes[i].bottom;
        DEBUG_LOG_G2D("clearhole: hole(l:%d,t:%d,r:%d,b:%d)", rect.left, rect.top, rect.right,
                      rect.bottom);
        setG2dSurface(surfaceX, target, rect);
        surface.clrcolor = clrcolor;
        clearFunction(getHandle(), &surface);
    }

    return 0;
}

int DeviceComposer::onLayerDestroy(Layer* layer) {
    auto id = layer->getId();
    if (mG2dCachedBuffers.find(id) != mG2dCachedBuffers.end()) {
        ::android::GraphicBufferAllocator::get().free(mG2dCachedBuffers[id].hnd);
        mG2dCachedBuffers.erase(id);
    }

    return 0;
}

int DeviceComposer::convertBuffer(buffer_handle_t inBuf, HandleInfo& inInfo, buffer_handle_t outBuf,
                                  HandleInfo& outInfo, bool useOcl) {
#ifdef DEBUG_DUMP_VIRT_G2D_CONSUMPTION
    nsecs_t g2dStart = systemTime(CLOCK_MONOTONIC);
#endif
    G2dBuffer srcBuff{inBuf, inInfo};
    G2dBuffer dstBuff{outBuf, outInfo};
    common::Rect rect{0, 0, static_cast<int32_t>(inInfo.width),
                      static_cast<int32_t>(inInfo.height)};
    if (useOcl && mOclCvt->isValid()) {
        auto ret = mOclCvt->openclConvert(srcBuff, dstBuff);
        if (ret) {
            ALOGE("%s: OpenCL CSC convert fail", __FUNCTION__);
            return ret;
        }
    } else {
        struct g2d_surfaceEx sSurfaceX, dSurfaceX;
        memset(&sSurfaceX, 0, sizeof(sSurfaceX));
        memset(&dSurfaceX, 0, sizeof(dSurfaceX));
        setG2dSurface(sSurfaceX, srcBuff, rect);
        setG2dSurface(dSurfaceX, dstBuff, rect);
        blitSurface(&sSurfaceX, &dSurfaceX);
    }

#ifdef DEBUG_DUMP_VIRT_G2D_CONSUMPTION
    nsecs_t g2dEnd = systemTime(CLOCK_MONOTONIC);
    char fmt1[6], fmt2[6];
    char* fmt_name1 = drmGetFormatName(inInfo.drm_format, fmt1);
    char* fmt_name2 = drmGetFormatName(outInfo.drm_format, fmt2);
    char* modifier_name1 = drmGetFormatModifierName(inInfo.modifier);
    char* modifier_name2 = drmGetFormatModifierName(outInfo.modifier);
    ALOGI("%s: covert buffer(%s:%s -> %s:%s) cost %3.3fms", __func__, fmt_name1, modifier_name1,
          fmt_name2, modifier_name2, (g2dEnd - g2dStart) / 1000000.0);
    free(modifier_name1);
    free(modifier_name2);
#endif
    return 0;
}

G2dInterBuffer* DeviceComposer::preComposition(Layer* layer, buffer_handle_t handle) {
#if !defined(PXP_LIMITATION_DOWN_SCALE) && !defined(G2D_FORMAT_CONVERSION)
    return nullptr;
#else
    auto id = layer->getId();
    auto type = layer->getCompositionType();

    if ((type != Composition::DEVICE) || (handle == nullptr))
        return nullptr;

    HandleInfo inBufInfo;
    if (getInfoFromHandle(handle, &inBufInfo) != 0)
        return nullptr;

    uint32_t dstW = 0, dstH = 0, dstFormat = 0, dstUsage = 0;
    int cachedBufferType = G2D_CACHE_TYPE_NONE;
    bool opencl_prefered = false;
    common::Rect drect = layer->getDisplayFrame();
    uint32_t dispW = (drect.right - drect.left);
    uint32_t dispH = (drect.bottom - drect.top);
    common::Rect srect = layer->getSourceCropInt();
    uint32_t cropW = srect.right - srect.left;
    uint32_t cropH = srect.bottom - srect.top;
#if defined(PXP_LIMITATION_DOWN_SCALE)
    if ((cropW > dispW) || (cropH > dispH)) {
        DEBUG_LOG_G2D("%s: layer %" PRId64 " scaling(src: %d x %d to dst: %d x %d)", __FUNCTION__,
                      id, cropW, cropH, dispW, dispH);
        dstW = dispW;
        dstH = dispH;
        dstFormat = inBufInfo.format;
        dstUsage = inBufInfo.usage;
        cachedBufferType = G2D_CACHE_TYPE_SCALING;
    } else
#elif defined(G2D_FORMAT_CONVERSION)
    if (inBufInfo.format == HAL_PIXEL_FORMAT_P010_TILED) {
        /* HAL_PIXEL_FORMAT_P010_TILED is packed 10bit NV12(NV15) with DRM_FORMAT_MOD_AMPHION_TILED
         * modifier, need to convert to linear 8bit NV12 format. So DPU can process it.
         */
        DEBUG_LOG_G2D("%s: layer %" PRId64 " format conversion(0x%x --> 0x%x)", __FUNCTION__, id,
                      inBufInfo.format, HAL_PIXEL_FORMAT_YCbCr_420_SP);
        dstW = inBufInfo.width;
        dstH = inBufInfo.height;
        dstFormat = HAL_PIXEL_FORMAT_YCbCr_420_SP;
        dstUsage = inBufInfo.usage;
        cachedBufferType = G2D_CACHE_TYPE_CSC;
        opencl_prefered = true;
    } else
#endif
    {
        return nullptr;
    }

    bool reuseBuff = true;
    buffer_handle_t outHandle;
    HandleInfo outBufInfo;
    if (mG2dCachedBuffers.find(id) != mG2dCachedBuffers.end()) {
        auto interBuf = mG2dCachedBuffers[id];
        if (interBuf.originBufferId == inBufInfo.buffer_id) {
            return &(mG2dCachedBuffers[id]);
        } else if ((dstW == interBuf.info.width) && (dstH == interBuf.info.height) &&
                   (dstFormat == interBuf.info.format) && (dstUsage == interBuf.info.usage)) {
            reuseBuff = true;
            outHandle = interBuf.hnd;
            outBufInfo = interBuf.info;
        } else {
            reuseBuff = false;
            mG2dCachedBuffers.erase(id);
            ::android::GraphicBufferAllocator::get().free(interBuf.hnd);
        }
    } else {
        reuseBuff = false;
    }

    DEBUG_LOG_G2D("%s:layer id=%" PRId64 ", %s buffer:%d x %d, format=0x%x, usage=0x%x",
                  __FUNCTION__, id, reuseBuff ? "reuse" : "new", dstW, dstH, dstFormat, dstUsage);
    if (!reuseBuff) {
        uint32_t outStride;
        auto status =
                ::android::GraphicBufferAllocator::get().allocate(dstW, dstH, dstFormat, 1,
                                                                  dstUsage, &outHandle, &outStride,
                                                                  "HwcG2dCacheBuffer");
        if (status != ::android::OK) {
            ALOGE("%s: failed to allocate g2d cache buffer", __FUNCTION__);
            return nullptr;
        }
        if (getInfoFromHandle(outHandle, &outBufInfo) != 0)
            return nullptr;
    }

    if (cachedBufferType == G2D_CACHE_TYPE_SCALING) {
        lockBuffer(outHandle, outBufInfo);
        lockBuffer(handle, inBufInfo);
    }

    if (cachedBufferType == G2D_CACHE_TYPE_SCALING) {
        common::PixelFormat format = static_cast<common::PixelFormat>(inBufInfo.format);
        if ((format == common::PixelFormat::RGBA_8888) ||
            (format == common::PixelFormat::RGBX_8888) ||
            (format == common::PixelFormat::BGRA_8888)) {
            // Point to the upper left corner of the crop rectangles
            uint8_t* srcBuffer = (uint8_t*)(inBufInfo.base + srect.top * inBufInfo.strides[0] +
                                            srect.left * 4); // 4 bytes per pexil
            uint8_t* dstBuffer = (uint8_t*)(outBufInfo.base);
            const int srcStrideBytes = static_cast<int>(inBufInfo.strides[0]);
            const int dstStrideBytes = static_cast<int>(outBufInfo.strides[0]);

            auto result = libyuv::ARGBScale(srcBuffer, srcStrideBytes, cropW, cropH, dstBuffer,
                                            dstStrideBytes, dstW, dstH, libyuv::kFilterNone);
            if (result)
                ALOGE("%s: libyuv ARGBScale(%dx%d -> %dx%d) operation fail", __FUNCTION__, cropW,
                      cropH, dstW, dstH);
        } else if (format == common::PixelFormat::YV12) {
            uint8_t* srcY = (uint8_t*)(inBufInfo.base);
            uint8_t* srcU = srcY + inBufInfo.offsets[1];
            uint8_t* srcV = srcY + inBufInfo.offsets[2];
            uint8_t* dstY = (uint8_t*)(outBufInfo.base);
            uint8_t* dstU = dstY + outBufInfo.offsets[1];
            uint8_t* dstV = dstY + outBufInfo.offsets[2];
            int srcWidth = inBufInfo.width;
            int srcHeight = inBufInfo.height;
            int dstWidth = outBufInfo.width;
            int dstHeight = outBufInfo.height;

            auto result = libyuv::I420Scale(srcY, inBufInfo.strides[0], srcU, inBufInfo.strides[1],
                                            srcV, inBufInfo.strides[2], srcWidth, srcHeight, dstY,
                                            outBufInfo.strides[0], dstU, outBufInfo.strides[1],
                                            dstV, outBufInfo.strides[2], dstWidth, dstHeight,
                                            libyuv::kFilterNone);
            if (result)
                ALOGE("%s: libyuv I420Scale(%dx%d -> %dx%d) operation fail", __FUNCTION__, cropW,
                      cropH, dstW, dstH);
        }
    } else if (cachedBufferType == G2D_CACHE_TYPE_CSC) {
        convertBuffer(handle, inBufInfo, outHandle, outBufInfo, opencl_prefered);
    }

    if (!reuseBuff) {
        G2dInterBuffer newBuff{id, cachedBufferType, outHandle, outBufInfo, inBufInfo.buffer_id};
        mG2dCachedBuffers.emplace(id, newBuff);
    }

    if (cachedBufferType == G2D_CACHE_TYPE_SCALING) {
        unlockBuffer(handle, inBufInfo);
        unlockBuffer(outHandle, outBufInfo);
    }

    return &(mG2dCachedBuffers[id]);
#endif
}

int DeviceComposer::composeLayerLocked(Layer* layer, G2dBuffer& layerBuffer,
                                       G2dBuffer& targetBuffer, bool bypass) {
    DEBUG_LOG("%s: compose layer %ld", __FUNCTION__, layer->getId());
    if (layer == NULL || targetBuffer.hnd == NULL) {
        ALOGE("%s: invalid layer or target", __FUNCTION__);
        return -EINVAL;
    }

    auto type = layer->getCompositionType();
    auto mode = layer->getBlendMode();
    auto transform = layer->getTransform();
    auto alpha = (uint8_t)(layer->getPlaneAlpha() * 255);

    common::Rect srect = layer->getSourceCropInt();
    common::Rect drect = layer->getDisplayFrame();
    struct g2d_surfaceEx dSurfaceX;
    struct g2d_surface& dSurface = dSurfaceX.base;

    if (layerBuffer.hnd != nullptr) {
        DEBUG_LOG_G2D("%s: compose layer id=%ld, %d x %d, zorder:0x%x, phys:0x%" PRIx64
                      ", transform:%s, blend:%s, alpha:0x%x, name=%s",
                      __FUNCTION__, layer->getId(), layerBuffer.info.width, layerBuffer.info.height,
                      layer->getZOrder(), layerBuffer.info.phys, toString(transform).c_str(),
                      toString(mode).c_str(), alpha, layerBuffer.info.name);
    } else {
        DEBUG_LOG_G2D("%s: compose layer id=%ld, zorder:0x%x, transform:%s, blend:%s, "
                      "alpha:0x%x, solid color layer",
                      __FUNCTION__, layer->getId(), layer->getZOrder(), toString(transform).c_str(),
                      toString(mode).c_str(), alpha);
    }

    if ((isRectEmpty(srect) && !(type == Composition::SOLID_COLOR)) || isRectEmpty(drect)) {
        ALOGE("%s: invalid srect or drect", __FUNCTION__);
        return 0;
    }
    if (alpha == 0) {
        DEBUG_LOG_G2D("%s: global_alpha is 0(transparent), skip such layer", __FUNCTION__);
        return 0;
    }

    if (type == Composition::SOLID_COLOR) {
        prepareSolidColorBuffer(targetBuffer);
    }

    memset(&dSurfaceX, 0, sizeof(dSurfaceX));
    setG2dSurface(dSurfaceX, targetBuffer, drect);

    bool needDither = false;
    std::vector<common::Rect>& visible = layer->getVisibleRegion();
    for (auto& clip : visible) {
        if (isRectEmpty(clip)) {
            DEBUG_LOG_G2D("%s: invalid clip", __FUNCTION__);
            continue;
        }

        if (!rectIntersect(drect, clip)) {
            DEBUG_LOG_G2D("%s: invalid clip rect", __FUNCTION__);
            continue;
        }
        setClipping(srect, drect, clip, transform);
        DEBUG_LOG_G2D("layer:%ld, sourceCrop(l:%d,t:%d,r:%d,b:%d), visible(l:%d,t:%d,r:%d,b:%d), "
                      "display(l:%d,t:%d,r:%d,b:%d)",
                      layer->getId(), srect.left, srect.top, srect.right, srect.bottom, clip.left,
                      clip.top, clip.right, clip.bottom, drect.left, drect.top, drect.right,
                      drect.bottom);

        struct g2d_surfaceEx sSurfaceX;
        memset(&sSurfaceX, 0, sizeof(sSurfaceX));
        struct g2d_surface& sSurface = sSurfaceX.base;

        if (!(type == Composition::SOLID_COLOR) && layerBuffer.hnd) {
            if ((layerBuffer.interPtr != nullptr) &&
                (layerBuffer.interPtr->type == G2D_CACHE_TYPE_SCALING))
                setG2dSurface(sSurfaceX, layerBuffer, drect);
            else
                setG2dSurface(sSurfaceX, layerBuffer, srect);
#ifndef G2D_LIMITATION_PXP // PXP G2D don't support DITHER
            if ((targetBuffer.info.format == static_cast<uint32_t>(common::PixelFormat::RGB_565)) &&
                (layerBuffer.info.format == static_cast<uint32_t>(common::PixelFormat::RGBA_8888) ||
                 layerBuffer.info.format == static_cast<uint32_t>(common::PixelFormat::RGBX_8888) ||
                 layerBuffer.info.format ==
                         static_cast<uint32_t>(common::PixelFormat::BGRA_8888))) {
                needDither = true;
            }
#endif
        } else if (mSolidColorBuffer.hnd) {
            setG2dSurface(sSurfaceX, mSolidColorBuffer, drect);
#ifndef G2D_LIMITATION_PXP
            sSurface.clrcolor = 0xff << 24;
#endif
        } else {
            return -EINVAL;
        }

        convertRotation(transform, sSurface, dSurface);
        if (!bypass)
            convertBlending(mode, sSurface, dSurface);

        sSurface.global_alpha = alpha;

        if ((mode != common::BlendMode::NONE) && !bypass) {
            enableFunction(getHandle(), G2D_GLOBAL_ALPHA, true);
#ifndef G2D_LIMITATION_PXP
            if (type == Composition::SOLID_COLOR)
                enableFunction(getHandle(), G2D_BLEND_DIM, true);
            else
#endif
                enableFunction(getHandle(), G2D_BLEND, true);
        }

        if (needDither)
            enableFunction(getHandle(), G2D_DITHER, true);

        blitSurface(&sSurfaceX, &dSurfaceX);

        if (needDither)
            enableFunction(getHandle(), G2D_DITHER, false);

        if ((mode != common::BlendMode::NONE) && !bypass) {
#ifndef G2D_LIMITATION_PXP
            if (type == Composition::SOLID_COLOR)
                enableFunction(getHandle(), G2D_BLEND_DIM, false);
            else
#endif
                enableFunction(getHandle(), G2D_BLEND, false);
            enableFunction(getHandle(), G2D_GLOBAL_ALPHA, false);
        }
    }

    return 0;
}

int DeviceComposer::setG2dSurface(struct g2d_surfaceEx& surfaceX, G2dBuffer& buff,
                                  common::Rect& rect) {
    struct g2d_surface& surface = surfaceX.base;
    if (buff.hnd == NULL) {
        ALOGE("%s: handle is invalid!", __FUNCTION__);
        return -1;
    }

    surface.format = convertFormat(buff.info.drm_format, buff);
    enum g2d_tiling tile = G2D_LINEAR;
    getTiling(buff, &tile);
#ifdef G2D_FORMAT_CONVERSION
    if (buff.info.drm_format == DRM_FORMAT_NV15 &&
        buff.info.modifier == DRM_FORMAT_MOD_AMPHION_TILED) {
        surfaceX.tiling = G2D_AMPHION_TILED_10BIT;
    } else
#endif
            if (buff.info.modifier == DRM_FORMAT_MOD_AMPHION_TILED) {
        surfaceX.tiling = G2D_AMPHION_TILED;
    } else {
        surfaceX.tiling = tile;
    }

    if (isFeatureSupported(G2D_FAST_CLEAR)) {
        getTileStatus(buff, &surfaceX);
    } else {
        resolveTileStatus(buff);
    }

    uint64_t phys = 0;
    uint32_t offset = 0;
    if (buff.info.phys)
        phys = buff.info.phys;
    else
        getBuffPhys(buff, &phys);

    getFlipOffset(buff, &offset);
    surface.planes[0] = static_cast<g2d_phys_addr_t>(phys + offset);

    switch (surface.format) {
        case G2D_GRAY8:
            surface.stride = static_cast<int>(buff.info.strides[0]); // convert to pixel stride
            break;
        case G2D_RGB565:
        case G2D_YUYV:
            surface.stride = static_cast<int>(buff.info.strides[0] / 2); // convert to pixel stride
            break;
        case G2D_RGBA8888:
        case G2D_BGRA8888:
        case G2D_RGBX8888:
        case G2D_BGRX8888:
        case G2D_RGBA1010102:
            surface.stride = static_cast<int>(buff.info.strides[0] / 4); // convert to pixel stride
            break;

        case G2D_NV16:
        case G2D_NV12:
        case G2D_NV21:
            surface.stride = static_cast<int>(buff.info.strides[0]);
            surface.planes[1] = surface.planes[0] + buff.info.offsets[1];
            break;

        case G2D_I420:
        case G2D_YV12: {
            surface.stride = static_cast<int>(buff.info.strides[0]);
            surface.planes[1] = surface.planes[0] + buff.info.offsets[1];
            surface.planes[2] = surface.planes[0] + buff.info.offsets[2];
        } break;

        default:
            ALOGE("%s: does not support format:%d", __FUNCTION__, surface.format);
            break;
    }
    int buff_width = static_cast<int>(buff.info.width);
    int buff_height = static_cast<int>(buff.info.height);
    surface.left = rect.left < buff_width ? rect.left : buff_width;
    surface.top = rect.top < buff_height ? rect.top : buff_height;
    surface.right = rect.right < buff_width ? rect.right : buff_width;
    surface.bottom = rect.bottom < buff_height ? rect.bottom : buff_height;
    surface.width = buff_width;
    surface.height = buff_height;

    DEBUG_LOG_G2D("%s: dimension(%d,%d,%d,%d, %d x %d), format=%d, stride=%d, tiling=%d, "
                  "plane0=0x%" PRIx64 ", plane1=0x%" PRIx64 ", plane2=0x%" PRIx64,
                  __FUNCTION__, surface.left, surface.top, surface.right, surface.bottom,
                  surface.width, surface.height, surface.format, surface.stride, surfaceX.tiling,
                  surface.planes[0], surface.planes[1], surface.planes[2]);

    return 0;
}

enum g2d_format DeviceComposer::convertFormat(uint32_t format, G2dBuffer& buff) {
    enum g2d_format halFormat;
    switch (format) {
        case DRM_FORMAT_ABGR2101010:
            halFormat = G2D_RGBA1010102;
            break;
        case DRM_FORMAT_ABGR8888:
#ifdef FORMAT_WORKAROUND_FOR_PXP
            halFormat = G2D_BGRA8888;
#else
            halFormat = G2D_RGBA8888;
#endif
            break;
        case DRM_FORMAT_XBGR8888:
#ifdef FORMAT_WORKAROUND_FOR_PXP
            halFormat = G2D_BGRX8888;
#else
            halFormat = G2D_RGBX8888;
#endif
            break;
        case DRM_FORMAT_RGB565:
            halFormat = G2D_RGB565;
            break;
        case DRM_FORMAT_ARGB8888:
            halFormat = G2D_BGRA8888;
            break;
        case DRM_FORMAT_NV21:
            halFormat = G2D_NV21;
            break;
        case DRM_FORMAT_NV12:
#ifdef FORMAT_WORKAROUND_FOR_PXP
            halFormat = G2D_NV21;
#else
            halFormat = G2D_NV12;
#endif
            break;
        case DRM_FORMAT_YUV420:
#ifdef FORMAT_WORKAROUND_FOR_PXP
            halFormat = G2D_YV12;
#else
            halFormat = G2D_I420;
#endif
            break;
        case DRM_FORMAT_YVU420_ANDROID:
        case DRM_FORMAT_YVU420:
#ifdef FORMAT_WORKAROUND_FOR_PXP
            halFormat = G2D_I420;
#else
            halFormat = G2D_YV12;
#endif
            break;
        case DRM_FORMAT_NV16:
            halFormat = G2D_NV16;
            break;
        case DRM_FORMAT_YUYV:
            halFormat = G2D_YUYV;
            break;
#ifdef G2D_FORMAT_CONVERSION
        case DRM_FORMAT_NV15:
            halFormat = G2D_NV12;
            break;
#endif
        case DRM_FORMAT_R8:
            halFormat = G2D_GRAY8;
            break;
        default:
            ALOGE("%s: unsupported format:0x%x", __FUNCTION__, format);
            halFormat = G2D_RGBA8888;
            break;
    }

    halFormat = alterFormat(buff, halFormat);
    return halFormat;
}

int DeviceComposer::convertRotation(common::Transform transform, struct g2d_surface& src,
                                    struct g2d_surface& dst) {
    switch (transform) {
        case common::Transform::NONE:
            dst.rot = G2D_ROTATION_0;
            break;
        case common::Transform::ROT_90:
            dst.rot = G2D_ROTATION_90;
            break;
        case common::Transform::ROT_180:
            dst.rot = G2D_ROTATION_180;
            break;
        case common::Transform::ROT_270:
            dst.rot = G2D_ROTATION_270;
            break;
        case common::Transform::FLIP_H:
            dst.rot = G2D_FLIP_H;
            break;
        case common::Transform::FLIP_V:
            dst.rot = G2D_FLIP_V;
            break;
        default:
            dst.rot = G2D_ROTATION_0;
            break;
    }

    return 0;
}

int DeviceComposer::convertBlending(common::BlendMode blending, struct g2d_surface& src,
                                    struct g2d_surface& dst) {
    switch (blending) {
        case common::BlendMode::PREMULTIPLIED:
            src.blendfunc = G2D_ONE;
            dst.blendfunc = G2D_ONE_MINUS_SRC_ALPHA;
            break;

        case common::BlendMode::COVERAGE:
            src.blendfunc = G2D_SRC_ALPHA;
            dst.blendfunc = G2D_ONE_MINUS_SRC_ALPHA;
            break;

        default:
            src.blendfunc = G2D_ONE;
            dst.blendfunc = G2D_ONE_MINUS_SRC_ALPHA;
            break;
    }

    return 0;
}

//-----------------------Start of API wrappers for libgpuhelper.so------------------------------
inline int checkGpuHelperLimitation(G2dBuffer& buff) {
#ifdef G2D_LIMITATION_DPU
    // Don't call gpuhelper APIs for P010_TILED and NV12_TILED
    if (buff.info.format == HAL_PIXEL_FORMAT_P010_TILED ||
        buff.info.format == HAL_PIXEL_FORMAT_NV12_TILED)
        return -1;
    else
        return 0;
#else
    return 0;
#endif
}

int DeviceComposer::getAlignedSize(G2dBuffer& buff, int* width, int* height) {
    if (mGetAlignedSize == NULL || checkGpuHelperLimitation(buff) != 0) {
        return -EINVAL;
    }

    return (*mGetAlignedSize)((void*)buff.hnd, (void*)width, (void*)height);
}

int DeviceComposer::getFlipOffset(G2dBuffer& buff, uint32_t* offset) {
    if (mGetFlipOffset == NULL || checkGpuHelperLimitation(buff) != 0) {
        return -EINVAL;
    }

    return (*mGetFlipOffset)((void*)buff.hnd, (void*)offset);
}

int DeviceComposer::getTiling(G2dBuffer& buff, enum g2d_tiling* tile) {
    if (mGetTiling == NULL || checkGpuHelperLimitation(buff) != 0) {
        return -EINVAL;
    }

    return (*mGetTiling)((void*)buff.hnd, (void*)tile);
}

enum g2d_format DeviceComposer::alterFormat(G2dBuffer& buff, enum g2d_format format) {
    if (mAlterFormat == NULL || checkGpuHelperLimitation(buff) != 0) {
        return format;
    }

    return (enum g2d_format)(*mAlterFormat)((void*)buff.hnd, (void*)format);
}

int DeviceComposer::alignTile(int* width, int* height, int format, int usage) {
    if (mAlignTile == NULL) {
        return -EINVAL;
    }
    return (*mAlignTile)(width, height, (void*)(intptr_t)format, (void*)(intptr_t)usage);
}

int DeviceComposer::getTileStatus(G2dBuffer& buff, struct g2d_surfaceEx* surfaceX) {
    if (mGetTileStatus == NULL || checkGpuHelperLimitation(buff) != 0) {
        return -EINVAL;
    }

    return (*mGetTileStatus)((void*)buff.hnd, surfaceX);
}

int DeviceComposer::resolveTileStatus(G2dBuffer& buff) {
    if (mResolveTileStatus == NULL || checkGpuHelperLimitation(buff) != 0) {
        return -EINVAL;
    }

    return (*mResolveTileStatus)((void*)buff.hnd);
}

int DeviceComposer::lockSurface(G2dBuffer& buff) {
    if (mLockSurface == NULL || checkGpuHelperLimitation(buff) != 0) {
        return -EINVAL;
    }

#ifndef G2D_LIMITATION_VIV
    buff.originPhys = buff.info.phys;
#endif
    int ret = (*mLockSurface)((void*)buff.hnd);
#ifdef G2D_LIMITATION_VIV
    // The phys in handle will change after lockSurface() if GPU disable flat-mapping, update info
    getPhysFromHandle(buff.hnd, &buff.info.phys);
#endif

    return ret;
}

int DeviceComposer::unlockSurface(G2dBuffer& buff) {
    if (mUnlockSurface == NULL || checkGpuHelperLimitation(buff) != 0) {
        return -EINVAL;
    }

    int ret = (*mUnlockSurface)((void*)buff.hnd);
#ifndef G2D_LIMITATION_VIV
    // Restore the original phys to handle after unlockSurface(), so the phys are correct for next
    //  frame composition when use DPU or PXP.
    setPhysToHandle(buff.hnd, buff.originPhys);
#endif

    return ret;
}
//-----------------------End of API wrappers for libgpuhelper.so--------------------------------

int DeviceComposer::setClipping(common::Rect& /*src*/, common::Rect& /*dst*/, common::Rect& clip,
                                common::Transform /*rotation*/) {
    if (mSetClipping == NULL) {
        return -EINVAL;
    }

    return (*mSetClipping)(getHandle(), (void*)(intptr_t)clip.left, (void*)(intptr_t)clip.top,
                           (void*)(intptr_t)clip.right, (void*)(intptr_t)clip.bottom);
}

int DeviceComposer::blitSurface(struct g2d_surfaceEx* srcEx, struct g2d_surfaceEx* dstEx) {
    if (mBlitFunction == NULL) {
        return -EINVAL;
    }

    return (*mBlitFunction)(getHandle(), srcEx, dstEx);
}

int DeviceComposer::openEngine(void** handle) {
    if (mOpenEngine == NULL) {
        return -EINVAL;
    }

    return (*mOpenEngine)((void*)handle);
}

int DeviceComposer::closeEngine(void* handle) {
    if (mCloseEngine == NULL) {
        return -EINVAL;
    }

    return (*mCloseEngine)((void*)handle);
}

int DeviceComposer::clearFunction(void* handle, struct g2d_surface* area) {
    if (mClearFunction == NULL) {
        return -EINVAL;
    }

    return (*mClearFunction)((void*)handle, area);
}

int DeviceComposer::enableFunction(void* handle, enum g2d_cap_mode cap, bool enable) {
    if (mEnableFunction == NULL || mDisableFunction == NULL) {
        return -EINVAL;
    }

    int ret = 0;
    if (enable) {
        ret = (*mEnableFunction)((void*)handle, (void*)cap);
    } else {
        ret = (*mDisableFunction)((void*)handle, (void*)cap);
    }

    return ret;
}

int DeviceComposer::finishEngine(void* handle) {
    if (mFinishEngine == NULL) {
        return -EINVAL;
    }

    return (*mFinishEngine)((void*)handle);
}

bool DeviceComposer::isFeatureSupported(g2d_feature feature) {
    if (mQueryFeature == NULL || getHandle() == NULL) {
        return false;
    }

    int enable = 0;
    (*mQueryFeature)(getHandle(), (void*)feature, (void*)&enable);
    return (enable != 0);
}

int DeviceComposer::getBuffPhys(G2dBuffer& buff, uint64_t* phys) {
    if (mBuffInfoFromFd == NULL) {
        return -EINVAL;
    }

    if (buff.hnd == NULL) {
        ALOGE("%s: handle is invalid!", __FUNCTION__);
        return -EINVAL;
    }

    struct g2d_buf* buf = (struct g2d_buf*)(*mBuffInfoFromFd)((void*)(intptr_t)buff.info.fd);
    if (buf && buf->buf_paddr)
        *phys = static_cast<uint64_t>(buf->buf_paddr);

    if (buf) {
        free(buf->buf_handle);
        free(buf);
    }

    return 0;
}

int DeviceComposer::createFenceFd(void* handle) {
    if (mCreateFenceFd == NULL) {
        return -1;
    }

    return (*mCreateFenceFd)((void*)handle);
}

bool DeviceComposer::checkMustDeviceComposition(Layer* layer) {
    DEBUG_LOG("%s: check layer %ld", __FUNCTION__, layer->getId());

    auto layerBuffer = layer->getBuffer().getBuffer();
    HandleInfo info;
    if (layerBuffer == NULL || (getInfoFromHandle(layerBuffer, &info) != 0)) {
        return false;
    }

    // vpu tile format must be handled by device.
    if (layerBuffer != nullptr &&
        (info.modifier == DRM_FORMAT_MOD_AMPHION_TILED || info.usage & GRALLOC_USAGE_PROTECTED ||
         info.drm_format == DRM_FORMAT_R8)) {
        DEBUG_LOG("%s: 2d composition is must", __FUNCTION__);
        return true;
    }

    return false;
}

bool DeviceComposer::checkDeviceComposition(Layer* layer) {
    DEBUG_LOG("%s: check layer %ld", __FUNCTION__, layer->getId());

    auto layerBuffer = layer->getBuffer().getBuffer();
    HandleInfo info;
    if (layerBuffer == NULL) { // support device composition for SOLID_COLOR layer
        return true;
    } else if (getInfoFromHandle(layerBuffer, &info) != 0) {
        ALOGE("%s: fail to get buffer infomation", __FUNCTION__);
        return false;
    }

#ifndef G2D_LIMITATION_PXP
    if (layer->getCompositionType() == Composition::CLIENT) {
        DEBUG_LOG("%s: Not process type=CLIENT layer", __FUNCTION__);
        return false;
    }

    if (layer->getColorTransform() != std::nullopt) {
        DEBUG_LOG("%s: g2d can't support color transform", __FUNCTION__);
        return false;
    }

    bool rotationCap = isFeatureSupported(G2D_ROTATION);
    // rotation case skip device composition.
    if ((layer->getTransform() != common::Transform::NONE) && !rotationCap) {
        DEBUG_LOG("%s: g2d can't support rotation", __FUNCTION__);
        return false;
    }
#endif
#ifdef G2D_LIMITATION_VIV
    if (info.drm_format == DRM_FORMAT_ABGR2101010) {
        DEBUG_LOG("%s: g2d can't support ABGR2101010 format", __FUNCTION__);
        return false;
    }

    common::Dataspace dataspace = layer->getDataspace();
    // video nv12 full range should be handled by client
    if (layerBuffer != nullptr && info.drm_format == DRM_FORMAT_NV12 &&
        ((common::Dataspace)((int)dataspace & (int)common::Dataspace::RANGE_MASK) ==
         common::Dataspace::RANGE_FULL)) {
        DEBUG_LOG("%s: g2d can't support video nv12 full range", __FUNCTION__);
        return false;
    }
#endif

#ifdef G2D_LIMITATION_DPU
    if ((info.drm_format == DRM_FORMAT_P010) || (info.drm_format == DRM_FORMAT_P210)) {
        DEBUG_LOG("%s: g2d can't support 0x%x format", __FUNCTION__, info.drm_format);
        return false;
    }
#endif
    if (!(info.usage &
          (GRALLOC_USAGE_PROTECTED | GRALLOC_USAGE_PRIVATE_3 | GRALLOC_USAGE_HW_COMPOSER |
           GRALLOC_USAGE_HW_FB))) {
        ALOGI("%s: g2d can't support the buffer from system/system-uncached heap", __FUNCTION__);
        return false;
    }

    return true;
}

std::tuple<bool, ::android::base::unique_fd> DeviceComposer::composeLayers(
        std::vector<Layer*> layers, buffer_handle_t target) {
    DEBUG_LOG("%s: ------%zu layers compose to target-------", __FUNCTION__, layers.size());
    ATRACE_CALL();

    Mutex::Autolock _l(sLock);
    if (!target || (getInfoFromHandle(target, &mTarget.info) != 0)) {
        ALOGE("%s: composer target buffer is invalid", __FUNCTION__);
        return std::make_tuple(false, ::android::base::unique_fd());
    }
    mTarget.hnd = target;
    DEBUG_LOG_G2D("%s: --------target(fd=%d, %d x %d)--------", __FUNCTION__, mTarget.info.fd,
                  mTarget.info.width, mTarget.info.height);

    lockSurface(mTarget);
    clearWormHole(layers, mTarget);

    // to do composite.
    int i = 0, ret = 0;
    for (auto layer : layers) {
        if (layer->getCompositionType() == Composition::SIDEBAND)
            // set side band parameters.
            continue;

        auto hnd = layer->getBuffer().getBuffer();
        G2dInterBuffer* interData = preComposition(layer, hnd);

        G2dBuffer layerBuffer;
        if (interData != nullptr) {
            layerBuffer.hnd = interData->hnd;
            layerBuffer.interPtr = interData;
        } else {
            layerBuffer.hnd = hnd;
        }
        if (layerBuffer.hnd != NULL && (getInfoFromHandle(layerBuffer.hnd, &layerBuffer.info) == 0))
            lockSurface(layerBuffer);

        ret = composeLayerLocked(layer, layerBuffer, mTarget, i == 0);

        if (layerBuffer.hnd != NULL)
            unlockSurface(layerBuffer);

        if (ret != 0) {
            ALOGE("%s: compose layer %zu failed", __FUNCTION__, layer->getId());
            break;
        }
        i++;
    }
    ::android::base::unique_fd composeFence(createFenceFd(getHandle()));

    unlockSurface(mTarget);

    if (!composeFence.ok())
        finishComposite();

    return std::make_tuple(true, std::move(composeFence));
}

} // namespace aidl::android::hardware::graphics::composer3::impl
