/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 * Copyright 2023 NXP
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * *    * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <array>
#include <chrono>
#include <iterator>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>

#include "utils/config_parser.h"
#include "utils/thermal_files.h"
#include "utils/thermal_watcher.h"

#include <aidl/android/hardware/thermal/BnThermal.h>

namespace aidl::android::hardware::thermal::impl::imx {

using NotificationCallback = std::function<void(const std::vector<Temperature> &temps)>;
using NotificationTime = std::chrono::time_point<std::chrono::steady_clock>;

using ::android::base::ReadFileToString;
using ::android::base::Split;
using ::android::base::StringPrintf;
using ::android::base::Trim;
using std::string;

constexpr char kConfigProperty[] = "vendor.thermal.config";
constexpr char kSocType[] = "ro.boot.soc_type";
constexpr char kInitRetryCountProperty[] = "vendor.thermal.init_retry_count";
constexpr char kInitRetryIntervalMsProperty[] = "vendor.thermal.init_retry_interval_ms";
constexpr int kDefaultInitRetryCount = 5;
constexpr int kDefaultInitRetryIntervalMs = 1000;

struct SensorStatus {
    ThrottlingSeverity severity;
    ThrottlingSeverity prev_hot_severity;
    ThrottlingSeverity prev_cold_severity;
};

struct CpuUsage {
    string name;
    uint64_t active;
    uint64_t total;
    bool isOnline;
};

class ThermalHelper {
  public:
    ThermalHelper(const NotificationCallback &cb);
    ~ThermalHelper() = default;
    bool fillCurrentTemperatures(bool filterType, bool filterCallback, TemperatureType type,
                                 std::vector<Temperature> *temperatures);
    bool fillTemperatureThresholds(bool filterType, TemperatureType type,
                                   std::vector<TemperatureThreshold> *thresholds) const;
    bool fillCurrentCoolingDevices(bool filterType, CoolingType type,
                                   std::vector<CoolingDevice> *coolingdevices) const;
    bool fillCpuUsages(std::vector<CpuUsage> *cpu_usages) const;
    // Dissallow copy and assign.
    ThermalHelper(const ThermalHelper &) = delete;
    void operator=(const ThermalHelper &) = delete;

    bool isInitializedOk() const { return is_initialized_; }

    // Read the temperature of a single sensor.
    bool readTemperature(std::string_view sensor_name, Temperature *out);
    bool readTemperature(
            std::string_view sensor_name, Temperature *out,
            std::pair<ThrottlingSeverity, ThrottlingSeverity> *throttling_status = nullptr) const;

    bool readTemperatureThreshold(std::string_view sensor_name, TemperatureThreshold *out) const;
    // Read the value of a single cooling device.
    bool readCoolingDevice(std::string_view cooling_device, CoolingDevice *out) const;
    // Get SensorInfo Map
    const std::map<std::string, SensorInfo> &GetSensorInfoMap() const { return sensor_info_map_; }
    void enableCPU(std::string cpu, bool enable);

  private:
    bool initializeSensorMap(const std::map<std::string, std::string> &path_map);
    bool initializeCoolingDevices(const std::map<std::string, std::string> &path_map);
    bool initializeTrip(const std::map<std::string, std::string> &path_map);

    // For thermal_watcher_'s polling thread
    bool thermalWatcherCallbackFunc(const std::set<std::string> &uevent_sensors);
    // Return hot and cold severity status as std::pair
    std::pair<ThrottlingSeverity, ThrottlingSeverity> getSeverityFromThresholds(
        const ThrottlingArray &hot_thresholds, const ThrottlingArray &cold_thresholds,
        const ThrottlingArray &hot_hysteresis, const ThrottlingArray &cold_hysteresis,
        ThrottlingSeverity prev_hot_severity, ThrottlingSeverity prev_cold_severity,
        float value) const;

    ::android::sp<ThermalWatcher> thermal_watcher_;
    ThermalFiles thermal_sensors_;
    ThermalFiles cooling_devices_;
    bool is_initialized_;
    const NotificationCallback cb_;
    const std::map<std::string, CoolingType> cooling_device_info_map_;
    const std::map<std::string, SensorInfo> sensor_info_map_;

    mutable std::shared_mutex sensor_status_map_mutex_;
    std::map<std::string, SensorStatus> sensor_status_map_;
};

}  // namespace aidl::android::hardware::thermal::impl::imx
