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

#ifndef _IMAGE_PROCESS_FACTORY_H
#define _IMAGE_PROCESS_FACTORY_H

#include <list>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

#include "ImageProcessDevice.h"

namespace fsl {

using namespace android;

// Factory for creating ImageProcessDevice instances.
// Probes all registered device types and selects the best one based on
// confidence scoring for the given input/output format and resolution.
//
// Usage:
//   auto& factory = ImageProcessFactory::getInstance();
//   auto device = factory.createDevice(srcFmt, dstFmt, srcW, srcH, dstW, dstH);
//   if (device) {
//       device->convert(dstBuf, srcBuf);
//   }
//
// Or to create a specific device by engine type:
//   auto device = factory.createDeviceByEngine(ENG_G2D);
class ImageProcessFactory {
public:
    // Get the singleton factory instance.
    // The factory itself is a singleton (lightweight, just holds the device registry).
    // But the devices it creates are NOT singletons - each caller gets their own instance.
    static ImageProcessFactory& getInstance();

    // Create the best device for the given conversion parameters.
    // Returns nullptr if no device can handle the conversion.
    std::shared_ptr<ImageProcessDevice> createDevice(uint32_t srcFormat, uint32_t dstFormat,
                                                     uint32_t srcWidth, uint32_t srcHeight,
                                                     uint32_t dstWidth, uint32_t dstHeight,
                                                     ImxEngine engine = ENG_NOTCARE);

    // Create a device for a specific engine type.
    // Returns nullptr if the engine is not available.
    std::shared_ptr<ImageProcessDevice> createDeviceByEngine(ImxEngine engine);

    // Dump all entries in mDeviceCache to the log.
    // Prints key fields (srcFmt, dstFmt, srcW, srcH, dstW, dstH, engine) and
    // the device pointer with its shared_ptr reference count.
    void dumpCache() const;

private:
    ImageProcessFactory();
    ~ImageProcessFactory() = default;
    ImageProcessFactory(const ImageProcessFactory&) = delete;
    ImageProcessFactory& operator=(const ImageProcessFactory&) = delete;

    // Device creator function type
    using DeviceCreator = std::shared_ptr<ImageProcessDevice> (*)();

    // Static sniff function type - called without instantiating a device
    using SniffFunc = float (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

    struct DeviceEntry {
        const char* name;
        ImxEngine engine;
        DeviceCreator creator;
        SniffFunc sniffer;
    };

    // Registered device types
    std::vector<DeviceEntry> mDeviceEntries;

    // Sniff all devices and find the best one.
    // Returns the index into mDeviceEntries, or -1 if none can handle.
    int selectBestDevice(uint32_t srcFormat, uint32_t dstFormat, uint32_t srcWidth,
                         uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight);

    // LRU device cache keyed by (srcFmt, dstFmt, srcW, srcH, dstW, dstH, engine).
    // Capacity is limited to kMaxCacheSize entries; the most recently used entry
    // is kept at the front of the list.
    static constexpr size_t kMaxCacheSize = 16;

    using CacheKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, ImxEngine>;

    struct CacheEntry {
        CacheKey key;
        std::shared_ptr<ImageProcessDevice> device;
    };

    std::list<CacheEntry> mDeviceCache;
    mutable std::mutex mCacheMutex;

    // Look up a cached device for the given key. Moves the hit entry to the
    // front (LRU promotion) and returns the device, or nullptr on a miss.
    std::shared_ptr<ImageProcessDevice> lookupCache(const CacheKey& key);

    // Insert a newly created device into the cache. Evicts the least recently
    // used entry when the cache is full.
    void insertCache(const CacheKey& key, std::shared_ptr<ImageProcessDevice> device);
};

} // namespace fsl

#endif // _IMAGE_PROCESS_FACTORY_H
