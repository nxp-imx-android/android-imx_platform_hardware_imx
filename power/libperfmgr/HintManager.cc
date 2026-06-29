/*
 * Copyright (C) 2017 The Android Open Source Project
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
 * See the License for the specic language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG (ATRACE_TAG_POWER | ATRACE_TAG_HAL)
#define LOG_TAG "libperfmgr"

#include "perfmgr/HintManager.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <inttypes.h>
#include <json/reader.h>
#include <json/value.h>
#include <utils/Trace.h>

#include <algorithm>
#include <set>
#include <string>

#include "perfmgr/EventNode.h"
#include "perfmgr/FileNode.h"
#include "perfmgr/FlagProvider.h"
#include "perfmgr/PropertyNode.h"

namespace android {
namespace perfmgr {

namespace {
constexpr std::chrono::milliseconds kMilliSecondZero = std::chrono::milliseconds(0);
constexpr std::chrono::steady_clock::time_point kTimePointMax =
        std::chrono::steady_clock::time_point::max();
}  // namespace

using ::android::base::GetProperty;
using ::android::base::StringPrintf;

constexpr char kPowerHalTruncateProp[] = "vendor.powerhal.truncate";
constexpr std::string_view kConfigDebugPathProperty("vendor.powerhal.config.debug");
constexpr std::string_view kConfigProperty("vendor.powerhal.config");
constexpr std::string_view kConfigDefaultFileName("powerhint.json");
constexpr char kAdpfEventNodePath[] = "<AdpfConfig>:";

constexpr std::string_view kConfigDebugUsesFallback(
        "persist.vendor.powerhal.config.debug.usefallback");

bool HintManager::ValidateHint(const std::string& hint_type) const {
    if (nm_.get() == nullptr) {
        LOG(ERROR) << "NodeLooperThread not present";
        return false;
    }
    return IsHintSupported(hint_type);
}

bool HintManager::IsHintSupported(const std::string& hint_type) const {
    if (actions_.find(hint_type) == actions_.end()) {
        LOG(DEBUG) << "Hint type not present in actions: " << hint_type;
        return false;
    }
    return true;
}

bool HintManager::IsHintEnabled(const std::string &hint_type) const {
    std::lock_guard<std::mutex> lock(actions_.at(hint_type).hint_lock);
    return actions_.at(hint_type).mask_requesters.empty();
}

bool HintManager::InitHintStatus(const std::unique_ptr<HintManager> &hm) {
    if (hm.get() == nullptr) {
        return false;
    }
    for (auto &a : hm->actions_) {
        // timeout_ms equaling kMilliSecondZero means forever until cancelling.
        // As a result, if there's one NodeAction has timeout_ms of 0, we will store
        // 0 instead of max. Also node actions could be empty, set to 0 in that case.
        std::chrono::milliseconds timeout = kMilliSecondZero;
        if (a.second.node_actions.size()) {
            auto [min, max] =
                    std::minmax_element(a.second.node_actions.begin(), a.second.node_actions.end(),
                                        [](const auto act1, const auto act2) {
                                            return act1.timeout_ms < act2.timeout_ms;
                                        });
            timeout = min->timeout_ms == kMilliSecondZero ? kMilliSecondZero : max->timeout_ms;
        }
        a.second.status.reset(new HintStatus(timeout));
    }
    return true;
}

void HintManager::DoHintStatus(const std::string &hint_type, std::chrono::milliseconds timeout_ms) {
    std::lock_guard<std::mutex> lock(actions_.at(hint_type).hint_lock);
    actions_.at(hint_type).status->stats.count.fetch_add(1);
    auto now = std::chrono::steady_clock::now();
    ATRACE_INT(("H:" + hint_type).c_str(), (timeout_ms == kMilliSecondZero)
                                                   ? std::numeric_limits<int>::max()
                                                   : timeout_ms.count());
    ATRACE_NAME(("H:" + hint_type + ":" + std::to_string((timeout_ms == kMilliSecondZero)
                                                   ? std::numeric_limits<int>::max()
                                                   : timeout_ms.count())).c_str());
    if (now > actions_.at(hint_type).status->end_time) {
        actions_.at(hint_type).status->stats.duration_ms.fetch_add(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                        actions_.at(hint_type).status->end_time -
                        actions_.at(hint_type).status->start_time)
                        .count());
        actions_.at(hint_type).status->start_time = now;
    }
    actions_.at(hint_type).status->end_time =
            (timeout_ms == kMilliSecondZero) ? kTimePointMax : now + timeout_ms;
}

void HintManager::EndHintStatus(const std::string &hint_type) {
    std::lock_guard<std::mutex> lock(actions_.at(hint_type).hint_lock);
    // Update HintStats if the hint ends earlier than expected end_time
    auto now = std::chrono::steady_clock::now();
    ATRACE_INT(("H:" + hint_type).c_str(), 0);
    ATRACE_NAME(("H:" + hint_type + ":0").c_str());
    if (now < actions_.at(hint_type).status->end_time) {
        actions_.at(hint_type).status->stats.duration_ms.fetch_add(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - actions_.at(hint_type).status->start_time)
                        .count());
        actions_.at(hint_type).status->end_time = now;
    }
}

void HintManager::DoHintAction(const std::string &hint_type) {
    for (auto &action : actions_.at(hint_type).hint_actions) {
        if (!action.enable_property.empty() &&
            !android::base::GetBoolProperty(action.enable_property, true)) {
            // Disabled action based on its control property
            continue;
        }
        if ((action.enable_flag != nullptr && !action.enable_flag()) ||
            (action.disable_flag != nullptr && action.disable_flag())) {
            continue;
        }
        switch (action.type) {
            case HintActionType::DoHint:
                DoHint(action.value);
                break;
            case HintActionType::EndHint:
                EndHint(action.value);
                break;
            case HintActionType::MaskHint:
                if (actions_.find(action.value) == actions_.end()) {
                    LOG(ERROR) << "Failed to find " << action.value << " action";
                } else {
                    std::lock_guard<std::mutex> lock(actions_.at(hint_type).hint_lock);
                    actions_.at(action.value).mask_requesters.insert(hint_type);
                }
                break;
            default:
                // should not reach here
                LOG(ERROR) << "Invalid "
                           << static_cast<std::underlying_type<HintActionType>::type>(action.type)
                           << " type";
        }
    }
}

void HintManager::EndHintAction(const std::string &hint_type) {
    for (auto &action : actions_.at(hint_type).hint_actions) {
        if (action.type == HintActionType::MaskHint &&
            actions_.find(action.value) != actions_.end()) {
            std::lock_guard<std::mutex> lock(actions_.at(hint_type).hint_lock);
            actions_.at(action.value).mask_requesters.erase(hint_type);
        }
    }
}

bool HintManager::DoHint(const std::string& hint_type) {
    LOG(VERBOSE) << "Do Powerhint: " << hint_type;
    if (!ValidateHint(hint_type) || !IsHintEnabled(hint_type) ||
        !nm_->Request(actions_.at(hint_type).node_actions, hint_type)) {
        return false;
    }
    DoHintStatus(hint_type, actions_.at(hint_type).status->max_timeout);
    DoHintAction(hint_type);
    return true;
}

bool HintManager::DoHint(const std::string& hint_type,
                         std::chrono::milliseconds timeout_ms_override) {
    LOG(VERBOSE) << "Do Powerhint: " << hint_type << " for "
                 << timeout_ms_override.count() << "ms";
    if (!ValidateHint(hint_type) || !IsHintEnabled(hint_type)) {
        return false;
    }
    std::vector<NodeAction> actions_override = actions_.at(hint_type).node_actions;
    for (auto& action : actions_override) {
        action.timeout_ms = timeout_ms_override;
    }
    if (!nm_->Request(actions_override, hint_type)) {
        return false;
    }
    DoHintStatus(hint_type, timeout_ms_override);
    DoHintAction(hint_type);
    return true;
}

bool HintManager::EndHint(const std::string& hint_type) {
    LOG(VERBOSE) << "End Powerhint: " << hint_type;
    if (!ValidateHint(hint_type) || !nm_->Cancel(actions_.at(hint_type).node_actions, hint_type)) {
        return false;
    }
    EndHintStatus(hint_type);
    EndHintAction(hint_type);
    return true;
}

bool HintManager::IsRunning() const {
    return (nm_.get() == nullptr) ? false : nm_->isRunning();
}

std::vector<std::string> HintManager::GetHints() const {
    std::vector<std::string> hints;
    for (auto const& action : actions_) {
        hints.push_back(action.first);
    }
    return hints;
}

HintStats HintManager::GetHintStats(const std::string &hint_type) const {
    HintStats hint_stats;
    if (ValidateHint(hint_type)) {
        std::lock_guard<std::mutex> lock(actions_.at(hint_type).hint_lock);
        hint_stats.count =
                actions_.at(hint_type).status->stats.count.load(std::memory_order_relaxed);
        hint_stats.duration_ms =
                actions_.at(hint_type).status->stats.duration_ms.load(std::memory_order_relaxed);
    }
    return hint_stats;
}

void HintManager::DumpToFd(int fd) {
    std::string header("========== Begin perfmgr nodes ==========\n");
    if (!android::base::WriteStringToFd(header, fd)) {
        LOG(ERROR) << "Failed to dump fd: " << fd;
    }
    nm_->DumpToFd(fd);
    std::string footer("==========  End perfmgr nodes  ==========\n");
    if (!android::base::WriteStringToFd(footer, fd)) {
        LOG(ERROR) << "Failed to dump fd: " << fd;
    }
    header = "========== Begin perfmgr stats ==========\n"
             "Hint Name\t"
             "Counts\t"
             "Duration\n";
    if (!android::base::WriteStringToFd(header, fd)) {
        LOG(ERROR) << "Failed to dump fd: " << fd;
    }
    std::string hint_stats_string;
    std::vector<std::string> keys(GetHints());
    std::sort(keys.begin(), keys.end());
    for (const auto &ordered_key : keys) {
        HintStats hint_stats(GetHintStats(ordered_key));
        hint_stats_string += StringPrintf("%s\t%" PRIu32 "\t%" PRIu64 "\n", ordered_key.c_str(),
                                          hint_stats.count, hint_stats.duration_ms);
    }
    if (!android::base::WriteStringToFd(hint_stats_string, fd)) {
        LOG(ERROR) << "Failed to dump fd: " << fd;
    }
    footer = "==========  End perfmgr stats  ==========\n";
    if (!android::base::WriteStringToFd(footer, fd)) {
        LOG(ERROR) << "Failed to dump fd: " << fd;
    }

    // Dump current ADPF profiles
    if (IsAdpfSupported()) {
        header = "========== ADPF Tag Profile begin ==========\n";
        if (!android::base::WriteStringToFd(header, fd)) {
            LOG(ERROR) << "Failed to dump fd: " << fd;
        }

        header = "---- Default non-tagged adpf profile ----\n";
        if (!android::base::WriteStringToFd(header, fd)) {
            LOG(ERROR) << "Failed to dump fd: " << fd;
        }
        GetAdpfProfileFromDoHint()->dumpToFd(fd);

        for (const auto &tag_profile : tag_profile_map_) {
            header = StringPrintf("---- Tagged ADPF Profile: %s ----\n", tag_profile.first.c_str());
            if (!android::base::WriteStringToFd(header, fd)) {
                LOG(ERROR) << "Failed to dump fd: " << fd;
            }
            tag_profile.second->dumpToFd(fd);
        }

        footer = "========== ADPF Tag Profile end ==========\n";
        if (!android::base::WriteStringToFd(footer, fd)) {
            LOG(ERROR) << "Failed to dump fd: " << fd;
        }
    } else {
        header = "========== IsAdpfSupported: No ===========\n";
        if (!android::base::WriteStringToFd(header, fd)) {
            LOG(ERROR) << "Failed to dump fd: " << fd;
        }
    }

    DumpOtherConfigs(fd);

    FlagProvider::GetInstance().DumpToFd(fd);

    fsync(fd);
}

void HintManager::DumpOtherConfigs(int fd) {
    std::ostringstream dumpBuf;
    dumpBuf << "========== Other configurations begin ==========\n";
    if (other_configs_.GPUSysfsPath) {
        dumpBuf << "GPUSysfsPath: " << other_configs_.GPUSysfsPath.value() << "\n";
    }
    if (other_configs_.enableMetricCollection) {
        dumpBuf << "EnableMetricCollection: " << other_configs_.enableMetricCollection.value()
                << "\n";
    }
    if (other_configs_.maxNumOfCachedSessionMetrics) {
        dumpBuf << "MaxNumOfCachedSessionMetrics: "
                << other_configs_.maxNumOfCachedSessionMetrics.value() << "\n";
    }
    dumpBuf << "EnableSFPreferHighCap: " << other_configs_.enableSFPreferHighCap
            << "\n";
    dumpBuf << "========== Other configurations end ==========\n";

    if (!android::base::WriteStringToFd(dumpBuf.str(), fd)) {
        LOG(ERROR) << "Failed to dump fd: " << fd;
    }
}

bool HintManager::Start() {
    return nm_->Start();
}

std::unique_ptr<HintManager> HintManager::sInstance = nullptr;

void HintManager::Reload(bool start) {
    std::string config_path = "/vendor/etc/";
    std::string debug_config_path = "/data/vendor/etc/";
    bool usesDebug = false;

    usesDebug = android::base::GetBoolProperty(kConfigDebugPathProperty.data(), false);
    if (usesDebug) {
        bool usesFallback = android::base::GetBoolProperty(kConfigDebugUsesFallback.data(), false);
        debug_config_path.append(
                GetProperty(kConfigProperty.data(), kConfigDefaultFileName.data()));
        LOG(WARNING) << "Pixel Power HAL AIDL Service is starting with debug config: "
                     << debug_config_path;
        HintManager::GetFromJSON(debug_config_path, start);
        if (!sInstance) {
            if (usesFallback) {
                LOG(ERROR) << "Invalid debug config: " << debug_config_path
                           << " falling back to default.";
            } else {
                LOG(FATAL) << "Invalid debug config: " << debug_config_path;
            }
        } else {
            LOG(WARNING) << "Pixel Power HAL AIDL Service successfully loaded debug config: "
                         << debug_config_path;
            return;
        }
    }

    config_path.append(GetProperty(kConfigProperty.data(), kConfigDefaultFileName.data()));

    LOG(INFO) << "Pixel Power HAL AIDL Service with Extension is starting with config: "
              << config_path;
    // Reload and start the HintManager
    HintManager::GetFromJSON(config_path, start);
    if (!sInstance) {
        LOG(FATAL) << "Invalid config: " << config_path;
    }
}

HintManager *HintManager::GetInstance() {
    FlagProvider::SetUp();
    if (sInstance == nullptr) {
        HintManager::Reload(false);
    }
    return sInstance.get();
}

OtherConfigs HintManager::ParseOtherConfigs(const std::string &json_doc) {
    OtherConfigs otherConf;
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errorMessage;
    if (!reader->parse(&*json_doc.begin(), &*json_doc.end(), &root, &errorMessage)) {
        LOG(ERROR) << "Failed to parse JSON config: " << errorMessage;
        return otherConf;
    }

    // TODO(guibing@): Remove this part after all the powerhint configurations moved its position
    // under "OtherConfigs". Keep it now for compatibility with existing powerhint json files.
    if (!root["GpuSysfsPath"].empty() && root["GpuSysfsPath"].isString()) {
        otherConf.GPUSysfsPath = root["GpuSysfsPath"].asString();
    }

    if (root["OtherConfigs"].empty()) {
        return otherConf;
    }

    // Parse other configurations
    Json::Value extraOtherConf = root["OtherConfigs"];
    if (!extraOtherConf["EnableMetricCollection"].empty() &&
        extraOtherConf["EnableMetricCollection"].isBool()) {
        otherConf.enableMetricCollection = extraOtherConf["EnableMetricCollection"].asBool();
    }
    if (!extraOtherConf["MaxNumOfCachedSessionMetrics"].empty() &&
        extraOtherConf["MaxNumOfCachedSessionMetrics"].isUInt()) {
        otherConf.maxNumOfCachedSessionMetrics =
                extraOtherConf["MaxNumOfCachedSessionMetrics"].asUInt();
    }
    if (!extraOtherConf["GpuSysfsPath"].empty() && extraOtherConf["GpuSysfsPath"].isString()) {
        otherConf.GPUSysfsPath = extraOtherConf["GpuSysfsPath"].asString();
    }
    if (!extraOtherConf["EnableSFPreferHighCap"].empty() &&
        extraOtherConf["EnableSFPreferHighCap"].isBool()) {
        otherConf.enableSFPreferHighCap = extraOtherConf["EnableSFPreferHighCap"].asBool();
    }
    return otherConf;
}

HintManager *HintManager::GetFromJSON(const std::string &config_path, bool start) {
    std::string json_doc;

    if (!android::base::ReadFileToString(config_path, &json_doc)) {
        LOG(ERROR) << "Failed to read JSON config from " << config_path;
        return nullptr;
    }

    std::vector<std::unique_ptr<Node>> nodes = ParseNodes(json_doc);
    if (nodes.empty()) {
        LOG(ERROR) << "Failed to parse Nodes section from " << config_path;
        return nullptr;
    }
    std::vector<std::shared_ptr<AdpfConfig>> adpfs = HintManager::ParseAdpfConfigs(json_doc);
    if (adpfs.empty()) {
        LOG(INFO) << "No AdpfConfig section in the " << config_path;
    }

    std::unordered_map<std::string, Hint> actions = HintManager::ParseActions(json_doc, nodes);

    // Parse ADPF Event Node
    std::unordered_map<std::string, std::shared_ptr<AdpfConfig>> tag_adpfs;
    LOG(VERBOSE) << "Parse ADPF Hint Event Table from all nodes.";
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const std::string &node_name = nodes[i]->GetName();
        const std::vector<std::string> &node_paths = nodes[i]->GetPaths();

        for (auto &path : node_paths) {
            if (path.starts_with(kAdpfEventNodePath)) {
                std::string tag = path.substr(strlen(kAdpfEventNodePath));
                std::size_t index = nodes[i]->GetDefaultIndex();
                std::string profile_name = nodes[i]->GetValues()[index];
                for (std::size_t j = 0; j < adpfs.size(); ++j) {
                    if (adpfs[j]->mName == profile_name) {
                        tag_adpfs[tag] = adpfs[j];
                        LOG(INFO) << "[" << tag << ":" << node_name << "] set to '" << profile_name
                                << "'";
                        break;
                    }
                }
                if (!tag_adpfs[tag]) {
                    tag_adpfs[tag] = adpfs[0];
                    LOG(INFO) << "[" << tag << ":" << node_name << "] fallback to '"
                              << adpfs[0]->mName << "'";
                }
            }
        }
    }

    if (actions.empty()) {
        LOG(ERROR) << "Failed to parse Actions section from " << config_path;
        return nullptr;
    }

    auto const other_configs = ParseOtherConfigs(json_doc);

    sp<NodeLooperThread> nm = new NodeLooperThread(std::move(nodes));
    sInstance =
            std::make_unique<HintManager>(std::move(nm), actions, adpfs, tag_adpfs, other_configs);

    if (!HintManager::InitHintStatus(sInstance)) {
        LOG(ERROR) << "Failed to initialize hint status";
        return nullptr;
    }

    LOG(INFO) << "Initialized HintManager from JSON config: " << config_path;

    if (start) {
        sInstance->Start();
    }

    return HintManager::GetInstance();
}

std::vector<std::unique_ptr<Node>> HintManager::ParseNodes(const std::string &json_doc) {
    // function starts
    std::vector<std::unique_ptr<Node>> nodes_parsed;
    std::set<std::string> nodes_name_parsed;
    std::set<std::string> nodes_path_parsed;
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errorMessage;

    if (!reader->parse(&*json_doc.begin(), &*json_doc.end(), &root, &errorMessage)) {
        LOG(ERROR) << "Failed to parse JSON config: " << errorMessage;
        return nodes_parsed;
    }

    Json::Value nodes = root["Nodes"];
    for (Json::Value::ArrayIndex i = 0; i < nodes.size(); ++i) {
        std::string name = nodes[i]["Name"].asString();
        LOG(VERBOSE) << "Node[" << i << "]'s Name: " << name;
        if (name.empty()) {
            LOG(ERROR) << "Failed to read "
                       << "Node[" << i << "]'s Name";
            nodes_parsed.clear();
            return nodes_parsed;
        }

        auto result = nodes_name_parsed.insert(name);
        if (!result.second) {
            LOG(ERROR) << "Duplicate Node[" << i << "]'s Name";
            nodes_parsed.clear();
            return nodes_parsed;
        }

        std::vector<std::string> paths_parsed;
        std::string path = nodes[i]["Path"].asString();

        if (!path.empty()) {
            LOG(WARNING) << "In node" << name << " old node path format detected.";
            auto result = nodes_path_parsed.insert(path);
            if (!result.second) {
                LOG(ERROR) << "Duplicate Node[" << i << "]'s Paths";
                nodes_parsed.clear();
                return nodes_parsed;
            }
            paths_parsed.push_back(path);
        }

        Json::Value paths = nodes[i]["Paths"];

        if (paths.empty()) {
            LOG(ERROR) << "Failed to read "
                       << "Node[" << i << "]'s Paths";
            if (paths_parsed.empty()) {
                nodes_parsed.clear();
                return nodes_parsed;
            }
        }

        for (Json::Value::ArrayIndex j = 0; j < paths.size(); ++j) {
            path = paths[j].asString();
            if (path.empty()) {
                LOG(ERROR) << "Failed to read "
                           << "Node[" << i << "]'s Paths";
                nodes_parsed.clear();
                return nodes_parsed;
            }
            auto result = nodes_path_parsed.insert(path);
            if (!result.second) {
                LOG(ERROR) << "Duplicate Node[" << i << "]'s Paths";
                nodes_parsed.clear();
                return nodes_parsed;
            }
            paths_parsed.push_back(path);
        }

        bool is_event_node = false;
        bool is_file = false;
        std::string node_type = nodes[i]["Type"].asString();
        LOG(VERBOSE) << "Node[" << i << "]'s Type: " << node_type;
        if (node_type.empty()) {
            is_file = true;
            LOG(VERBOSE) << "Failed to read "
                         << "Node[" << i << "]'s Type, set to 'File' as default";
        } else if (node_type == "Event") {
            is_event_node = true;
        } else if (node_type == "File") {
            is_file = true;
        } else if (node_type == "Property") {
            is_file = false;
        } else {
            LOG(ERROR) << "Invalid Node[" << i
                       << "]'s Type: only File and Property supported.";
            nodes_parsed.clear();
            return nodes_parsed;
        }

        std::vector<RequestGroup> values_parsed;
        std::set<std::string> values_set_parsed;
        Json::Value values = nodes[i]["Values"];
        for (Json::Value::ArrayIndex j = 0; j < values.size(); ++j) {
            std::string value = values[j].asString();
            LOG(VERBOSE) << "Node[" << i << "]'s Value[" << j << "]: " << value;
            auto result = values_set_parsed.insert(value);
            if (!result.second) {
                LOG(ERROR) << "Duplicate value parsed in Node[" << i
                           << "]'s Value[" << j << "]";
                nodes_parsed.clear();
                return nodes_parsed;
            }
            if (is_file && value.empty()) {
                LOG(ERROR) << "Failed to read Node[" << i << "]'s Value[" << j
                           << "]";
                nodes_parsed.clear();
                return nodes_parsed;
            }
            values_parsed.emplace_back(value);
        }
        if (values_parsed.size() < 1) {
            LOG(ERROR) << "Failed to read Node[" << i << "]'s Values";
            nodes_parsed.clear();
            return nodes_parsed;
        }

        Json::UInt64 default_index = values_parsed.size() - 1;
        if (nodes[i]["DefaultIndex"].empty() ||
            !nodes[i]["DefaultIndex"].isUInt64()) {
            LOG(INFO) << "Failed to read Node[" << i
                      << "]'s DefaultIndex, set to last index: "
                      << default_index;
        } else {
            default_index = nodes[i]["DefaultIndex"].asUInt64();
        }
        if (default_index > values_parsed.size() - 1) {
            default_index = values_parsed.size() - 1;
            LOG(ERROR) << "Node[" << i
                       << "]'s DefaultIndex out of bound, max value index: "
                       << default_index;
            nodes_parsed.clear();
            return nodes_parsed;
        }
        LOG(VERBOSE) << "Node[" << i << "]'s DefaultIndex: " << default_index;

        bool reset = false;
        if (nodes[i]["ResetOnInit"].empty() ||
            !nodes[i]["ResetOnInit"].isBool()) {
            LOG(INFO) << "Failed to read Node[" << i
                      << "]'s ResetOnInit, set to 'false'";
        } else {
            reset = nodes[i]["ResetOnInit"].asBool();
        }
        LOG(VERBOSE) << "Node[" << i << "]'s ResetOnInit: " << std::boolalpha
                     << reset << std::noboolalpha;

        if (is_event_node) {
            auto update_callback = [](const std::string &name, const std::vector<std::string> &path,
                                      const std::string &val) {
                HintManager::GetInstance()->OnNodeUpdate(name, path, val);
            };
            nodes_parsed.emplace_back(std::make_unique<EventNode>(
                    name, paths_parsed, values_parsed, static_cast<std::size_t>(default_index),
                    reset, update_callback));
        } else if (is_file) {
            bool truncate = android::base::GetBoolProperty(kPowerHalTruncateProp, true);
            if (nodes[i]["Truncate"].empty() || !nodes[i]["Truncate"].isBool()) {
                LOG(INFO) << "Failed to read Node[" << i << "]'s Truncate, set to 'true'";
            } else {
                truncate = nodes[i]["Truncate"].asBool();
            }
            LOG(VERBOSE) << "Node[" << i << "]'s Truncate: " << std::boolalpha << truncate
                         << std::noboolalpha;

            bool hold_fd = false;
            if (nodes[i]["HoldFd"].empty() || !nodes[i]["HoldFd"].isBool()) {
                LOG(INFO) << "Failed to read Node[" << i
                          << "]'s HoldFd, set to 'false'";
            } else {
                hold_fd = nodes[i]["HoldFd"].asBool();
            }
            LOG(VERBOSE) << "Node[" << i << "]'s HoldFd: " << std::boolalpha
                         << hold_fd << std::noboolalpha;

            bool write_only = false;
            if (nodes[i]["WriteOnly"].empty() || !nodes[i]["WriteOnly"].isBool()) {
                LOG(INFO) << "Failed to read Node[" << i
                          << "]'s WriteOnly, set to 'false'";
            } else {
                write_only = nodes[i]["WriteOnly"].asBool();
            }
            LOG(VERBOSE) << "Node[" << i << "]'s WriteOnly: " << std::boolalpha
                         << write_only << std::noboolalpha;

            bool allow_failure = false;
            if (nodes[i]["AllowFailure"].empty() || !nodes[i]["AllowFailure"].isBool()) {
                LOG(INFO) << "Failed to read Node[" << i
                        << "]'s AllowFailure, set to 'false'";
            } else {
                allow_failure = nodes[i]["AllowFailure"].asBool();
            }
            LOG(VERBOSE) << "Node[" << i << "]'s AllowFailure: " << std::boolalpha
                         << allow_failure << std::noboolalpha;

            nodes_parsed.emplace_back(std::make_unique<FileNode>(
                    name, paths_parsed, values_parsed, static_cast<std::size_t>(default_index),
                    reset, truncate, allow_failure, hold_fd, write_only));
        } else {
            nodes_parsed.emplace_back(
                    std::make_unique<PropertyNode>(name, paths_parsed, values_parsed,
                                                   static_cast<std::size_t>(default_index), reset));
        }
    }
    LOG(INFO) << nodes_parsed.size() << " Nodes parsed successfully";
    return nodes_parsed;
}

std::unordered_map<std::string, Hint> HintManager::ParseActions(
        const std::string &json_doc, const std::vector<std::unique_ptr<Node>> &nodes) {
    // function starts
    std::unordered_map<std::string, Hint> actions_parsed;
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errorMessage;

    if (!reader->parse(&*json_doc.begin(), &*json_doc.end(), &root, &errorMessage)) {
        LOG(ERROR) << "Failed to parse JSON config";
        return actions_parsed;
    }

    Json::Value actions = root["Actions"];
    std::size_t total_parsed = 0;

    std::map<std::string, std::size_t> nodes_index;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        nodes_index[nodes[i]->GetName()] = i;
    }

    for (Json::Value::ArrayIndex i = 0; i < actions.size(); ++i) {
        const std::string& hint_type = actions[i]["PowerHint"].asString();
        LOG(VERBOSE) << "Action[" << i << "]'s PowerHint: " << hint_type;
        if (hint_type.empty()) {
            LOG(ERROR) << "Failed to read "
                       << "Action[" << i << "]'s PowerHint";
            actions_parsed.clear();
            return actions_parsed;
        }

        HintActionType action_type = HintActionType::Node;
        std::string type_string = actions[i]["Type"].asString();
        std::string enable_property = actions[i]["EnableProperty"].asString();
        std::string enable_flag = actions[i]["EnableFlag"].asString();
        std::string disable_flag = actions[i]["DisableFlag"].asString();
        LOG(VERBOSE) << "Action[" << i << "]'s Type: " << type_string;
        if (type_string.empty()) {
            LOG(VERBOSE) << "Failed to read "
                         << "Action[" << i << "]'s Type, set to 'Node' as default";
        } else if (type_string == "DoHint") {
            action_type = HintActionType::DoHint;
        } else if (type_string == "EndHint") {
            action_type = HintActionType::EndHint;
        } else if (type_string == "MaskHint") {
            action_type = HintActionType::MaskHint;
        } else {
            LOG(ERROR) << "Invalid Action[" << i << "]'s Type: " << type_string;
            actions_parsed.clear();
            return actions_parsed;
        }
        if (action_type == HintActionType::Node) {
            std::string node_name = actions[i]["Node"].asString();
            LOG(VERBOSE) << "Action[" << i << "]'s Node: " << node_name;
            std::size_t node_index;

            if (nodes_index.find(node_name) == nodes_index.end()) {
                LOG(ERROR) << "Failed to find "
                           << "Action[" << i << "]'s Node from Nodes section: [" << node_name
                           << "]";
                actions_parsed.clear();
                return actions_parsed;
            }
            node_index = nodes_index[node_name];

            std::string value_name = actions[i]["Value"].asString();
            LOG(VERBOSE) << "Action[" << i << "]'s Value: " << value_name;
            std::size_t value_index = 0;

            if (!nodes[node_index]->GetValueIndex(value_name, &value_index)) {
                LOG(ERROR) << "Failed to read Action[" << i << "]'s Value";
                LOG(ERROR) << "Action[" << i << "]'s Value " << value_name
                           << " is not defined in Node[" << node_name << "]";
                actions_parsed.clear();
                return actions_parsed;
            }
            LOG(VERBOSE) << "Action[" << i << "]'s ValueIndex: " << value_index;

            Json::UInt64 duration = 0;
            if (actions[i]["Duration"].empty() || !actions[i]["Duration"].isUInt64()) {
                LOG(ERROR) << "Failed to read Action[" << i << "]'s Duration";
                actions_parsed.clear();
                return actions_parsed;
            } else {
                duration = actions[i]["Duration"].asUInt64();
            }
            LOG(VERBOSE) << "Action[" << i << "]'s Duration: " << duration;

            for (const auto &action : actions_parsed[hint_type].node_actions) {
                if (action.node_index == node_index) {
                    LOG(ERROR)
                        << "Action[" << i
                        << "]'s NodeIndex is duplicated with another Action";
                    actions_parsed.clear();
                    return actions_parsed;
                }
            }
            actions_parsed[hint_type].node_actions.emplace_back(
                    node_index, value_index, std::chrono::milliseconds(duration), enable_property,
                    enable_flag, disable_flag);

        } else {
            const std::string &hint_value = actions[i]["Value"].asString();
            LOG(VERBOSE) << "Action[" << i << "]'s Value: " << hint_value;
            if (hint_value.empty()) {
                LOG(ERROR) << "Failed to read "
                           << "Action[" << i << "]'s Value";
                actions_parsed.clear();
                return actions_parsed;
            }
            actions_parsed[hint_type].hint_actions.emplace_back(
                    action_type, hint_value, enable_property, enable_flag, disable_flag);
        }

        ++total_parsed;
    }

    LOG(INFO) << total_parsed << " actions parsed successfully";

    for (const auto& action : actions_parsed) {
        LOG(INFO) << "PowerHint " << action.first << " has " << action.second.node_actions.size()
                  << " node actions"
                  << ", and " << action.second.hint_actions.size() << " hint actions parsed";
    }

    return actions_parsed;
}

#define ADPF_PARSE(VARIABLE, ENTRY, TYPE)                                                        \
    static_assert(std::is_same<decltype(adpfs[i][ENTRY].as##TYPE()), decltype(VARIABLE)>::value, \
                  "Parser type mismatch");                                                       \
    if (adpfs[i][ENTRY].empty() || !adpfs[i][ENTRY].is##TYPE()) {                                \
        LOG(ERROR) << "Failed to read AdpfConfig[" << name << "][" ENTRY "]'s Values";           \
        adpfs_parsed.clear();                                                                    \
        return adpfs_parsed;                                                                     \
    }                                                                                            \
    VARIABLE = adpfs[i][ENTRY].as##TYPE()

#define ADPF_PARSE_OPTIONAL(VARIABLE, ENTRY, TYPE)                     \
    static_assert(std::is_same<decltype(adpfs[i][ENTRY].as##TYPE()),   \
                               decltype(VARIABLE)::value_type>::value, \
                  "Parser type mismatch");                             \
    if (!adpfs[i][ENTRY].empty() && adpfs[i][ENTRY].is##TYPE()) {      \
        VARIABLE = adpfs[i][ENTRY].as##TYPE();                         \
    }

std::vector<std::shared_ptr<AdpfConfig>> HintManager::ParseAdpfConfigs(
        const std::string &json_doc) {
    // function starts
    bool pidOn;
    double pidPOver;
    double pidPUnder;
    double pidI;
    double pidDOver;
    double pidDUnder;
    int64_t pidIInit;
    int64_t pidIHighLimit;
    int64_t pidILowLimit;
    bool adpfUclamp;
    uint32_t uclampMinInit;
    uint32_t uclampMinHighLimit;
    uint32_t uclampMinLowLimit;
    uint64_t samplingWindowP;
    uint64_t samplingWindowI;
    uint64_t samplingWindowD;
    double staleTimeFactor;
    uint64_t reportingRate;
    double targetTimeFactor;

    std::vector<std::shared_ptr<AdpfConfig>> adpfs_parsed;
    std::set<std::string> name_parsed;
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errorMessage;
    if (!reader->parse(&*json_doc.begin(), &*json_doc.end(), &root, &errorMessage)) {
        LOG(ERROR) << "Failed to parse JSON config: " << errorMessage;
        return adpfs_parsed;
    }
    Json::Value adpfs = root["AdpfConfig"];
    for (Json::Value::ArrayIndex i = 0; i < adpfs.size(); ++i) {
        std::optional<bool> gpuBoost;
        std::optional<uint64_t> gpuBoostCapacityMax;
        uint64_t gpuCapacityLoadUpHeadroom = 0;
        std::string name = adpfs[i]["Name"].asString();
        LOG(VERBOSE) << "AdpfConfig[" << i << "]'s Name: " << name;
        if (name.empty()) {
            LOG(ERROR) << "Failed to read "
                       << "AdpfConfig[" << i << "]'s Name";
            adpfs_parsed.clear();
            return adpfs_parsed;
        }
        auto result = name_parsed.insert(name);
        if (!result.second) {
            LOG(ERROR) << "Duplicate AdpfConfig[" << i << "]'s Name";
            adpfs_parsed.clear();
            return adpfs_parsed;
        }

        // heuristic boost configs
        std::optional<bool> heuristicBoostOn;
        std::optional<uint32_t> hBoostModerateJankThreshold;
        std::optional<double> hBoostOffMaxAvgDurRatio;
        std::optional<double> hBoostSevereJankPidPu;
        std::optional<uint32_t> hBoostSevereJankThreshold;
        std::optional<std::pair<uint32_t, uint32_t>> hBoostUclampMinCeilingRange;
        std::optional<std::pair<uint32_t, uint32_t>> hBoostUclampMinFloorRange;
        std::optional<double> jankCheckTimeFactor;
        std::optional<uint32_t> lowFrameRateThreshold;
        std::optional<uint32_t> maxRecordsNum;
        std::optional<bool> heuristicRampup;
        std::optional<uint32_t> defaultRampupMult;
        std::optional<uint32_t> highRampupMult;

        std::optional<uint32_t> uclampMinLoadUp;
        std::optional<uint32_t> uclampMinLoadReset;
        std::optional<int32_t> uclampMaxEfficientBase;
        std::optional<int32_t> uclampMaxEfficientOffset;

        ADPF_PARSE(pidOn, "PID_On", Bool);
        ADPF_PARSE(pidPOver, "PID_Po", Double);
        ADPF_PARSE(pidPUnder, "PID_Pu", Double);
        ADPF_PARSE(pidI, "PID_I", Double);
        ADPF_PARSE(pidIInit, "PID_I_Init", Int64);
        ADPF_PARSE(pidIHighLimit, "PID_I_High", Int64);
        ADPF_PARSE(pidILowLimit, "PID_I_Low", Int64);
        ADPF_PARSE(pidDOver, "PID_Do", Double);
        ADPF_PARSE(pidDUnder, "PID_Du", Double);
        ADPF_PARSE(adpfUclamp, "UclampMin_On", Bool);
        ADPF_PARSE(uclampMinInit, "UclampMin_Init", UInt);
        ADPF_PARSE_OPTIONAL(uclampMinLoadUp, "UclampMin_LoadUp", UInt);
        ADPF_PARSE_OPTIONAL(uclampMinLoadReset, "UclampMin_LoadReset", UInt);
        ADPF_PARSE(uclampMinHighLimit, "UclampMin_High", UInt);
        ADPF_PARSE(uclampMinLowLimit, "UclampMin_Low", UInt);
        ADPF_PARSE(samplingWindowP, "SamplingWindow_P", UInt64);
        ADPF_PARSE(samplingWindowI, "SamplingWindow_I", UInt64);
        ADPF_PARSE(samplingWindowD, "SamplingWindow_D", UInt64);
        ADPF_PARSE(staleTimeFactor, "StaleTimeFactor", Double);
        ADPF_PARSE(reportingRate, "ReportingRateLimitNs", UInt64);
        ADPF_PARSE(targetTimeFactor, "TargetTimeFactor", Double);
        ADPF_PARSE_OPTIONAL(heuristicBoostOn, "HeuristicBoost_On", Bool);
        ADPF_PARSE_OPTIONAL(hBoostModerateJankThreshold, "HBoostModerateJankThreshold", UInt);
        ADPF_PARSE_OPTIONAL(hBoostOffMaxAvgDurRatio, "HBoostOffMaxAvgDurRatio", Double);
        ADPF_PARSE_OPTIONAL(hBoostSevereJankPidPu, "HBoostSevereJankPidPu", Double);
        ADPF_PARSE_OPTIONAL(hBoostSevereJankThreshold, "HBoostSevereJankThreshold", UInt);
        ADPF_PARSE_OPTIONAL(jankCheckTimeFactor, "JankCheckTimeFactor", Double);
        ADPF_PARSE_OPTIONAL(lowFrameRateThreshold, "LowFrameRateThreshold", UInt);
        ADPF_PARSE_OPTIONAL(maxRecordsNum, "MaxRecordsNum", UInt);
        ADPF_PARSE_OPTIONAL(heuristicRampup, "HeuristicRampup", Bool);
        ADPF_PARSE_OPTIONAL(defaultRampupMult, "DefaultRampupMult", UInt);
        ADPF_PARSE_OPTIONAL(highRampupMult, "HighRampupMult", UInt);
        ADPF_PARSE_OPTIONAL(uclampMaxEfficientBase, "UclampMax_EfficientBase", Int);
        ADPF_PARSE_OPTIONAL(uclampMaxEfficientOffset, "UclampMax_EfficientOffset", Int);

        if (!adpfs[i]["GpuBoost"].empty() && adpfs[i]["GpuBoost"].isBool()) {
            gpuBoost = adpfs[i]["GpuBoost"].asBool();
        }
        if (!adpfs[i]["GpuCapacityBoostMax"].empty() &&
            adpfs[i]["GpuCapacityBoostMax"].isUInt64()) {
            gpuBoostCapacityMax = adpfs[i]["GpuCapacityBoostMax"].asUInt64();
        }
        if (!adpfs[i]["GpuCapacityLoadUpHeadroom"].empty() &&
            adpfs[i]["GpuCapacityLoadUpHeadroom"].isUInt64()) {
            gpuCapacityLoadUpHeadroom = adpfs[i]["GpuCapacityLoadUpHeadroom"].asUInt64();
        }

        if (!adpfs[i]["HBoostUclampMinCeilingRange"].empty()) {
            Json::Value ceilRange = adpfs[i]["HBoostUclampMinCeilingRange"];
            if (ceilRange.size() == 2 && ceilRange[0].isUInt() && ceilRange[1].isUInt()) {
                hBoostUclampMinCeilingRange =
                        std::make_pair(ceilRange[0].asUInt(), ceilRange[1].asUInt());
            }
        }

        if (!adpfs[i]["HBoostUclampMinFloorRange"].empty()) {
            Json::Value floorRange = adpfs[i]["HBoostUclampMinFloorRange"];
            if (floorRange.size() == 2 && floorRange[0].isUInt() && floorRange[1].isUInt()) {
                hBoostUclampMinFloorRange =
                        std::make_pair(floorRange[0].asUInt(), floorRange[1].asUInt());
            }
        }

        // Check all the heuristic configurations are there if heuristic boost is going to
        // be used.
        if (heuristicBoostOn.has_value()) {
            if (!hBoostModerateJankThreshold.has_value() || !hBoostOffMaxAvgDurRatio.has_value() ||
                !hBoostSevereJankPidPu.has_value() || !hBoostSevereJankThreshold.has_value() ||
                !hBoostUclampMinCeilingRange.has_value() ||
                !hBoostUclampMinFloorRange.has_value() || !jankCheckTimeFactor.has_value() ||
                !lowFrameRateThreshold.has_value() || !maxRecordsNum.has_value()) {
                LOG(ERROR) << "Part of the heuristic boost configurations are missing!";
                adpfs_parsed.clear();
                return adpfs_parsed;
            }

            // check heuristic rampup configurations.
            if (heuristicRampup.has_value() &&
                (!defaultRampupMult.has_value() || !highRampupMult.has_value())) {
                LOG(ERROR) << "Part of the heuristic rampup configurations are missing!";
                adpfs_parsed.clear();
                return adpfs_parsed;
            }
        }

        if (uclampMaxEfficientBase.has_value() != uclampMaxEfficientBase.has_value()) {
            LOG(ERROR) << "Part of the power efficiency configuration is missing!";
            adpfs_parsed.clear();
            return adpfs_parsed;
        }

        if (!uclampMinLoadUp.has_value()) {
            uclampMinLoadUp = uclampMinHighLimit;
        }
        if (!uclampMinLoadReset.has_value()) {
            uclampMinLoadReset = uclampMinHighLimit;
        }

        adpfs_parsed.emplace_back(std::make_shared<AdpfConfig>(
                name, pidOn, pidPOver, pidPUnder, pidI, pidIInit, pidIHighLimit, pidILowLimit,
                pidDOver, pidDUnder, adpfUclamp, uclampMinInit, uclampMinHighLimit,
                uclampMinLowLimit, samplingWindowP, samplingWindowI, samplingWindowD, reportingRate,
                targetTimeFactor, staleTimeFactor, gpuBoost, gpuBoostCapacityMax,
                gpuCapacityLoadUpHeadroom, heuristicBoostOn, hBoostModerateJankThreshold,
                hBoostOffMaxAvgDurRatio, hBoostSevereJankPidPu, hBoostSevereJankThreshold,
                hBoostUclampMinCeilingRange, hBoostUclampMinFloorRange, jankCheckTimeFactor,
                lowFrameRateThreshold, maxRecordsNum, heuristicRampup, defaultRampupMult,
                highRampupMult, uclampMinLoadUp.value(), uclampMinLoadReset.value(),
                uclampMaxEfficientBase, uclampMaxEfficientOffset));
    }
    LOG(INFO) << adpfs_parsed.size() << " AdpfConfigs parsed successfully";
    return adpfs_parsed;
}

// TODO(jimmyshiu@): Deprecated. Remove once all powerhint.json up-to-date.
std::shared_ptr<AdpfConfig> HintManager::GetAdpfProfileFromDoHint() const {
    if (adpfs_.empty())
        return nullptr;
    return adpfs_[adpf_index_];
}

// TODO(jimmyshiu@): Deprecated. Remove once all powerhint.json up-to-date.
bool HintManager::SetAdpfProfileFromDoHint(const std::string &profile_name) {
    for (std::size_t i = 0; i < adpfs_.size(); ++i) {
        if (adpfs_[i]->mName == profile_name) {
            if (adpf_index_ != i) {
                ATRACE_NAME(StringPrintf("%s %s:%s", __func__, adpfs_[adpf_index_]->mName.c_str(),
                                         profile_name.c_str())
                                    .c_str());
                adpf_index_ = i;
            }
            return true;
        }
    }
    return false;
}

bool HintManager::IsAdpfSupported() const {
    return !adpfs_.empty();
}

std::shared_ptr<AdpfConfig> HintManager::GetAdpfProfile(const std::string &tag) const {
    if (adpfs_.empty())
        return nullptr;
    if (tag_profile_map_.find(tag) == tag_profile_map_.end()) {
        // TODO(jimmyshiu@): `return adpfs_[0]` once the GetAdpfProfileFromDoHint() retired.
        return GetAdpfProfileFromDoHint();
    }
    return tag_profile_map_.at(tag);
}

bool HintManager::SetAdpfProfile(const std::string &tag, const std::string &profile) {
    if (tag_profile_map_.find(tag) == tag_profile_map_.end()) {
        LOG(WARNING) << "SetAdpfProfile('" << tag << "', " << profile << ") Invalidate Tag!!!";
        return false;
    }
    if (tag_profile_map_[tag]->mName == profile) {
        LOG(VERBOSE) << "SetAdpfProfile:(" << tag << ", " << profile << ") value not changed!";
        return true;
    }

    bool updated = false;
    for (std::size_t i = 0; i < adpfs_.size(); ++i) {
        if (adpfs_[i]->mName == profile) {
            LOG(DEBUG) << "SetAdpfProfile('" << tag << "', '" << profile << "') Done!";
            tag_profile_map_[tag] = adpfs_[i];
            updated = true;
            break;
        }
    }
    if (!updated) {
        LOG(WARNING) << "SetAdpfProfile(" << tag << ") failed to find profile:'" << profile << "'";
    }
    return updated;
}

bool HintManager::IsAdpfProfileSupported(const std::string &profile_name) const {
    for (std::size_t i = 0; i < adpfs_.size(); ++i) {
        if (adpfs_[i]->mName == profile_name) {
            return true;
        }
    }
    return false;
}

void HintManager::OnNodeUpdate(const std::string &name,
                               __attribute__((unused)) const std::vector<std::string> &paths,
                               const std::string &value) {
    // Check if the node is to update ADPF.
    for (const auto &path : paths) {
        if (path.starts_with(kAdpfEventNodePath)) {
            std::string tag = path.substr(strlen(kAdpfEventNodePath));
            bool updated = SetAdpfProfile(tag, value);
            if (!updated) {
                LOG(DEBUG) << "OnNodeUpdate:[" << name << "] failed to update '" << value << "'";
                return;
            }
            auto &callback_list = tag_update_callback_list_[tag];
            for (const auto &callback : callback_list) {
                (*callback)(tag_profile_map_[tag]);
            }
        }
    }
}

void HintManager::RegisterAdpfUpdateEvent(const std::string &tag, AdpfCallback *update_adpf_func) {
    tag_update_callback_list_[tag].push_back(update_adpf_func);
}

void HintManager::UnregisterAdpfUpdateEvent(const std::string &tag,
                                            AdpfCallback *update_adpf_func) {
    auto &callback_list = tag_update_callback_list_[tag];
    // Use std::find to locate the function object
    auto it = std::find_if(
            callback_list.begin(), callback_list.end(),
            [update_adpf_func](const std::function<void(const std::shared_ptr<AdpfConfig>)> *func) {
                return func == update_adpf_func;
            });
    if (it != callback_list.end()) {
        // Erase the found function object
        callback_list.erase(it);
    }
}

std::optional<std::string> HintManager::gpu_sysfs_config_path() const {
    return other_configs_.GPUSysfsPath;
}

OtherConfigs HintManager::GetOtherConfigs() const {
    return other_configs_;
}

}  // namespace perfmgr
}  // namespace android
