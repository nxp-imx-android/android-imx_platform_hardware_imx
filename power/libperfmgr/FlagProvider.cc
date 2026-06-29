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
#include "perfmgr/FlagProvider.h"

#include <android-base/file.h>

#include <format>
#include <memory>

namespace android::perfmgr {

FlagProvider &FlagProvider::GetInstance() {
    SetUp();
    return *static_cast<FlagProvider *>(powerhal::flags::provider_.get());
}

void FlagProvider::SetUp() {
    if (sOriginalProvider == nullptr) {
        sOriginalProvider = std::make_unique<FlagProvider>();
        sOriginalProvider.swap(powerhal::flags::provider_);
    }
}

void FlagProvider::TearDown() {
    if (sOriginalProvider != nullptr) {
        sOriginalProvider.swap(powerhal::flags::provider_);
        sOriginalProvider = nullptr;
    }
}

void FlagProvider::OverrideValue(FlagGetterPtr method, bool value) {
    this->*mOverriders[method] = value;
}

void FlagProvider::DropOverride(FlagGetterPtr method) {
    this->*mOverriders[method] = std::nullopt;
}

void FlagProvider::ClearOverrides() {
    for (auto &&overrider : mOverriders) {
        this->*overrider.second = std::nullopt;
    }
}

void FlagProvider::DumpToFd(int fd) {
    std::string header("========== Begin FlagProvider flags ==========\n");
    android::base::WriteStringToFd(header, fd);
    for (auto &&[flagName, getter] : mStringAssociations) {
        std::string line = std::format("{} : {}\n", flagName, getter() ? "true" : "false");
        android::base::WriteStringToFd(line, fd);
    }
    std::string footer("========== End FlagProvider flags ==========\n");
    android::base::WriteStringToFd(footer, fd);
}

FlagGetterPtr FlagProvider::GetterFromString(const std::string &flagName) {
    auto out = mStringAssociations.find(flagName);
    return out == mStringAssociations.end() ? nullptr : out->second;
}

std::unique_ptr<RawFlagProvider> FlagProvider::sOriginalProvider = nullptr;

}  // namespace android::perfmgr
