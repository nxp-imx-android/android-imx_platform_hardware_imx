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

#ifndef _G2D_DEVICE_H
#define _G2D_DEVICE_H

#include <g2d.h>
#include <g2dExt.h>
#include <utils/Mutex.h>

#include "ImageProcessDevice.h"

namespace fsl {

using namespace android;

typedef int (*hwc_func1)(void* handle);
typedef int (*hwc_func2)(void* handle, void* arg1);
typedef int (*hwc_func3)(void* handle, void* arg1, void* arg2);
typedef int (*hwc_func4)(void* handle, void* arg1, void* arg2, void* arg3);
typedef int (*hwc_query)(void*, enum g2d_feature, int*);
typedef int (*hwc_enable)(void*, enum g2d_cap_mode);
typedef int (*hwc_disable)(void*, enum g2d_cap_mode);
typedef struct g2d_buf* (*hwc_alloc)(int, int);
typedef int (*hwc_free)(struct g2d_buf*);
typedef int (*hwc_get_coord_from_dct)(void*, const char*, struct g2d_buf*,
                                      struct g2d_warp_coordinates*);

struct DewarpCtx {
    struct g2d_warp_coordinates coord;
    struct g2d_buf* g2d_coord_buf;
    bool enable;
};

// G2D/DPU image processing device.
// Handles 2D blitter operations via the g2d library.
class G2dDevice : public ImageProcessDevice {
public:
    // @param engine: ENG_G2D or ENG_DPU
    explicit G2dDevice(ImxEngine engine);
    ~G2dDevice() override;

    ImxEngine getEngine() const override { return mEngine; }
    const char* getName() const override;

    static float g2d_sniff(uint32_t srcFormat, uint32_t dstFormat, uint32_t srcWidth,
                            uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight);
    static float dpu_sniff(uint32_t srcFormat, uint32_t dstFormat, uint32_t srcWidth,
                            uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight);

    int convert(ImxImageBuffer& dst, ImxImageBuffer& src) override;

private:
    bool init();
    static bool getDefaultG2DLib(char* libName, int size, bool* pVIVG2D);
    int openEngine(void** handle);
    int closeEngine(void* handle);
    void* getHandle();
    void LockG2dAddr(ImxImageBuffer& imxBuf);
    void UnLockG2dAddr(ImxImageBuffer& imxBuf);

    int ConvertByG2DCopy(ImxImageBuffer& dst, ImxImageBuffer& src);
    int ConvertByG2DBlit(ImxImageBuffer& dst, ImxImageBuffer& src);
    int PrepareDewarpBinary();

    static int convertPixelFormatToG2DFormat(int format);

    ImxEngine mEngine;
    bool mInitialized;
    bool mbVIVG2D;

    void* mG2dModule;
    void* mG2dHandle;
    hwc_func1 mOpenEngine;
    hwc_func1 mCloseEngine;
    hwc_func1 mFinishEngine;
    hwc_func4 mCopyEngine;
    hwc_func3 mBlitEngine;
    Mutex mG2dLock;

    void* mHelperHandle;
    hwc_func1 mLockSurface;
    hwc_func1 mUnlockSurface;

    // Dewarp
    DewarpCtx mDewarpCtx;
    hwc_query mQueryFeature;
    hwc_func2 mSetWarpCord;
    hwc_enable mEnableEngine;
    hwc_disable mDisableEngine;
    hwc_alloc mAlloc;
    hwc_free mFree;
    hwc_get_coord_from_dct mGetCoordFromDct;
};

} // namespace fsl

#endif // _G2D_DEVICE_H
