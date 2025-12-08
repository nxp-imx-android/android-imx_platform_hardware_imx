/*
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

#include "VirtualFrameComposer.h"

#include <drm_fourcc.h>
#include <hardware/gralloc.h>
#include <sync/sync.h>
#include <ui/Fence.h>

#include "BufferInfo.h"
#include "Common.h"
#include "Display.h"
#include "Layer.h"

using namespace android;

namespace aidl::android::hardware::graphics::composer3::impl {

HWC3::Error VirtualFrameComposer::init(std::shared_ptr<DeviceComposer>& g2d) {
    DEBUG_LOG("%s", __FUNCTION__);

    mG2dComposer = g2d;
    if (!mG2dComposer->isValid())
        ALOGW("%s: G2D composition is not valid", __FUNCTION__);

    return HWC3::Error::None;
}

HWC3::Error VirtualFrameComposer::registerOnHotplugCallback(const HotplugCallback& cb) {
    DEBUG_LOG("%s", __FUNCTION__);
    return HWC3::Error::None;
}

HWC3::Error VirtualFrameComposer::unregisterOnHotplugCallback() {
    DEBUG_LOG("%s", __FUNCTION__);
    return HWC3::Error::None;
}

HWC3::Error VirtualFrameComposer::onDisplayCreate(Display* display) {
    const auto displayId = display->getId();
    DEBUG_LOG("%s display:%d", __FUNCTION__, displayId);

    int32_t activeConfigId = -1;
    if (display->getActiveConfig(&activeConfigId) != HWC3::Error::None) {
        ALOGE("%s: fail to get active config id", __FUNCTION__);
    }
    int32_t width = 0, height = 0;
    common::PixelFormat format = common::PixelFormat::UNSPECIFIED;
    if (activeConfigId >= 0) {
        display->getDisplayAttribute(activeConfigId, DisplayAttribute::WIDTH, &width);
        display->getDisplayAttribute(activeConfigId, DisplayAttribute::HEIGHT, &height);
        format = display->getFormat();
    }

    // Ensure created.
    mDisplays.emplace(displayId, VirtualDisplay{width, height, format});
    mDisplayLayers.emplace(displayId, ValidatedLayers{});

    if (format != common::PixelFormat::RGBA_8888) {
        format = common::PixelFormat::RGBA_8888;
        std::vector<buffer_handle_t> buffers;
        auto ret = mG2dComposer->prepareDeviceFrameBuffer(width, height, static_cast<int>(format),
                                                          buffers, 1, false);
        if (ret) {
            ALOGE("%s: fail to allocate temp buffer(%d x %d, %s) for format conversion",
                  __FUNCTION__, width, height, toString(format).c_str());
        } else {
            mDisplays[displayId].convertBuffer = buffers[0];
            if (getInfoFromHandle(buffers[0], &mDisplays[displayId].convertInfo) != 0)
                ALOGE("%s: fail to get buffer info of format conversion buffer", __FUNCTION__);
        }
    }

    mG2dComposer->onDisplayCreate(displayId);

    return HWC3::Error::None;
}

HWC3::Error VirtualFrameComposer::onDisplayDestroy(Display* display) {
    const auto displayId = display->getId();
    DEBUG_LOG("%s display:%d", __FUNCTION__, displayId);

    auto it = mDisplays.find(displayId);
    if (it == mDisplays.end()) {
        ALOGE("%s: Not find display %d", __FUNCTION__, displayId);
        return HWC3::Error::BadDisplay;
    }

    mG2dComposer->onDisplayDestroy(displayId);
    mDisplays.erase(it);
    return HWC3::Error::None;
}

HWC3::Error VirtualFrameComposer::onDisplayLayerDestroy(Display* display, Layer* layer) {
    const auto displayId = display->getId();
    DEBUG_LOG("%s display:%d", __FUNCTION__, displayId);

    mG2dComposer->onDisplayLayerDestroy(displayId, layer);

    return HWC3::Error::None;
}

HWC3::Error VirtualFrameComposer::onDisplayOutputBufferSet(Display* display) {
    const auto displayId = display->getId();
    DEBUG_LOG("%s display:%d", __FUNCTION__, displayId);

    auto it = mDisplays.find(displayId);
    if (it == mDisplays.end()) {
        ALOGE("%s: Not find display %d", __FUNCTION__, displayId);
        return HWC3::Error::BadDisplay;
    }

    return HWC3::Error::None;
}

HWC3::Error VirtualFrameComposer::onDisplayClientTargetSet(Display* display) {
    const auto displayId = display->getId();
    DEBUG_LOG("%s display:%d", __FUNCTION__, displayId);

    auto it = mDisplays.find(displayId);
    if (it == mDisplays.end()) {
        ALOGE("%s: Not find display %d", __FUNCTION__, displayId);
        return HWC3::Error::BadDisplay;
    }

    auto& displayBuffer = mDisplays[displayId];
    displayBuffer.forceHwcCopy = true;

    return HWC3::Error::None;
}

HWC3::Error VirtualFrameComposer::onActiveConfigChange(Display* display, int32_t configId) {
    DEBUG_LOG("%s display:%d", __FUNCTION__, display->getId());

    return HWC3::Error::None;
};

HWC3::Error VirtualFrameComposer::validateDisplay(Display* display, DisplayChanges* outChanges) {
    const auto displayId = display->getId();
    DEBUG_LOG("%s display:%d", __FUNCTION__, displayId);

    auto it = mDisplays.find(displayId);
    if (it == mDisplays.end()) {
        ALOGE("%s: Not find display %d", __FUNCTION__, displayId);
        return HWC3::Error::BadDisplay;
    }

    auto& layersForComposition = mDisplayLayers[displayId].layersForComposition;
    const std::vector<Layer*>& layers = display->getOrderedLayers();

    bool deviceComposition = true;
    bool mustDeviceComposition = false;
    bool fallBackToClient = false;

    for (Layer* layer : layers) {
        const auto layerId = layer->getId();
        const auto composeType = layer->getCompositionType();
        if ((int)composeType == Composition_NXP_PRIVATE || composeType == Composition::INVALID)
            continue;

        layersForComposition.push_back(layer);
        if (layer->hasLuts()) {
            fallBackToClient = true;
        }

        if (mG2dComposer->isValid()) {
            // if some layer cannot support, not use device composition
            deviceComposition = deviceComposition && mG2dComposer->checkDeviceComposition(layer);
            mustDeviceComposition =
                    mustDeviceComposition || mG2dComposer->checkMustDeviceComposition(layer);
        }
    }

    if (!mG2dComposer->isValid() ||
        (!mustDeviceComposition &&
         (!deviceComposition || fallBackToClient ||
          display->getColorTransformHint() != common::ColorTransform::IDENTITY))) {
        /* currently Device Composer(G2D/DPU) cannot process color transform */
        for (auto& layer : layersForComposition) {
            const auto layerId = layer->getId();
            const auto layerCompositionType = layer->getCompositionType();

            if (layerCompositionType != Composition::CLIENT) {
                outChanges->addLayerCompositionChange(display->getHwcId(), layerId,
                                                      Composition::CLIENT);
            }
        }
        layersForComposition.clear();
    } else {
        for (auto& layer : layersForComposition) {
            const auto layerId = layer->getId();
            const auto layerCompositionType = layer->getCompositionType();

            if (layerCompositionType == Composition::CLIENT) {
                outChanges->addLayerCompositionChange(display->getHwcId(), layerId,
                                                      Composition::DEVICE);
            }
        }
    }

    return HWC3::Error::None;
}

HWC3::Error VirtualFrameComposer::presentDisplay(
        Display* display, ::android::base::unique_fd* outDisplayFence,
        std::unordered_map<int64_t, ::android::base::unique_fd>* outLayerFences) {
    const auto displayId = display->getId();
    DEBUG_LOG("%s display:%d", __FUNCTION__, displayId);

    auto displayBufferIt = mDisplays.find(displayId);
    if (displayBufferIt == mDisplays.end()) {
        ALOGE("%s: failed to find display buffers for display:%d", __FUNCTION__, displayId);
        return HWC3::Error::BadDisplay;
    }
    auto& displayBuffer = displayBufferIt->second;

    auto& outBuffer = display->getOutputBuffer();
    ::android::base::unique_fd fence = outBuffer.getFence();
    if (fence.ok()) {
        int err = sync_wait(fence.get(), 300);
        if (err < 0 && errno == ETIME) {
            ALOGE("%s waited on acquire fence %" PRId32 " for 3000 ms", __FUNCTION__, fence.get());
        }
    }
    displayBuffer.outputBuffer = outBuffer.getBuffer();
    if (getInfoFromHandle(displayBuffer.outputBuffer, &displayBuffer.outputInfo) != 0)
        return HWC3::Error::BadParameter;


    auto& layersForComposition = mDisplayLayers[displayId].layersForComposition;
    ::android::base::unique_fd outputFence;

    bool needConvert = false;
    buffer_handle_t cvtSrc, cvtDst, composeTarget;
    HandleInfo *cvtSrcInfo, *cvtDstInfo;
    if (displayBuffer.forceHwcCopy) {
        displayBuffer.clientBuffer = display->waitAndGetClientTargetBuffer();
        if (getInfoFromHandle(displayBuffer.clientBuffer, &displayBuffer.clientInfo) != 0)
            return HWC3::Error::BadParameter;

        cvtSrc = displayBuffer.clientBuffer;
        cvtSrcInfo = &displayBuffer.clientInfo;
#ifdef RGBA_RB_SWAPPED
        // seems the FB from GPU composition is R/B swapped, need to swap back
        if (cvtSrcInfo->drm_format == DRM_FORMAT_ABGR8888)
            cvtSrcInfo->drm_format = DRM_FORMAT_ARGB8888;
#endif
        cvtDst = displayBuffer.outputBuffer;
        cvtDstInfo = &displayBuffer.outputInfo;
        needConvert = true;
    } else if (layersForComposition.size() > 0) { // need to compose layers
        if (displayBuffer.outputInfo.format != displayBuffer.convertInfo.format) {
            // need to do composition, then convert RGBA -> YUV
            composeTarget = displayBuffer.convertBuffer;
            cvtSrc = composeTarget;
            cvtSrcInfo = &displayBuffer.convertInfo;
            cvtDst = displayBuffer.outputBuffer;
            cvtDstInfo = &displayBuffer.outputInfo;
            needConvert = true;
        } else { // compose layers to output buffer directly
            composeTarget = displayBuffer.outputBuffer;
        }
    }

    if (layersForComposition.size() > 0) {
#ifdef DEBUG_DUMP_VIRT_G2D_CONSUMPTION
        nsecs_t g2dStart = systemTime(CLOCK_MONOTONIC);
#endif
        auto [ret, composeFence] =
                mG2dComposer->composeLayers(displayId, layersForComposition, composeTarget);
        if (ret) {
            outputFence = std::move(composeFence);
        } else {
            ALOGE("%s display %d G2D composition failed", __FUNCTION__, displayId);
            outputFence = ::android::base::unique_fd();
        }
#ifdef DEBUG_DUMP_VIRT_G2D_CONSUMPTION
        nsecs_t g2dEnd = systemTime(CLOCK_MONOTONIC);
        ALOGI("%s: G2d compose buffer for virtual display cost %3.3fms", __func__,
              (g2dEnd - g2dStart) / 1000000.0);
#endif
    } else {
        outputFence = ::android::base::unique_fd();
    }

    if (needConvert) {
        bool useOcl = false; // not use opencl by default
#ifdef G2D_LIMITATION_VIV
        useOcl = true; // G2D-VIV don't support RGBA8888->NV12 format conversion, use OpenCL
#endif
        // Need to wait composition complete fence before do CSC with opencl
        if (useOcl && outputFence.ok()) {
            int err = sync_wait(outputFence.get(), 3000);
            if (err < 0 && errno == ETIME) {
                ALOGE("%s waited on output fence %" PRId32 " for 3000 ms", __FUNCTION__,
                      outputFence.get());
            }
        }
        mG2dComposer->convertBuffer(cvtSrc, cvtSrcInfo, cvtDst, cvtDstInfo, useOcl);
    }

    *outDisplayFence = std::move(outputFence);

    layersForComposition.clear();
    displayBuffer.forceHwcCopy = false;

    return HWC3::Error::None;
}

HWC3::Error VirtualFrameComposer::getClientTargetProperty(Display* display,
                                                          ClientTargetProperty* outProperty) {
    DEBUG_LOG("%s display:%d", __FUNCTION__, display->getId());

    outProperty->pixelFormat = common::PixelFormat::RGBA_8888;
    outProperty->dataspace = common::Dataspace::SRGB_LINEAR;

    return HWC3::Error::None;
}

} // namespace aidl::android::hardware::graphics::composer3::impl
