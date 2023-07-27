/*
 * Copyright (C) 2016 The Android Open Source Project
 * Copyright 2019-2023 NXP.
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

#include "EvsDisplay.h"

#include <sync/sync.h>
#include <ui/DisplayMode.h>
#include <ui/DisplayState.h>

#include <ui/GraphicBufferAllocator.h>
#include <ui/GraphicBufferMapper.h>

#include "gralloc_handle.h"
#include "gralloc_metadata.h"
#include "gralloc_driver.h"

namespace android {
namespace hardware {
namespace automotive {
namespace evs {
namespace V1_1 {
namespace implementation {

using namespace android;
using ::nxp::hardware::display::V1_0::Error;
using ::android::frameworks::automotive::display::V1_0::HwDisplayConfig;
using ::android::frameworks::automotive::display::V1_0::HwDisplayState;
using aidl::android::hardware::graphics::common::BlendMode;
using aidl::android::hardware::graphics::common::Dataspace;

#define DISPLAY_WIDTH 1280
#define DISPLAY_HEIGHT 720

/**
 * Getter for IDisplay service. This function is called outside of EvsDisplay
 * constructor since it depends on DisplayManager and it can be costly
 * operation during early booting process.
 */
sp<IDisplay> EvsDisplay::getDisplay()
{
    if (mDisplay == nullptr) {
        //
        //  Create the native full screen window and get a suitable configuration to match it
        //
        uint32_t layer = -1;
        sp<IDisplay> display = nullptr;
        while (display.get() == nullptr) {
            display = IDisplay::getService();
            if (display.get() == nullptr) {
                ALOGE("%s get display service failed", __func__);
                usleep(200000);
            }
        }

        display->getLayer(DISPLAY_BUFFER_NUM,
            [&](const auto& tmpError, const auto& tmpLayer) {
                if (tmpError == Error::NONE) {
                    layer = tmpLayer;
                }
        });

        if (layer == (uint32_t)-1) {
            ALOGE("%s get layer failed", __func__);
            return nullptr;
        }

        {
            std::unique_lock<std::mutex> lock(mLock);
            mIndex = 0;
            mDisplay = display;
            mLayer = layer;
        }
    }
    return mDisplay;
}

EvsDisplay::EvsDisplay(sp<IAutomotiveDisplayProxyService> pDisplayProxy, uint64_t displayId)
    : mDisplayProxy(pDisplayProxy),
      mDisplayId(displayId) {
    ALOGD("EvsDisplay instantiated");

    // Set up our self description
    // NOTE:  These are arbitrary values chosen for testing
    mInfo.displayId             = "evs hal Display";
    mInfo.vendorFlags           = 3870;
}

EvsDisplay::EvsDisplay()
{
    ALOGD("EvsDisplay instantiated");

    // Set up our self description
    // NOTE:  These are arbitrary values chosen for testing
    mInfo.displayId   = "evs hal Display";
    mInfo.vendorFlags = 3870;

    mWidth = DISPLAY_WIDTH;
    mHeight = DISPLAY_HEIGHT;
    mFormat = HAL_PIXEL_FORMAT_RGBA_8888;

    initialize();
}

EvsDisplay::~EvsDisplay()
{
    LOG(DEBUG) << "EvsGlDisplay being destroyed";
    forceShutdown();
}

void EvsDisplay::showWindow(sp<IAutomotiveDisplayProxyService>& pWindowProxy, uint64_t id)
{
    LOG(INFO) << __FUNCTION__ << "window is showing";
    if (pWindowProxy != nullptr) {
        pWindowProxy->showWindow(id);
    }
}


void EvsDisplay::hideWindow(sp<IAutomotiveDisplayProxyService>& pWindowProxy, uint64_t id)
{
    if (pWindowProxy != nullptr) {
        pWindowProxy->hideWindow(id);
    }
}

// Main entry point
bool EvsDisplay::initialize()
{
    // allocate memory.
    buffer_handle_t buffer;
    struct gralloc_buffer_descriptor desc;
    gralloc_driver driver;
    driver.init();

    desc.width = mWidth;
    desc.height = mHeight;
    desc.droid_format = mFormat;
    desc.droid_usage = fsl::USAGE_HW_TEXTURE | fsl::USAGE_HW_RENDER | fsl::USAGE_HW_VIDEO_ENCODER;
    desc.drm_format = 0x0;
    desc.use_flags = 0x0;
    desc.reserved_region_size = sizeof(gralloc_metadata);

    for (int i = 0; i < DISPLAY_BUFFER_NUM; i++) {
        buffer = nullptr;

        desc.name = "EVS Display Buf" + std::to_string(i);
        int ret = driver.allocate(&desc, &buffer);
        if (ret) {
            ALOGE("Failed to allocate EVS Display buffers.");
            return false;
        }

        void* reservedRegionAddr = nullptr;
        uint64_t reservedRegionSize = 0;
        ret = driver.get_reserved_region(buffer, &reservedRegionAddr, &reservedRegionSize);
        if (ret) {
            driver.release(buffer);
            ALOGE("Failed to initializeMetadata. Failed to getReservedRegion.");
            return false;
        }
        gralloc_metadata* grallocMetadata = reinterpret_cast<gralloc_metadata*>(reservedRegionAddr);
        snprintf(grallocMetadata->name, GRALLOC_METADATA_MAX_NAME_SIZE, "%s", desc.name.c_str());
        grallocMetadata->dataspace = Dataspace::UNKNOWN;
        grallocMetadata->blendMode = BlendMode::INVALID;

        std::unique_lock<std::mutex> lock(mLock);
        mBuffers[i] = (fsl::Memory*) buffer;
    }

    return true;
}

/**
 * This gets called if another caller "steals" ownership of the display
 */
void EvsDisplay::forceShutdown()
{
    LOG(DEBUG) << "EvsGlDisplay forceShutdown";

    if (mDisplayProxy != nullptr) {
        std::lock_guard<std::mutex> lock(mLock);

        // If the buffer isn't being held by a remote client, release it now as an
        // optimization to release the resources more quickly than the destructor might
        // get called.
        if (mBuffer.memHandle) {
            // Report if we're going away while a buffer is outstanding
            if (mFrameBusy) {
                LOG(ERROR) << "EvsGlDisplay going down while client is holding a buffer";
            }

            // Drop the graphics buffer we've been using
            GraphicBufferAllocator& alloc(GraphicBufferAllocator::get());
            alloc.free(mBuffer.memHandle);
            mBuffer.memHandle = nullptr;

            mGlWrapper.hideWindow(mDisplayProxy, mDisplayId);
            mGlWrapper.shutdown();
        }

        // Put this object into an unrecoverable error state since somebody else
        // is going to own the display now.
        mRequestedState = EvsDisplayState::DEAD;
    } else {
        int layer;
        sp<IDisplay> display = getDisplay();
        {
            std::unique_lock<std::mutex> lock(mLock);
            layer = mLayer;
            mLayer = -1;
            display = mDisplay;
        }

        if (display != nullptr) {
            display->putLayer(layer);
        }

        buffer_handle_t buffer;
        struct gralloc_buffer_descriptor desc;
        gralloc_driver driver;
        driver.init();

        for (int i = 0; i < DISPLAY_BUFFER_NUM; i++) {
            {
                std::unique_lock<std::mutex> lock(mLock);
                if (mBuffers[i] == nullptr) {
                    continue;
                }

                buffer = mBuffers[i];
                mBuffers[i] = nullptr;
            }
            driver.release(buffer);
        }

        std::lock_guard<std::mutex> lock(mLock);
        // Put this object into an unrecoverable error state since somebody else
        // is going to own the display now.
        mRequestedState = EvsDisplayState::DEAD;
    }
}

/**
 * Returns basic information about the EVS display provided by the system.
 * See the description of the DisplayDesc structure for details.
 */
Return<void> EvsDisplay::getDisplayInfo(getDisplayInfo_cb _hidl_cb) {
    LOG(DEBUG) << __FUNCTION__;

    // Send back our self description
    _hidl_cb(mInfo);
    return Void();
}

/**
 * Clients may set the display state to express their desired state.
 * The HAL implementation must gracefully accept a request for any state
 * while in any other state, although the response may be to ignore the request.
 * The display is defined to start in the NOT_VISIBLE state upon initialization.
 * The client is then expected to request the VISIBLE_ON_NEXT_FRAME state, and
 * then begin providing video.  When the display is no longer required, the client
 * is expected to request the NOT_VISIBLE state after passing the last video frame.
 */
Return<EvsResult> EvsDisplay::setDisplayState(EvsDisplayState state) {
    LOG(DEBUG) << __FUNCTION__;
    std::lock_guard<std::mutex> lock(mLock);

    if (mRequestedState == EvsDisplayState::DEAD) {
        // This object no longer owns the display -- it's been superceeded!
        return EvsResult::OWNERSHIP_LOST;
    }

    // Ensure we recognize the requested state so we don't go off the rails
    if (state >= EvsDisplayState::NUM_STATES) {
        return EvsResult::INVALID_ARG;
    }

    switch (state) {
    case EvsDisplayState::NOT_VISIBLE:
        hideWindow(mDisplayProxy, mDisplayId);
        break;
    case EvsDisplayState::VISIBLE:
        showWindow(mDisplayProxy, mDisplayId);
        break;
    default:
        break;
    }

    // Record the requested state
    mRequestedState = state;

    return EvsResult::OK;
}


/**
 * The HAL implementation should report the actual current state, which might
 * transiently differ from the most recently requested state.  Note, however, that
 * the logic responsible for changing display states should generally live above
 * the device layer, making it undesirable for the HAL implementation to
 * spontaneously change display states.
 */
Return<EvsDisplayState> EvsDisplay::getDisplayState() {
    LOG(DEBUG) << __FUNCTION__;
    std::lock_guard<std::mutex> lock(mLock);

    return mRequestedState;
}

/**
 * This call returns a handle to a frame buffer associated with the display.
 * This buffer may be locked and written to by software and/or GL.  This buffer
 * must be returned via a call to returnTargetBufferForDisplay() even if the
 * display is no longer visible.
 */
Return<void> EvsDisplay::getTargetBuffer(getTargetBuffer_cb _hidl_cb) {
    LOG(DEBUG) << __FUNCTION__;

    if (mDisplayProxy != nullptr) {
        std::lock_guard<std::mutex> lock(mLock);

        if (mRequestedState == EvsDisplayState::DEAD) {
            LOG(ERROR) << "Rejecting buffer request from object that lost ownership of the display.";
            _hidl_cb({});
            return Void();
         }

        // If we don't already have a buffer, allocate one now
        if (!mBuffer.memHandle) {
            // Initialize our display window
            // NOTE:  This will cause the display to become "VISIBLE" before a frame is actually
            // returned, which is contrary to the spec and will likely result in a black frame being
            // (briefly) shown.
            if (!mGlWrapper.initialize(mDisplayProxy, mDisplayId)) {
                // Report the failure
                LOG(ERROR) << "Failed to initialize GL display";
                _hidl_cb({});
                return Void();
            }

            // Assemble the buffer description we'll use for our render target
            mBuffer.width       = mGlWrapper.getWidth();
            mBuffer.height      = mGlWrapper.getHeight();
            mBuffer.format      = HAL_PIXEL_FORMAT_RGBA_8888;
            mBuffer.usage       = GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_VIDEO_ENCODER;
            mBuffer.bufferId    = 0x3870;  // Arbitrary magic number for self recognition
            mBuffer.pixelSize   = 4;

            // Allocate the buffer that will hold our displayable image
            buffer_handle_t handle = nullptr;
            GraphicBufferAllocator& alloc(GraphicBufferAllocator::get());
            status_t result = alloc.allocate(mBuffer.width, mBuffer.height,
                                             mBuffer.format, 1,
                                             mBuffer.usage, &handle,
                                             &mBuffer.stride,
                                             0, "EvsGlDisplay");
            if (result != NO_ERROR) {
                LOG(ERROR) << "Error " << result
                           << " allocating " << mBuffer.width << " x " << mBuffer.height
                           << " graphics buffer.";
                _hidl_cb({});
                mGlWrapper.shutdown();
                return Void();
            }
            if (!handle) {
            LOG(ERROR) << "We didn't get a buffer handle back from the allocator";
                _hidl_cb({});
                mGlWrapper.shutdown();
                return Void();
            }

            mBuffer.memHandle = handle;
            LOG(DEBUG) << "Allocated new buffer " << mBuffer.memHandle.getNativeHandle()
                       << " with stride " <<  mBuffer.stride;
            mFrameBusy = false;
        }

        // Do we have a frame available?
        if (mFrameBusy) {
            // This means either we have a 2nd client trying to compete for buffers
            // (an unsupported mode of operation) or else the client hasn't returned
            // a previously issued buffer yet (they're behaving badly).
            // NOTE:  We have to make the callback even if we have nothing to provide
            LOG(ERROR) << "getTargetBuffer called while no buffers available.";
            _hidl_cb({});
            return Void();
        } else {
            // Mark our buffer as busy
            mFrameBusy = true;

            // Send the buffer to the client
            LOG(VERBOSE) << "Providing display buffer handle " << mBuffer.memHandle.getNativeHandle()
                         << " as id " << mBuffer.bufferId;
            _hidl_cb(mBuffer);
            return Void();
        }
    } else {
        BufferDesc_1_0 hbuf = {};
        {
            std::lock_guard<std::mutex> lock(mLock);

            if (mRequestedState == EvsDisplayState::DEAD) {
                ALOGE("Rejecting buffer request from object that lost ownership of the display.");
                _hidl_cb(hbuf);
                return Void();
            }
        }

        int layer;
        uint32_t slot = -1;
        sp<IDisplay> display = getDisplay();
        {
            std::lock_guard<std::mutex> lock(mLock);
            display = mDisplay;
            layer = mLayer;
        }
        if (display == nullptr)  {
            ALOGE("%s invalid display", __func__);
            _hidl_cb(hbuf);
            return Void();
        }

        display->getSlot(layer, [&](const auto& tmpError, const auto& tmpSlot) {
            if (tmpError == Error::NONE) {
                slot = tmpSlot;
            }
        });

        if (slot == (uint32_t)-1) {
            ALOGE("%s get slot failed", __func__);
            _hidl_cb(hbuf);
            return Void();
        }

        fsl::Memory *buffer = nullptr;
        {
            std::lock_guard<std::mutex> lock(mLock);
            if (mBuffers[slot] == nullptr) {
                ALOGE("%s can't find valid buffer", __func__);
                _hidl_cb(hbuf);
                return Void();
            }
            buffer = mBuffers[slot];
        }

        // Assemble the buffer description we'll use for our render target
        // hard code the resolution 640*480
        hbuf.width     = buffer->width;
        hbuf.height    = buffer->height;
        hbuf.stride    = buffer->stride;
        hbuf.format    = buffer->format;
        hbuf.usage     = buffer->usage;
        hbuf.bufferId  = slot;
        hbuf.pixelSize = 4;
        hbuf.memHandle = buffer;

        // Send the buffer to the client
        ALOGV("Providing display buffer handle %p as id %d",
              hbuf.memHandle.getNativeHandle(), hbuf.bufferId);
        _hidl_cb(hbuf);
        return Void();
    }
}


/**
 * This call tells the display that the buffer is ready for display.
 * The buffer is no longer valid for use by the client after this call.
 */
Return<EvsResult> EvsDisplay::returnTargetBufferForDisplay(const BufferDesc_1_0& buffer) {
    LOG(VERBOSE) << __FUNCTION__ << " " << buffer.memHandle.getNativeHandle();

    // Nobody should call us with a null handle
    if (!buffer.memHandle.getNativeHandle()) {
        LOG(ERROR) << __FUNCTION__
                   << " called without a valid buffer handle.";
        return EvsResult::INVALID_ARG;
    }

    if (mDisplayProxy != nullptr) {
        std::lock_guard<std::mutex> lock(mLock);
        if (buffer.bufferId != mBuffer.bufferId) {
            LOG(ERROR) << "Got an unrecognized frame returned.";
            return EvsResult::INVALID_ARG;
        }
        if (!mFrameBusy) {
            LOG(ERROR) << "A frame was returned with no outstanding frames.";
            return EvsResult::BUFFER_NOT_AVAILABLE;
        }

        mFrameBusy = false;

        // If we've been displaced by another owner of the display, then we can't do anything else
        if (mRequestedState == EvsDisplayState::DEAD) {
            return EvsResult::OWNERSHIP_LOST;
        }

        // If we were waiting for a new frame, this is it!
        if (mRequestedState == EvsDisplayState::VISIBLE_ON_NEXT_FRAME) {
            mRequestedState = EvsDisplayState::VISIBLE;
            mGlWrapper.showWindow(mDisplayProxy, mDisplayId);
        }

        // Validate we're in an expected state
        if (mRequestedState != EvsDisplayState::VISIBLE) {
            // Not sure why a client would send frames back when we're not visible.
            LOG(WARNING) << "Got a frame returned while not visible - ignoring.";
        } else {
            // Update the texture contents with the provided data
    // TODO:  Why doesn't it work to pass in the buffer handle we got from HIDL?
    //        if (!mGlWrapper.updateImageTexture(buffer)) {
            if (!mGlWrapper.updateImageTexture(mBuffer)) {
                return EvsResult::UNDERLYING_SERVICE_ERROR;
            }

            // Put the image on the screen
            mGlWrapper.renderImageToScreen();
            #ifdef EVS_DEBUG
            if (!sDebugFirstFrameDisplayed) {
                LOG(DEBUG) << "EvsFirstFrameDisplayTiming start time: "
                           << elapsedRealtime() << " ms.";
                sDebugFirstFrameDisplayed = true;
            }
            #endif
        }
    } else {
        if (buffer.bufferId >= DISPLAY_BUFFER_NUM) {
            ALOGE ("%s invalid buffer id.\n", __func__);
            return EvsResult::INVALID_ARG;
        }

        EvsDisplayState state;
        sp<IDisplay> display = getDisplay();
        fsl::Memory *abuffer = nullptr;
        int layer;
        {
            std::lock_guard<std::mutex> lock(mLock);
            state = mRequestedState;
            abuffer = mBuffers[buffer.bufferId];
            display = mDisplay;
            layer = mLayer;
        }

        if (abuffer == nullptr) {
            ALOGE ("%s abuffer invalid.\n", __func__);
            return EvsResult::INVALID_ARG;
        }

        if (display != nullptr) {
            display->presentLayer(layer, buffer.bufferId, abuffer);
        }

        // If we've been displaced by another owner of the display, then we can't do anything else
        if (state == EvsDisplayState::DEAD) {
            return EvsResult::OWNERSHIP_LOST;
        }

        // If we were waiting for a new frame, this is it!
        if (state == EvsDisplayState::VISIBLE_ON_NEXT_FRAME) {
            //showWindow();
            std::lock_guard<std::mutex> lock(mLock);
            mRequestedState = EvsDisplayState::VISIBLE;
        }

        // Validate we're in an expected state
        if (state != EvsDisplayState::VISIBLE) {
            // Not sure why a client would send frames back when we're not visible.
            ALOGW("Got a frame returned while not visible - ignoring.\n");
        }
        else {
            ALOGV("Got a visible frame %d returned.\n", buffer.bufferId);
        }
    }

    return EvsResult::OK;
}

Return<void> EvsDisplay::getDisplayInfo_1_1(__attribute__ ((unused))getDisplayInfo_1_1_cb _info_cb) {
    android::ui::DisplayMode displayMode;
    android::ui::DisplayState displayState;
    HwDisplayConfig activeConfig;
    HwDisplayState  activeState;

    if (mDisplayProxy != nullptr) {
        return mDisplayProxy->getDisplayInfo(mDisplayId, _info_cb);
    } else {
        if (getDisplay() == nullptr) {
            _info_cb(activeConfig, activeState);
            return Void();
        }
        displayMode.resolution = ui::Size(mWidth, mHeight);
        displayMode.refreshRate = 60.f;
        displayState.layerStack.id = mLayer;
        activeConfig.setToExternal((uint8_t*)&displayMode, sizeof(android::ui::DisplayMode));
        activeState.setToExternal((uint8_t*)&displayState, sizeof(android::ui::DisplayState));
        _info_cb(activeConfig, activeState);
        return Void();
    }
}

} // namespace implementation
} // namespace V1_1
} // namespace evs
} // namespace automotive
} // namespace hardware
} // namespace android
