/*
 * Copyright 2025 The Android Open Source Project
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

#pragma once

#include <powerhal_flags.h>

#include <string>
#include <unordered_map>

namespace android::perfmgr {

/*
 * FlagProvider is a singleton that allows overriding, managing, and looking up flags by string.
 *
 * Ex:
 * // Forces powerhal::flags::test_flag() to eval to true:
 * FlagProvider::GetInstance().OverrideValue(powerhal::flags::test_flag, true);
 *
 * // Fetches the getter for powerhal::flags::test_flag from the string "test_flag"
 * auto getter = FlagProvider::GetInstance().GetterFromString("test_flag");
 * bool value = getter();
 */

#define ADD_FLAG(flagname)                                                          \
  public:                                                                           \
    bool flagname() {                                                               \
        return override_##flagname.value_or(sOriginalProvider->flagname());         \
    }                                                                               \
                                                                                    \
  private:                                                                          \
    std::optional<bool> override_##flagname;                                        \
    MethodTracker tracker_##flagname{this, "" #flagname, powerhal::flags::flagname, \
                                     &FlagProvider::override_##flagname};

using RawFlagProvider = powerhal::flags::flag_provider_interface;
using FlagGetterPtr = bool (*)();

class FlagProvider : public RawFlagProvider {
    static std::unique_ptr<RawFlagProvider> sOriginalProvider;
    std::unordered_map<std::string, FlagGetterPtr> mStringAssociations{};
    std::unordered_map<FlagGetterPtr, std::optional<bool> FlagProvider::*> mOverriders{};

    // Allows the macro to save string and method associations
    class MethodTracker {
      public:
        MethodTracker(FlagProvider *provider, const std::string flagName, FlagGetterPtr method,
                      std::optional<bool> FlagProvider::*overrider) {
            provider->mStringAssociations[flagName] = method;
            provider->mOverriders[method] = overrider;
        }
    };

  public:
    static FlagProvider &GetInstance();
    static void SetUp();
    static void TearDown();
    void OverrideValue(FlagGetterPtr method, bool value);
    void DropOverride(FlagGetterPtr method);
    void ClearOverrides();
    void DumpToFd(int fd);
    FlagGetterPtr GetterFromString(const std::string &flagName);

    ADD_FLAG(test_flag)
    ADD_FLAG(gpu_load_up_for_blurs)
    ADD_FLAG(ramp_down_sf_prefer_high_cap)
    ADD_FLAG(chrome_profile_bypass_hboost_severe_jank_level)
};

}  // namespace android::perfmgr
