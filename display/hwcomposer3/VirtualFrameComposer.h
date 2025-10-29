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

#ifndef ANDROID_HWC_VIRTUALFRAMECOMPOSER_H
#define ANDROID_HWC_VIRTUALFRAMECOMPOSER_H

#include <map>

#include "Common.h"
#include "DeviceComposer.h"
#include "Display.h"
#include "FrameComposer.h"
#include "Layer.h"

namespace aidl::android::hardware::graphics::composer3::impl {

// A frame composer which always use G2D composition
// (GPU2D/DPU/PXP to do the composition).
class VirtualFrameComposer : public FrameComposer {
public:
    VirtualFrameComposer() = default;

    VirtualFrameComposer(const VirtualFrameComposer&) = delete;
    VirtualFrameComposer& operator=(const VirtualFrameComposer&) = delete;

    VirtualFrameComposer(VirtualFrameComposer&&) = delete;
    VirtualFrameComposer& operator=(VirtualFrameComposer&&) = delete;

    HWC3::Error init(std::shared_ptr<DeviceComposer>& g2d) override;

    HWC3::Error registerOnHotplugCallback(const HotplugCallback& cb) override;
    HWC3::Error unregisterOnHotplugCallback() override;

    HWC3::Error onDisplayCreate(Display* display) override;
    HWC3::Error onDisplayDestroy(Display* display) override;
    HWC3::Error onDisplayLayerDestroy(Display* display, Layer* layer) override;

    HWC3::Error onDisplayClientTargetSet(Display* display) override;
    HWC3::Error onDisplayOutputBufferSet(Display* display) override;
    HWC3::Error onActiveConfigChange(Display* display, int32_t configId) override;

    // Determines if this composer can compose the given layers on the given
    // display and requests changes for layers that can't not be composed.
    HWC3::Error validateDisplay(Display* display, DisplayChanges* outChanges) override;

    // Performs the actual composition of layers and presents the composed result
    // to the display.
    HWC3::Error presentDisplay(
            Display* display, ::android::base::unique_fd* outDisplayFence,
            std::unordered_map<int64_t, ::android::base::unique_fd>* outLayerFences) override;

    HWC3::Error setPowerMode(Display* display, PowerMode mode) override {
        return HWC3::Error::None;
    }
    HWC3::Error setDisplayBrightness(Display* display, float brightness) override {
        return HWC3::Error::None;
    }
    HWC3::Error getDisplayConnectionType(Display* display,
                                         DisplayConnectionType* outType) override {
        return HWC3::Error::None;
    }
    HWC3::Error getClientTargetProperty(Display* display,
                                        ClientTargetProperty* outProperty) override;
    HWC3::Error waitHardwareVsyncTimestamp(Display* display, int64_t* timestamp) override {
        *timestamp = 0;
        return HWC3::Error::None;
    }

    HWC3::Error getAllDeviceClients(std::map<uint32_t, DeviceClient*>& clients) override {
        return HWC3::Error::None;
    }

    HWC3::Error startHdcp(Display* display) override { return HWC3::Error::None; }
    HWC3::Error registerOnHdcpChangedCallback(const HdcpChangedCallback& cb) override {
        return HWC3::Error::None;
    }

private:
    struct VirtualDisplay {
        int32_t width;
        int32_t height;
        common::PixelFormat format;
        uint64_t usage;
        buffer_handle_t outputBuffer; // set by framework
        HandleInfo outputInfo;
        buffer_handle_t convertBuffer; // used for format conversion
        HandleInfo convertInfo;
        bool forceHwcCopy;
        buffer_handle_t clientBuffer; // GPU composition result
        HandleInfo clientInfo;
    };
    struct ValidatedLayers {
        std::vector<Layer*> layersForComposition;
    };
    std::unordered_map<int64_t, VirtualDisplay> mDisplays;
    std::unordered_map<int64_t, ValidatedLayers> mDisplayLayers;

    std::shared_ptr<DeviceComposer> mG2dComposer;
};

} // namespace aidl::android::hardware::graphics::composer3::impl

#endif
