/*
 * Copyright 2022 The Android Open Source Project
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

#ifndef ANDROID_HWC_DISPLAYCHANGES_H
#define ANDROID_HWC_DISPLAYCHANGES_H

#include <aidl/android/hardware/graphics/composer3/ChangedCompositionLayer.h>
#include <aidl/android/hardware/graphics/composer3/ChangedCompositionTypes.h>
#include <aidl/android/hardware/graphics/composer3/DisplayRequest.h>

#include <optional>

namespace aidl::android::hardware::graphics::composer3::impl {

struct DisplayChanges {
    std::optional<ChangedCompositionTypes> compositionChanges;
    std::optional<DisplayRequest> displayRequestChanges;

    void addLayerCompositionChange(int64_t displayId, int64_t layerId,
                                   Composition layerComposition) {
        if (!compositionChanges) {
            compositionChanges.emplace();
            compositionChanges->display = displayId;
        }

        ChangedCompositionLayer compositionChange;
        compositionChange.layer = layerId;
        compositionChange.composition = layerComposition;
        compositionChanges->layers.emplace_back(std::move(compositionChange));
    }

    // Request SurfaceFlinger to clear (punch a transparent hole in) the client
    // target over this layer's region. Used when the layer is composed by a
    // hardware overlay plane that sits BELOW the client/primary plane (e.g. 8MQ
    // DCSS), so opaque client-composited UI above it would otherwise cover the
    // overlay. mask = LayerRequest::CLEAR_CLIENT_TARGET.
    void addLayerRequest(int64_t displayId, int64_t layerId, int32_t mask) {
        if (!displayRequestChanges) {
            displayRequestChanges.emplace();
            displayRequestChanges->display = displayId;
            displayRequestChanges->mask = 0;
        }

        DisplayRequest::LayerRequest layerRequest;
        layerRequest.layer = layerId;
        layerRequest.mask = mask;
        displayRequestChanges->layerRequests.emplace_back(std::move(layerRequest));
    }

    void clearLayerCompositionChanges() { compositionChanges.reset(); }

    bool hasAnyChanges() const {
        return compositionChanges.has_value() || displayRequestChanges.has_value();
    }

    void reset() {
        compositionChanges.reset();
        displayRequestChanges.reset();
    }
};

} // namespace aidl::android::hardware::graphics::composer3::impl

#endif