/*
 * Copyright 2026 NXP.
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
#define LOG_TAG "ImageProcessFactory"

#include "ImageProcessFactory.h"

#include <cutils/log.h>
#include <system/graphics.h>

#include "CpuDevice.h"
#include "G2dDevice.h"
#include "OclCvtDevice.h"

namespace fsl {

// Template helper for device creation
template <typename T>
static std::shared_ptr<ImageProcessDevice> createDeviceInstance() {
    return std::make_shared<T>();
}

static std::shared_ptr<ImageProcessDevice> createG2dDevice() {
    G2dDevice &g2dDev = G2dDevice::getInstance(ENG_G2D);
    // G2dDevice is a singleton, so the deleter does nothing.
    return std::shared_ptr<G2dDevice>(&g2dDev, [](G2dDevice*){});
}

static std::shared_ptr<ImageProcessDevice> createDpuDevice() {
    G2dDevice &dpuDev = G2dDevice::getInstance(ENG_DPU);
    // G2dDevice is a singleton, so the deleter does nothing.
    return std::shared_ptr<G2dDevice>(&dpuDev, [](G2dDevice*){});
}

static std::shared_ptr<ImageProcessDevice> createCpuDevice() {
    CpuDevice &cpuDev = CpuDevice::getInstance();
    // CpuDevice is a singleton, so the deleter does nothing.
    return std::shared_ptr<CpuDevice>(&cpuDev, [](CpuDevice*){});
}

ImageProcessFactory& ImageProcessFactory::getInstance() {
    static ImageProcessFactory sInstance;
    return sInstance;
}

ImageProcessFactory::ImageProcessFactory() {
    // Register all device types in priority order.
    // The sniff() confidence scores determine actual selection.
    mDeviceEntries = {
            {"g2d", ENG_G2D, createG2dDevice, G2dDevice::g2d_sniff},
            {"dpu", ENG_DPU, createDpuDevice, G2dDevice::dpu_sniff},
            {"oclcvt", ENG_OCLCVT, createDeviceInstance<OclCvtDevice>, OclCvtDevice::sniff},
            {"cpu", ENG_CPU, createCpuDevice, CpuDevice::sniff},
    };

    ALOGI("ImageProcessFactory: registered %zu device types", mDeviceEntries.size());
}

int ImageProcessFactory::selectBestDevice(uint32_t srcFormat, uint32_t dstFormat, uint32_t srcWidth,
                                          uint32_t srcHeight, uint32_t dstWidth,
                                          uint32_t dstHeight) {

    float bestConfidence = CONFIDENCE_NONE;
    int bestIndex = -1;

    for (size_t i = 0; i < mDeviceEntries.size(); i++) {
        float confidence = mDeviceEntries[i].sniffer(srcFormat, dstFormat, srcWidth, srcHeight,
                                                     dstWidth, dstHeight);

        ALOGV("ImageProcessFactory: sniff %s -> confidence %.2f", mDeviceEntries[i].name,
              confidence);

        if (confidence > bestConfidence) {
            bestConfidence = confidence;
            bestIndex = (int)i;
        }
    }

    if (bestIndex >= 0) {
        ALOGI("ImageProcessFactory: selected %s (confidence %.2f) for "
              "src(fmt=0x%x, %ux%u) -> dst(fmt=0x%x, %ux%u)",
              mDeviceEntries[bestIndex].name, bestConfidence, srcFormat, srcWidth, srcHeight,
              dstFormat, dstWidth, dstHeight);
    } else {
        ALOGW("ImageProcessFactory: no device found for "
              "src(fmt=0x%x, %ux%u) -> dst(fmt=0x%x, %ux%u)",
              srcFormat, srcWidth, srcHeight, dstFormat, dstWidth, dstHeight);
    }

    return bestIndex;
}

std::shared_ptr<ImageProcessDevice> ImageProcessFactory::lookupCache(const CacheKey& key) {
    std::lock_guard<std::mutex> lock(mCacheMutex);
    for (auto it = mDeviceCache.begin(); it != mDeviceCache.end(); ++it) {
        if (it->key == key) {
            // Promote to front (most recently used).
            mDeviceCache.splice(mDeviceCache.begin(), mDeviceCache, it);
            return it->device;
        }
    }
    return nullptr;
}

void ImageProcessFactory::insertCache(const CacheKey& key,
                                      std::shared_ptr<ImageProcessDevice> device) {
    std::lock_guard<std::mutex> lock(mCacheMutex);
    // Evict an entry when the cache is full.
    // Prefer to evict the entry whose shared_ptr use_count is 1 (only the
    // cache holds a reference, so no caller is actively using it).  Among
    // idle entries (use_count == 1), pick the one closest to the back of the
    // list (least recently used).  If no idle entry exists, skip eviction and
    // allow the cache to grow beyond kMaxCacheSize temporarily rather than
    // destroy a device still held by a caller.
    if (mDeviceCache.size() >= kMaxCacheSize) {
        auto victim = mDeviceCache.end();
        for (auto it = mDeviceCache.begin(); it != mDeviceCache.end(); ++it) {
            if (it->device.use_count() == 1) {
                // Always update victim so the last match (closest to back/LRU)
                // is selected.
                victim = it;
            }
        }
        if (victim != mDeviceCache.end()) {
            mDeviceCache.erase(victim);
        } else {
            dumpCache();
        }
    }
    mDeviceCache.push_front({key, std::move(device)});
}

std::shared_ptr<ImageProcessDevice> ImageProcessFactory::createDevice(
        uint32_t srcFormat, uint32_t dstFormat, uint32_t srcWidth, uint32_t srcHeight,
        uint32_t dstWidth, uint32_t dstHeight, ImxEngine engine) {

    srcFormat = unifyNV12Format(srcFormat);
    dstFormat = unifyNV12Format(dstFormat);

    // When the caller does not specify an engine, resolve the best one first so
    // the cache key always uses a concrete engine (never ENG_NOTCARE).  This
    // ensures that a subsequent call with the same parameters and ENG_NOTCARE
    // hits the cache instead of creating a new device every time.
    int resolvedIndex = -1;
    if (engine == ENG_NOTCARE) {
        resolvedIndex = selectBestDevice(srcFormat, dstFormat, srcWidth, srcHeight,
                                         dstWidth, dstHeight);
        if (resolvedIndex < 0)
            return nullptr;
        engine = mDeviceEntries[resolvedIndex].engine;
    }

    CacheKey key(srcFormat, dstFormat, srcWidth, srcHeight, dstWidth, dstHeight, engine);

    // Return a cached device when the parameters match a previous call.
    auto cached = lookupCache(key);
    if (cached) {
        ALOGV("ImageProcessFactory: cache hit for src(fmt=0x%x, %ux%u) -> dst(fmt=0x%x, %ux%u)",
              srcFormat, srcWidth, srcHeight, dstFormat, dstWidth, dstHeight);
        return cached;
    }

    std::shared_ptr<ImageProcessDevice> device;
    if (resolvedIndex >= 0) {
        // ENG_NOTCARE path: best device was already selected above.
        device = mDeviceEntries[resolvedIndex].creator();
    } else {
        //create device directly for blob source format
        if (srcFormat == HAL_PIXEL_FORMAT_BLOB && engine != ENG_NOTCARE){
            device = createDeviceByEngine(engine);
        } else {
            // Caller specified a concrete engine - verify support via sniff first.
            float confidence = 0.0f;
            for (auto& entry : mDeviceEntries) {
                if (entry.engine == engine) {
                    confidence = entry.sniffer(srcFormat, dstFormat, srcWidth, srcHeight,
                                            dstWidth, dstHeight);
                    ALOGV("ImageProcessFactory: sniff %s (requested engine) -> confidence %.2f",
                        entry.name, confidence);
                    break;
                }
            }
            if (confidence > 0.0f) {
                device = createDeviceByEngine(engine);
            } else {
                ALOGI("ImageProcessFactory: requested engine %d sniff returned %.2f, "
                    "falling back to best device selection", (int)engine, confidence);
                int index = selectBestDevice(srcFormat, dstFormat, srcWidth, srcHeight,
                                            dstWidth, dstHeight);
                if (index < 0)
                    return nullptr;
                device = mDeviceEntries[index].creator();
                // Update the key to reflect the actual engine selected by the fallback,
                // so the cached entry engine matches the device that was created.
                key = CacheKey(srcFormat, dstFormat, srcWidth, srcHeight, dstWidth, dstHeight,
                            mDeviceEntries[index].engine);
            }
        }
    }

    if (device) {
        insertCache(key, device);
    }
    return device;
}

void ImageProcessFactory::dumpCache() const {
    ALOGI("ImageProcessFactory: dumpCache -- %zu entries (max %zu)", mDeviceCache.size(),
          kMaxCacheSize);
    int idx = 0;
    for (const auto& entry : mDeviceCache) {
        uint32_t srcFmt = std::get<0>(entry.key);
        uint32_t dstFmt = std::get<1>(entry.key);
        uint32_t srcW   = std::get<2>(entry.key);
        uint32_t srcH   = std::get<3>(entry.key);
        uint32_t dstW   = std::get<4>(entry.key);
        uint32_t dstH   = std::get<5>(entry.key);
        ImxEngine eng   = std::get<6>(entry.key);

        const char* engName = "notcare";
        for (const auto& de : mDeviceEntries) {
            if (de.engine == eng) {
                engName = de.name;
                break;
            }
        }
        ALOGI("  [%d] src(fmt=0x%x, %ux%u) -> dst(fmt=0x%x, %ux%u), engine=%s(%d),"
              " device=%p refcnt=%ld",
              idx++, srcFmt, srcW, srcH, dstFmt, dstW, dstH, engName, (int)eng,
              entry.device.get(), (long)entry.device.use_count());
    }
}

std::shared_ptr<ImageProcessDevice> ImageProcessFactory::createDeviceByEngine(ImxEngine engine) {
    for (auto& entry : mDeviceEntries) {
        if (entry.engine == engine) {
            return entry.creator();
        }
    }
    ALOGW("ImageProcessFactory: engine %d not registered", engine);
    return nullptr;
}

} // namespace fsl
