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
#include <sync/sync.h>
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
    mSolidColorBuffer.infoPtr = &mSolidColorBuffer.info;
    mOclCvt = std::make_unique<OclConverter>();
    if (mOclCvt->isValid())
        ALOGI("%s: OpenCL lib load successfully", __FUNCTION__);

    mInterCount = getMaxG2dInterCompositionResult();
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
        newFormat = srcBuffer.infoPtr->format;

    if ((tempBuffer->hnd != NULL) &&
        (srcBuffer.infoPtr->width == tempBuffer->infoPtr->width &&
         srcBuffer.infoPtr->height == tempBuffer->infoPtr->height &&
         newFormat == tempBuffer->infoPtr->format)) {
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
            ::android::GraphicBufferAllocator::get().allocate(srcBuffer.infoPtr->width,
                                                              srcBuffer.infoPtr->height, newFormat,
                                                              1, srcBuffer.infoPtr->usage,
                                                              &bufferHandle, &bufferStride,
                                                              "HwcG2dTempBuffer");
    if (status != ::android::OK) {
        ALOGE("%s: failed to allocate g2d temporary buffer", __FUNCTION__);
        return -1;
    }

    if (getInfoFromHandle(bufferHandle, tempBuffer->infoPtr) != 0) {
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
        rect.right = static_cast<int>(mSolidColorBuffer.infoPtr->width);
        rect.bottom = static_cast<int>(mSolidColorBuffer.infoPtr->height);
        clearRect(mSolidColorBuffer, rect, 0xff << 24);
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

int DeviceComposer::clearRect(G2dBuffer& buff, common::Rect& rect, uint32_t color) {
    if (buff.hnd == NULL || isRectEmpty(rect)) {
        return 0;
    }

    struct g2d_surfaceEx surfaceX;
    struct g2d_surface& surface = surfaceX.base;

    memset(&surfaceX, 0, sizeof(surfaceX));
    setG2dSurface(surfaceX, buff, rect);
    surface.clrcolor = color;
    clearFunction(getHandle(), &surface);

    DEBUG_LOG_G2D("clearRect: rect(l:%d,t:%d,r:%d,b:%d) with color(0xABGR):0x%08x", rect.left,
                  rect.top, rect.right, rect.bottom, color);
    return 0;
}

int DeviceComposer::clearWormHole(uint32_t displayId, std::vector<int64_t>& layerIds,
                                  G2dBuffer& target) {
    DEBUG_LOG("%s: clear worm hole", __FUNCTION__);
    if (target.hnd == nullptr || target.infoPtr == nullptr) {
        ALOGE("%s: no effective render buffer", __FUNCTION__);
        return -EINVAL;
    }

    auto& cachedComposition = mCachedDisplays[displayId].cachedCompositions;
    auto& cachedLayers = mCachedDisplays[displayId].cachedLayers;

    // calculate opaque region.
    ::android::Region opaque;
    for (auto id : layerIds) {
        G2dInterLayer* layer;
        if (id > 0)
            layer = &cachedLayers[id];
        else
            layer = &cachedComposition[id].interlayer;

        for (auto& rect : *layer->visible) {
            opaque.orSelf(::android::Rect(rect.left, rect.top, rect.right, rect.bottom));
        }
    }

    // calculate worm hole.
    ::android::Region screen(::android::Rect(target.infoPtr->width, target.infoPtr->height));
    screen.subtractSelf(opaque);
    const ::android::Rect* holes = NULL;
    size_t numRect = 0;
    holes = screen.getArray(&numRect);
#ifdef DEBUG_NXP_HWC_G2D
    std::string opaque_str;
    char tempStr[64];
    auto head = opaque.begin();
    auto const tail = opaque.end();
    while (head != tail) {
        sprintf(tempStr, "[%d,%d,%d,%d]", head->left, head->top, head->right, head->bottom);
        opaque_str += tempStr;
        head++;
    }
    DEBUG_LOG_G2D("%s: clear %zu worm holes(opaque=%s)", __FUNCTION__, numRect, opaque_str.c_str());
#endif

    // clear worm hole.
    struct g2d_surfaceEx surfaceX;
    memset(&surfaceX, 0, sizeof(surfaceX));
    struct g2d_surface& surface = surfaceX.base;
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

int DeviceComposer::onDisplayCreate(uint32_t displayId) {
    if (mCachedDisplays.find(displayId) == mCachedDisplays.end()) {
        mCachedDisplays.emplace(displayId, G2dCachedDisplay{});
    }

    return 0;
}

int DeviceComposer::onDisplayDestroy(uint32_t displayId) {
    auto& cachedComposition = mCachedDisplays[displayId].cachedCompositions;
    auto& cachedLayers = mCachedDisplays[displayId].cachedLayers;

    cachedLayers.clear();
    cachedComposition.clear();
    mCachedDisplays.erase(displayId);

    return 0;
}

int DeviceComposer::onDisplayLayerDestroy(uint32_t displayId, Layer* layer) {
    auto id = layer->getId();
    if (mInterBuffers.find(id) != mInterBuffers.end()) {
        ::android::GraphicBufferAllocator::get().free(mInterBuffers[id].hnd);
        mInterBuffers.erase(id);
    }

    auto& cachedLayers = mCachedDisplays[displayId].cachedLayers;

    if (cachedLayers.find(id) != cachedLayers.end()) {
        cachedLayers.erase(id);
    }

    return 0;
}

int DeviceComposer::convertBuffer(buffer_handle_t inBuf, HandleInfo* inInfoPtr,
                                  buffer_handle_t outBuf, HandleInfo* outInfoPtr, bool useOcl) {
#ifdef DEBUG_DUMP_VIRT_G2D_CONSUMPTION
    nsecs_t g2dStart = systemTime(CLOCK_MONOTONIC);
#endif
    G2dBuffer srcBuff{inBuf, inInfoPtr};
    G2dBuffer dstBuff{outBuf, outInfoPtr};
    common::Rect rect{0, 0, static_cast<int32_t>(inInfoPtr->width),
                      static_cast<int32_t>(inInfoPtr->height)};
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
    char* fmt_name1 = drmGetFormatName(inInfoPtr->drm_format, fmt1);
    char* fmt_name2 = drmGetFormatName(outInfoPtr->drm_format, fmt2);
    char* modifier_name1 = drmGetFormatModifierName(inInfoPtr->modifier);
    char* modifier_name2 = drmGetFormatModifierName(outInfoPtr->modifier);
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
    int interBufferType = G2D_CONVERSION_TYPE_NONE;
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
        interBufferType = G2D_CONVERSION_TYPE_SCALING;
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
        interBufferType = G2D_CONVERSION_TYPE_CSC;
        opencl_prefered = true;
    } else
#endif
    {
        return nullptr;
    }

    bool reuseBuff = true;
    buffer_handle_t outHandle;
    HandleInfo outBufInfo;
    if (mInterBuffers.find(id) != mInterBuffers.end()) {
        auto interBuf = mInterBuffers[id];
        if (interBuf.originBufferId == inBufInfo.buffer_id) {
            return &(mInterBuffers[id]);
        } else if ((dstW == interBuf.info.width) && (dstH == interBuf.info.height) &&
                   (dstFormat == interBuf.info.format) && (dstUsage == interBuf.info.usage)) {
            reuseBuff = true;
            outHandle = interBuf.hnd;
            outBufInfo = interBuf.info;
        } else {
            reuseBuff = false;
            mInterBuffers.erase(id);
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

    if (interBufferType == G2D_CONVERSION_TYPE_SCALING) {
        lockBuffer(outHandle, outBufInfo);
        lockBuffer(handle, inBufInfo);
    }

    if (interBufferType == G2D_CONVERSION_TYPE_SCALING) {
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
    } else if (interBufferType == G2D_CONVERSION_TYPE_CSC) {
        convertBuffer(handle, &inBufInfo, outHandle, &outBufInfo, opencl_prefered);
    }

    if (!reuseBuff) {
        G2dInterBuffer newBuff{id, interBufferType, outHandle, outBufInfo, inBufInfo.buffer_id};
        mInterBuffers.emplace(id, newBuff);
    }

    if (interBufferType == G2D_CONVERSION_TYPE_SCALING) {
        unlockBuffer(handle, inBufInfo);
        unlockBuffer(outHandle, outBufInfo);
    }

    return &(mInterBuffers[id]);
#endif
}

int DeviceComposer::composeLayerLocked(G2dBuffer& layerBuffer, G2dBuffer& targetBuffer,
                                       bool bypass) {
    if (layerBuffer.layer == nullptr || targetBuffer.hnd == nullptr) {
        ALOGE("%s: invalid layer or target", __FUNCTION__);
        return -EINVAL;
    }
    DEBUG_LOG("%s: compose layer %ld", __FUNCTION__, layerBuffer.layer->id);

    auto type = layerBuffer.layer->type;
    auto mode = layerBuffer.layer->mode;
    auto transform = layerBuffer.layer->transform;
    auto alpha = layerBuffer.layer->alpha;
    common::Rect& srect = layerBuffer.layer->srect;
    common::Rect& drect = layerBuffer.layer->drect;
#ifdef DEBUG_NXP_HWC_G2D
    if (layerBuffer.hnd != nullptr) {
        DEBUG_LOG_G2D("%s: compose layer(%s) id=%ld, %d x %d, zorder:0x%x, phys:0x%" PRIx64
                      ", transform:%s, blend:%s, alpha:0x%x, name=%s",
                      __FUNCTION__, toString(type).c_str(), layerBuffer.layer->id,
                      layerBuffer.infoPtr->width, layerBuffer.infoPtr->height,
                      layerBuffer.layer->zorder, layerBuffer.infoPtr->phys,
                      toString(transform).c_str(), toString(mode).c_str(), alpha,
                      layerBuffer.infoPtr->name);
    } else {
        DEBUG_LOG_G2D("%s: compose layer(%s) id=%ld, zorder:0x%x, transform:%s, blend:%s, "
                      "alpha:0x%x, solid color layer",
                      __FUNCTION__, toString(type).c_str(), layerBuffer.layer->id,
                      layerBuffer.layer->zorder, toString(transform).c_str(),
                      toString(mode).c_str(), alpha);
    }
#endif

    if (((layerBuffer.hnd != nullptr) && isRectEmpty(srect)) || isRectEmpty(drect)) {
        ALOGE("%s: type=%s, invalid srect(%d, %d, %d, %d) or drect(%d, %d, %d, %d)", __FUNCTION__,
              toString(type).c_str(), srect.left, srect.top, srect.right, srect.bottom, drect.left,
              drect.top, drect.right, drect.bottom);
        return 0;
    }
    if (alpha == 0) {
        DEBUG_LOG_G2D("%s: global_alpha is 0(transparent), skip such layer", __FUNCTION__);
        return 0;
    }

    if (layerBuffer.hnd == nullptr) {
        prepareSolidColorBuffer(targetBuffer);
    }

    struct g2d_surfaceEx dSurfaceX;
    struct g2d_surface& dSurface = dSurfaceX.base;

    memset(&dSurfaceX, 0, sizeof(dSurfaceX));
    setG2dSurface(dSurfaceX, targetBuffer, drect);

    bool needDither = false;
    std::vector<common::Rect>* visible = layerBuffer.layer->visible;
    for (auto& clip : *visible) {
        if (isRectEmpty(clip)) {
            DEBUG_LOG_G2D("%s: invalid clip(%d, %d, %d, %d)", __FUNCTION__, clip.left, clip.top,
                          clip.right, clip.bottom);
            continue;
        }

        if (!rectIntersect(drect, clip)) {
            DEBUG_LOG_G2D("%s: invalid clip rect", __FUNCTION__);
            continue;
        }
        setClipping(srect, drect, clip, transform);
        DEBUG_LOG_G2D("layer:%ld, sourceCrop(l:%d,t:%d,r:%d,b:%d), visible(l:%d,t:%d,r:%d,b:%d), "
                      "display(l:%d,t:%d,r:%d,b:%d)",
                      layerBuffer.layer->id, srect.left, srect.top, srect.right, srect.bottom,
                      clip.left, clip.top, clip.right, clip.bottom, drect.left, drect.top,
                      drect.right, drect.bottom);

        struct g2d_surfaceEx sSurfaceX;
        memset(&sSurfaceX, 0, sizeof(sSurfaceX));
        struct g2d_surface& sSurface = sSurfaceX.base;

        if (!(type == Composition::SOLID_COLOR) && layerBuffer.hnd) {
            if ((layerBuffer.interPtr != nullptr) &&
                (layerBuffer.interPtr->type == G2D_CONVERSION_TYPE_SCALING))
                setG2dSurface(sSurfaceX, layerBuffer, drect);
            else
                setG2dSurface(sSurfaceX, layerBuffer, srect);
#ifndef G2D_LIMITATION_PXP // PXP G2D don't support DITHER
            auto format = static_cast<common::PixelFormat>(targetBuffer.infoPtr->format);
            if ((format == common::PixelFormat::RGB_565) &&
                (format == common::PixelFormat::RGBA_8888 ||
                 format == common::PixelFormat::RGBX_8888 ||
                 format == common::PixelFormat::BGRA_8888)) {
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
    if (buff.hnd == nullptr || buff.infoPtr == nullptr) {
        ALOGE("%s: handle is invalid!", __FUNCTION__);
        return -1;
    }

    surface.format = convertFormat(buff.infoPtr->drm_format, buff);
    enum g2d_tiling tile = G2D_LINEAR;
    getTiling(buff, &tile);
#ifdef G2D_FORMAT_CONVERSION
    if (buff.infoPtr->drm_format == DRM_FORMAT_NV15 &&
        buff.infoPtr->modifier == DRM_FORMAT_MOD_AMPHION_TILED) {
        surfaceX.tiling = G2D_AMPHION_TILED_10BIT;
    } else
#endif
            if (buff.infoPtr->modifier == DRM_FORMAT_MOD_AMPHION_TILED) {
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
    if (buff.infoPtr->phys)
        phys = buff.infoPtr->phys;
    else
        getBuffPhys(buff, &phys);

    getFlipOffset(buff, &offset);
    surface.planes[0] = static_cast<g2d_phys_addr_t>(phys + offset);

    switch (surface.format) {
        case G2D_GRAY8:
            surface.stride = static_cast<int>(buff.infoPtr->strides[0]); // convert to pixel stride
            break;
        case G2D_RGB565:
        case G2D_YUYV:
            // convert to pixel stride
            surface.stride = static_cast<int>(buff.infoPtr->strides[0] / 2);
            break;
        case G2D_RGBA8888:
        case G2D_BGRA8888:
        case G2D_RGBX8888:
        case G2D_BGRX8888:
        case G2D_RGBA1010102:
            // convert to pixel stride
            surface.stride = static_cast<int>(buff.infoPtr->strides[0] / 4);
            break;

        case G2D_NV16:
        case G2D_NV12:
        case G2D_NV21:
            surface.stride = static_cast<int>(buff.infoPtr->strides[0]);
            surface.planes[1] = surface.planes[0] + buff.infoPtr->offsets[1];
            break;

        case G2D_I420:
        case G2D_YV12: {
            surface.stride = static_cast<int>(buff.infoPtr->strides[0]);
            surface.planes[1] = surface.planes[0] + buff.infoPtr->offsets[1];
            surface.planes[2] = surface.planes[0] + buff.infoPtr->offsets[2];
        } break;

        default:
            ALOGE("%s: does not support format:%d", __FUNCTION__, surface.format);
            break;
    }
    int buff_width = static_cast<int>(buff.infoPtr->width);
    int buff_height = static_cast<int>(buff.infoPtr->height);
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
    if (buff.infoPtr->format == HAL_PIXEL_FORMAT_P010_TILED ||
        buff.infoPtr->format == HAL_PIXEL_FORMAT_NV12_TILED)
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
    buff.originPhys = buff.infoPtr->phys;
#endif
    int ret = (*mLockSurface)((void*)buff.hnd);
#ifdef G2D_LIMITATION_VIV
    // The phys in handle will change after lockSurface() if GPU disable flat-mapping, update info
    getPhysFromHandle(buff.hnd, &buff.infoPtr->phys);
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

    if (buff.hnd == nullptr || buff.infoPtr == nullptr) {
        ALOGE("%s: handle is invalid!", __FUNCTION__);
        return -EINVAL;
    }

    struct g2d_buf* buf = (struct g2d_buf*)(*mBuffInfoFromFd)((void*)(intptr_t)buff.infoPtr->fd);
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
    auto infoPtr = layer->getBufferInfo();
    if (layerBuffer == nullptr || infoPtr == nullptr) {
        return false;
    }

    // vpu tile format must be handled by device.
    if (layerBuffer != nullptr &&
        (infoPtr->modifier == DRM_FORMAT_MOD_AMPHION_TILED ||
         infoPtr->usage & GRALLOC_USAGE_PROTECTED || infoPtr->drm_format == DRM_FORMAT_R8)) {
        DEBUG_LOG("%s: 2d composition is must", __FUNCTION__);
        return true;
    }

    return false;
}

bool DeviceComposer::checkDeviceComposition(Layer* layer) {
    DEBUG_LOG("%s: check layer %ld", __FUNCTION__, layer->getId());

    auto layerBuffer = layer->getBuffer().getBuffer();
    auto infoPtr = layer->getBufferInfo();
    if (layerBuffer == NULL) { // support device composition for SOLID_COLOR layer
        return true;
    } else if (infoPtr == nullptr) {
        ALOGE("%s: fail to get buffer infomation", __FUNCTION__);
        return false;
    }

#ifndef G2D_LIMITATION_PXP
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
    if (infoPtr->drm_format == DRM_FORMAT_ABGR2101010) {
        DEBUG_LOG("%s: g2d can't support ABGR2101010 format", __FUNCTION__);
        return false;
    }

    common::Dataspace dataspace = layer->getDataspace();
    // video nv12 full range should be handled by client
    if (layerBuffer != nullptr && infoPtr->drm_format == DRM_FORMAT_NV12 &&
        ((common::Dataspace)((int)dataspace & (int)common::Dataspace::RANGE_MASK) ==
         common::Dataspace::RANGE_FULL)) {
        DEBUG_LOG("%s: g2d can't support video nv12 full range", __FUNCTION__);
        return false;
    }
#endif
#ifdef G2D_LIMITATION_DPU
    if ((infoPtr->drm_format == DRM_FORMAT_P010) || (infoPtr->drm_format == DRM_FORMAT_P210)) {
        DEBUG_LOG("%s: g2d can't support 0x%x format", __FUNCTION__, infoPtr->drm_format);
        return false;
    }
#endif

    if (!(infoPtr->usage &
          (GRALLOC_USAGE_PROTECTED | GRALLOC_USAGE_PRIVATE_3 | GRALLOC_USAGE_HW_COMPOSER |
           GRALLOC_USAGE_HW_FB))) {
        ALOGI("%s: g2d can't support the buffer from system/system-uncached heap", __FUNCTION__);
        return false;
    }

    return true;
}

std::optional<std::vector<int64_t>> DeviceComposer::cacheG2dLayersStats(
        uint32_t displayId, std::vector<Layer*> layers) {
    auto& cachedLayers = mCachedDisplays[displayId].cachedLayers;
    std::vector<int64_t> orderedIds;
    for (auto& layer : layers) {
        auto id = layer->getId();
        auto zorder = layer->getZOrder();
        auto alpha = (uint8_t)(layer->getPlaneAlpha() * 255);
        auto drect = layer->getDisplayFrame();
        auto srect = layer->getSourceCropInt();
        auto transform = layer->getTransform();
        auto infoPtr = layer->getBufferInfo(); // the info.buffer_id = 0 for solid color layer
        auto& visible = layer->getVisibleRegion();
        common::Rect visibleRect{0, 0, 0, 0};
        if (visible.size() > 0)
            visibleRect = visible[0]; // TODO: only the first rect used now.

        if (cachedLayers.find(id) != cachedLayers.end()) {
            auto& cache = cachedLayers[id];
            if ((cache.buffer_id == infoPtr->buffer_id) && (cache.zorder == zorder) &&
                (cache.alpha == alpha) && (cache.drect == drect) && (cache.srect == srect) &&
                (cache.transform == transform) && (cache.visibleRect == visibleRect)) {
                cache.keep_count++;
            } else {
                cache.keep_count = 0;
                cache.buffer_id = infoPtr->buffer_id;
                cache.zorder = zorder;
                cache.alpha = alpha;
                cache.drect = drect;
                cache.srect = srect;
                cache.transform = transform;
                cache.visibleRect = visibleRect;
            }
            /* find that the layer with RGBX_8888 format(blend mode=NONE) in intermediate
               composition cannot be processed normally when do final composition(the pixel
               alpha seems not process correctly).
               TODO: need to DPU team to make sure NONE blend mode work normally.
             */
            if (infoPtr->buffer_id != 0 && infoPtr->drm_format != DRM_FORMAT_ABGR8888)
                cache.keep_count = 0;
        } else {
            cachedLayers.emplace(id,
                                 G2dInterLayer{
                                         .id = id,
                                         .zorder = zorder,
                                         .alpha = alpha,
                                         .type = layer->getCompositionType(),
                                         .mode = layer->getBlendMode(),
                                         .drect = drect,
                                         .srect = srect,
                                         .transform = transform,
                                         .visible = layer->getVisibleRegionPtr(),
                                         .visibleRect = visibleRect,
                                         .priv = layer,
                                         .keep_count = 0,
                                         .buffer_id = infoPtr->buffer_id,
                                 });
        }
        orderedIds.push_back(id);
    }

#ifdef G2D_CACHED_COMPOSITION
    if (mInterCount < 1)
        return std::nullopt;

    std::unordered_map<int32_t, std::vector<int64_t>> cachedIds;
    int32_t idx = 0;
    std::vector<int64_t> slices;
    for (auto& id : orderedIds) {
        auto& l = cachedLayers[id];
        DEBUG_LOG_G2D("%s: layer %ld, keep %d, slices size=%zu", __FUNCTION__, id, l.keep_count,
                      slices.size());
        if ((l.keep_count >= LAYER_LEAST_KEEP_CNT) && (l.alpha == 0xff)) {
            slices.push_back(l.id);
        } else if (slices.size() >= LAYER_LEAST_ADJACENT_CNT) {
            cachedIds.emplace(idx, slices);
            slices.clear();
            idx++;
        } else {
            slices.clear();
        }
    }
    if (slices.size() >= LAYER_LEAST_ADJACENT_CNT) {
        cachedIds.emplace(idx, slices);
        idx++;
    }

    auto& cachedComposition = mCachedDisplays[displayId].cachedCompositions;
    // Check if the layers are overlapped in each slices
    if (idx > 0) {
        std::vector<int32_t> idx_candidates;
        for (auto& [i, slices] : cachedIds) {
            common::Rect dispRect = {0, 0, 0, 0};
            uint64_t areaSum = 0;
            for (auto id : slices) {
                areaSum += calculateRect(cachedLayers[id].drect);
                mergeRect(dispRect, cachedLayers[id].drect);
            }
            uint64_t dispArea = calculateRect(dispRect);
            if (areaSum >= dispArea) {
                idx_candidates.emplace_back(i);
            }
        }

        std::vector<int32_t> bestIdx;
        int32_t max = mInterCount;
        while (max > 0 && idx_candidates.size() > 0) {
            auto best = idx_candidates.end();
            uint32_t maxContained = 0;
            for (auto it = idx_candidates.begin(); it != idx_candidates.end(); it++) {
                if (cachedIds[*it].size() > maxContained) {
                    maxContained = cachedIds[*it].size();
                    best = it;
                }
            }
            if (best != idx_candidates.end()) {
                bestIdx.push_back(*best);
                idx_candidates.erase(best);
            } else {
                break;
            }
            max--;
        }

        std::vector<int64_t> orderedInterIds;
        std::sort(bestIdx.begin(), bestIdx.end());
        for (auto& [id, interComp] : cachedComposition) {
            bool reuse = false;
            for (auto idx : bestIdx) {
                if (cachedIds[idx] == interComp.composedIds) { // matched
                    reuse = true;
                    break;
                }
            }
            if (!reuse) { // the id composition is out-date
                interComp.state = INTER_STATE_INVALID;
            }
        }
        for (auto idx : bestIdx) {
            int64_t interId = 0;
            for (auto& [id, interComp] : cachedComposition) {
                if ((interComp.state == INTER_STATE_COMPOSED) &&
                    (cachedIds[idx] == interComp.composedIds)) { // matched
                    interId = id;
                    break;
                }
            }
            if (interId == 0) { // not find any cached composition for this cachedIds[idx]
                if (cachedComposition.size() < mInterCount) {
                    // new intermediate composition id
                    interId = mInterId--;
                    cachedComposition.emplace(interId,
                                              G2dInterComposition{
                                                      .hnd = nullptr,
                                                      .state = INTER_STATE_VALIDATED,
                                                      .composedIds = std::move(cachedIds[idx]),
                                              });
                } else { // the cached composition pool is full
                    for (auto& [id, interComp] : cachedComposition) {
                        if (interComp.state == INTER_STATE_INVALID) {
                            interId = id;
                            interComp.composedIds = std::move(cachedIds[idx]);
                            interComp.state = INTER_STATE_VALIDATED;
                            break;
                        }
                    }
                }
            }
            if (interId == 0)
                ALOGE("%s: Should not happen!!!", __FUNCTION__);

            orderedInterIds.insert(orderedInterIds.end(), interId);
        }

        if (orderedInterIds.size() > 0)
            return std::make_optional(std::move(orderedInterIds));
        else
            return std::nullopt;
    } else { // there is no any intermediate composition used, reset all state
        for (auto& [_, interComp] : cachedComposition) interComp.state = INTER_STATE_INVALID;

        return std::nullopt;
    }
#else
    return std::nullopt;
#endif
}

void DeviceComposer::composeG2dLayers(uint32_t displayId, std::vector<int64_t>& layerIds,
                                      G2dBuffer& targetBuffer) {
#ifdef DEBUG_NXP_HWC_G2D
    char tempStr[12];
    std::string IdStr;
    for (const auto& id : layerIds) {
        sprintf(tempStr, "%ld ", id);
        IdStr += tempStr;
    }
    DEBUG_LOG_G2D("%s: ----compose %zu layers(%s)----", __FUNCTION__, layerIds.size(),
                  IdStr.c_str());
#endif
    if (targetBuffer.hnd == nullptr || targetBuffer.infoPtr == nullptr) {
        ALOGE("%s: invalid target handle or info", __func__);
        return;
    }

    auto& cachedComposition = mCachedDisplays[displayId].cachedCompositions;
    auto& cachedLayers = mCachedDisplays[displayId].cachedLayers;

    lockSurface(targetBuffer);

    // to do composite.
    int i = 0, ret = 0;
    for (auto id : layerIds) {
        G2dBuffer layerBuffer;
        if (id < 0) { // only for intermediate composition result
            layerBuffer.hnd = cachedComposition[id].hnd;
            layerBuffer.infoPtr = &cachedComposition[id].info;
            layerBuffer.layer = &cachedComposition[id].interlayer;
        } else {
            auto& layer = cachedLayers[id];
            if (layer.type == Composition::SIDEBAND)
                // set side band parameters.
                continue;

            auto hnd = layer.priv->getBuffer().getBuffer();
            G2dInterBuffer* interData = preComposition(layer.priv, hnd);

            if (interData != nullptr) {
                layerBuffer.hnd = interData->hnd;
                layerBuffer.infoPtr = &interData->info;
                layerBuffer.interPtr = interData;
            } else {
                layerBuffer.hnd = hnd;
                layerBuffer.infoPtr = layer.priv->getBufferInfo();
            }
            layerBuffer.layer = &layer;
        }

        if (layerBuffer.hnd != NULL)
            lockSurface(layerBuffer);

        ret = composeLayerLocked(layerBuffer, targetBuffer, i == 0);

        if (layerBuffer.hnd != NULL)
            unlockSurface(layerBuffer);

        if (ret != 0) {
            ALOGE("%s: compose layer %ld failed", __FUNCTION__, id);
            break;
        }
        i++;
    }

    unlockSurface(targetBuffer);
}

int DeviceComposer::composeInterLayer(uint32_t displayId, int64_t interId,
                                      G2dInterComposition& interComposition) {
#ifdef DEBUG_NXP_HWC_G2D
    char tempStr[12];
    std::string IdStr;
    for (const auto& id : interComposition.composedIds) {
        sprintf(tempStr, "%ld ", id);
        IdStr += tempStr;
    }
    DEBUG_LOG_G2D("%s: --compose %zu layers(%s) as intermediate layer--", __FUNCTION__,
                  interComposition.composedIds.size(), IdStr.c_str());
#endif
    auto& cachedLayers = mCachedDisplays[displayId].cachedLayers;
    if (interComposition.hnd == nullptr) {
        bool isSecure = false;
        if (mTarget.infoPtr->usage & GRALLOC_USAGE_PROTECTED)
            isSecure = true;
        std::vector<buffer_handle_t> buffers;
        auto ret = prepareDeviceFrameBuffer(mTarget.infoPtr->width, mTarget.infoPtr->height,
                                            /*mTarget.infoPtr->format*/
                                            static_cast<int>(common::PixelFormat::RGBA_8888),
                                            buffers, 1, isSecure);
        if (ret)
            return -1;

        interComposition.hnd = buffers[0];
        if (getInfoFromHandle(buffers[0], &(interComposition.info)) != 0) {
            ALOGE("%s: failed to get buffer info of cached composition buffer", __FUNCTION__);
            return -1;
        }
        interComposition.interlayer.visible = &interComposition.visible;
    }

    G2dBuffer dstBuffer;
    // The intermediate buffer is allocated when prepare G2D framebuffer
    dstBuffer.hnd = interComposition.hnd;
    dstBuffer.infoPtr = &interComposition.info;

    common::Rect rect;
    rect.left = rect.top = 0;
    rect.right = static_cast<int>(dstBuffer.infoPtr->width);
    rect.bottom = static_cast<int>(dstBuffer.infoPtr->height);
    clearRect(dstBuffer, rect, 0x00 << 24);

    composeG2dLayers(displayId, interComposition.composedIds, dstBuffer);

    common::BlendMode mode = common::BlendMode::NONE;
    if (interComposition.zorder > 0)
        mode = common::BlendMode::PREMULTIPLIED;

    common::Rect dispRect{0, 0, 0, 0};
    for (auto id : interComposition.composedIds) {
        mergeRect(dispRect, cachedLayers[id].drect);
    }
    auto& interlayer = interComposition.interlayer;
    interlayer.id = interId;
    interlayer.zorder = interComposition.zorder;
    interlayer.alpha = 0xff;
    interlayer.type = Composition::DEVICE;
    interlayer.mode = mode;
    interlayer.drect = dispRect;
    interlayer.srect = dispRect;
    interlayer.transform = common::Transform::NONE;
    interlayer.visible->clear();
    interlayer.visible->push_back(dispRect);
    DEBUG_LOG_G2D("%s: new intermediate layer with visible rect:%d, %d, %d, %d", __FUNCTION__,
                  dispRect.left, dispRect.top, dispRect.right, dispRect.bottom);

    return 0;
}

std::tuple<bool, ::android::base::unique_fd> DeviceComposer::composeLayers(
        uint32_t displayId, std::vector<Layer*> layers, buffer_handle_t target) {
    ATRACE_CALL();

    Mutex::Autolock _l(sLock);
    if (mCachedDisplays.find(displayId) == mCachedDisplays.end()) {
        ALOGE("%s: display id=%d is invalid", __FUNCTION__, displayId);
        return std::make_tuple(false, ::android::base::unique_fd());
    }
    if (!target || (getInfoFromHandle(target, &mTarget.info) != 0)) {
        ALOGE("%s: composer target buffer is invalid", __FUNCTION__);
        return std::make_tuple(false, ::android::base::unique_fd());
    }
    mTarget.hnd = target;
    mTarget.infoPtr = &mTarget.info;

#if defined(DEBUG_NXP_HWC_G2D) || defined(DEBUG_NXP_HWC)
    char tempStr[12];
    std::string IdStr;
    for (const auto& layer : layers) {
        sprintf(tempStr, "%ld ", layer->getId());
        IdStr += tempStr;
    }
    ALOGI("%s: ------display %d compose %zu layers(%s) to target(fd=%d, %d x %d)-------",
          __FUNCTION__, displayId, layers.size(), IdStr.c_str(), mTarget.info.fd,
          mTarget.info.width, mTarget.info.height);
#endif

    auto& cachedComposition = mCachedDisplays[displayId].cachedCompositions;
    auto& cachedLayers = mCachedDisplays[displayId].cachedLayers;

    std::vector<int64_t> finalIds;
    auto ids = cacheG2dLayersStats(displayId, layers);
    if (ids) {
        std::vector<int64_t>& orderedInterIds = *ids;
        uint32_t idx = 0;
        bool found = false;
        std::vector<int64_t>* cachedIds = &(cachedComposition[orderedInterIds[idx]].composedIds);
        for (auto layer : layers) {
            auto id = layer->getId();
            bool selected = false;
            if (cachedIds != nullptr)
                selected = std::any_of((*cachedIds).begin(), (*cachedIds).end(),
                                       [&](int64_t i) { return i == id; });
            if (selected) {
                if (finalIds.empty() || finalIds.back() > 0) { // all orderedInderId[x] < 0
                    finalIds.insert(finalIds.end(), orderedInterIds[idx]);
                    cachedComposition[orderedInterIds[idx]].zorder = layer->getZOrder();
                }
                found = true;
            } else {
                finalIds.insert(finalIds.end(), id);
                if (found) { // previous Ids check completed, check new Ids
                    idx++;
                    if (orderedInterIds.size() > idx)
                        cachedIds = &(cachedComposition[orderedInterIds[idx]].composedIds);
                    else
                        cachedIds = nullptr;
                    found = false;
                }
            }
        }

        for (auto oid : orderedInterIds) {
            if (cachedComposition[oid].state == INTER_STATE_VALIDATED) { // need to do composition
                composeInterLayer(displayId, oid, cachedComposition[oid]);
                cachedComposition[oid].state = INTER_STATE_COMPOSED;
            }
        }
    } else {
        for (auto layer : layers) finalIds.push_back(layer->getId());
    }

    composeG2dLayers(displayId, finalIds, mTarget);

    ::android::base::unique_fd composeFence(createFenceFd(getHandle()));
    if (!composeFence.ok())
        finishComposite();

#ifdef DEBUG_DUMP_G2D_INTER_COMPOSITION
    if (composeFence.ok()) {
        int err = sync_wait(composeFence.get(), 3000);
        if (err < 0 && errno == ETIME) {
            ALOGE("%s waited on g2d fence %" PRId32 " for 3000 ms", __FUNCTION__,
                  composeFence.get());
        }
    }
    for (auto& [id, comp] : cachedComposition)
        if (comp.state == INTER_STATE_COMPOSED)
            debug_dump_layerbuffer(comp.hnd, id);
#endif
    return std::make_tuple(true, std::move(composeFence));
}

} // namespace aidl::android::hardware::graphics::composer3::impl
