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

#ifndef _DEVICE_COMPOSER_H_
#define _DEVICE_COMPOSER_H_

#include <g2dExt.h>
#include <utils/threads.h>

#include "BufferInfo.h"
#include "Layer.h"
#include "OpenCLConverter.h"
#include "gralloc_handle.h"

namespace aidl::android::hardware::graphics::composer3::impl {

typedef int (*hwc_func1)(void* handle);
typedef int (*hwc_func2)(void* handle, void* arg1);
typedef int (*hwc_func3)(void* handle, void* arg1, void* arg2);
typedef int (*hwc_func4)(void* handle, void* arg1, void* arg2, void* arg3);
typedef int (*hwc_func5)(void* handle, void* arg1, void* arg2, void* arg3, void* arg4);
typedef void* (*hwc_buf_func)(void* arg1);

enum {
    G2D_CACHE_TYPE_NONE,
    G2D_CACHE_TYPE_SCALING,
    G2D_CACHE_TYPE_ROTATION,
    G2D_CACHE_TYPE_CSC, // format conversion
};

struct G2dInterBuffer {
    int64_t layerId;
    int type;
    buffer_handle_t hnd;
    HandleInfo info;
};

struct G2dBuffer {
    buffer_handle_t hnd;
    HandleInfo info;
    uint64_t originPhys; // used in lockSurface()/unlockSurface()
    G2dInterBuffer* interPtr;
};

using ::android::Mutex;

class DeviceComposer {
public:
    DeviceComposer();
    ~DeviceComposer();

    bool isValid();
    int alignTile(int* width, int* height, int format, int usage);

    bool checkMustDeviceComposition(Layer* layer);
    bool checkDeviceComposition(Layer* layer);
    int prepareDeviceFrameBuffer(uint32_t width, uint32_t height, uint32_t format,
                                 std::vector<buffer_handle_t>& buffers, uint32_t count,
                                 bool secure);
    int freeDeviceFrameBuffer(std::vector<buffer_handle_t>& buffers);
    int freeSolidColorBuffer();
    int onLayerDestroy(Layer* layer);

    std::tuple<bool, ::android::base::unique_fd> composeLayers(std::vector<Layer*> layers,
                                                               buffer_handle_t target);

private:
    void* getHandle();

    // clear worm hole introduced by layers not cover whole screen.
    int clearWormHole(std::vector<Layer*>& layers, G2dBuffer& target);
    G2dInterBuffer* preComposition(Layer* layer, buffer_handle_t handle);
    // compose display layer.
    int composeLayerLocked(Layer* layer, G2dBuffer& layerBuffer, G2dBuffer& targetBuffer,
                           bool bypass);
    // sync 2D blit engine.
    int finishComposite();
    bool isFeatureSupported(g2d_feature feature);

    int setG2dSurface(struct g2d_surfaceEx& surfaceX, G2dBuffer& buff, common::Rect& rect);
    enum g2d_format convertFormat(uint32_t format, G2dBuffer& buff);
    int convertRotation(common::Transform transform, struct g2d_surface& src,
                        struct g2d_surface& dst);
    int convertBlending(common::BlendMode blending, struct g2d_surface& src,
                        struct g2d_surface& dst);
    int prepareSolidColorBuffer(G2dBuffer& target);
    int prepareG2dTempBuffer(G2dBuffer& srcBuffer, uint32_t newFormat, G2dBuffer* tempBuffer);
    int clearRect(G2dBuffer& buff, common::Rect& rect);

    int getAlignedSize(G2dBuffer& buff, int* width, int* height);
    int getFlipOffset(G2dBuffer& buff, uint32_t* offset);
    int getTiling(G2dBuffer& buff, enum g2d_tiling* tile);
    enum g2d_format alterFormat(G2dBuffer& buff, enum g2d_format format);
    int getTileStatus(G2dBuffer& buff, struct g2d_surfaceEx* surfaceX);
    int resolveTileStatus(G2dBuffer& buff);
    // lock surface to get GPU specific resource.
    int lockSurface(G2dBuffer& buff);
    // unlock surface to release resource.
    int unlockSurface(G2dBuffer& buff);

    int setClipping(common::Rect& src, common::Rect& dst, common::Rect& clip,
                    common::Transform rotation);
    int blitSurface(struct g2d_surfaceEx* srcEx, struct g2d_surfaceEx* dstEx);
    int openEngine(void** handle);
    int closeEngine(void* handle);
    int clearFunction(void* handle, struct g2d_surface* area);
    int enableFunction(void* handle, enum g2d_cap_mode cap, bool enable);
    int finishEngine(void* handle);
    int getBuffPhys(G2dBuffer& buff, uint64_t* phys);
    int createFenceFd(void* handle);

private:
    static Mutex sLock;
    static thread_local void* sHandle;

    bool mG2dPrefered;

    G2dBuffer mTarget;
    G2dBuffer mSolidColorBuffer;
#ifdef G2D_FORMAT_CONVERSION
    G2dBuffer mG2dConvertBuffer;
#endif
    std::unordered_map<uint64_t, G2dInterBuffer> mG2dCachedBuffers;

    hwc_func3 mGetAlignedSize;
    hwc_func2 mGetFlipOffset;
    hwc_func2 mGetTiling;
    hwc_func2 mAlterFormat;
    hwc_func1 mLockSurface;
    hwc_func1 mUnlockSurface;
    hwc_func4 mAlignTile;
    hwc_func2 mGetTileStatus;
    hwc_func1 mResolveTileStatus;

    hwc_func5 mSetClipping;
    hwc_func3 mBlitFunction;
    hwc_func1 mOpenEngine;
    hwc_func1 mCloseEngine;
    hwc_func2 mClearFunction;
    hwc_func2 mEnableFunction;
    hwc_func2 mDisableFunction;
    hwc_func1 mFinishEngine;
    hwc_func3 mQueryFeature;
    hwc_buf_func mBuffInfoFromFd;
    hwc_func1 mCreateFenceFd;

    void* mHelperHandle = NULL;
    void* mG2dHandle = NULL;
    std::unique_ptr<OclConverter> mOclCvt;
};

} // namespace aidl::android::hardware::graphics::composer3::impl
#endif
