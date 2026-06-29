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
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG (ATRACE_TAG_POWER | ATRACE_TAG_HAL)
#define LOG_TAG "libperfmgr"

#include "perfmgr/NodeLooperThread.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <processgroup/processgroup.h>
#include <utils/Trace.h>

namespace android {
namespace perfmgr {

status_t NodeLooperThread::readyToRun() {
    // set task profile "PreferIdle" to lower scheduling latency.
    if (!SetTaskProfiles(0, {"PreferIdleSet"})) {
        LOG(WARNING) << "Device does not support 'PreferIdleSet' task profile.";
    }
    return NO_ERROR;
}

bool NodeLooperThread::Request(const std::vector<NodeAction>& actions,
                               const std::string& hint_type) {
    if (::android::Thread::exitPending()) {
        LOG(WARNING) << "NodeLooperThread is exiting";
        return false;
    }
    if (!::android::Thread::isRunning()) {
        LOG(WARNING) << "NodeLooperThread is not running, request " << hint_type;
    }

    Job *job = jobmgr_.getFreeJob();
    job->is_cancel = false;
    job->hint_type = hint_type;
    job->schedule_time = std::chrono::steady_clock::now();
    ATRACE_BEGIN(("enq:+" + hint_type).c_str());
    for (const auto& a : actions) {
        std::string act_name = nodes_[a.node_index]->GetName();
        ATRACE_BEGIN(act_name.c_str());
        job->actions.push_back(a);
        ATRACE_END();
    }
    jobmgr_.enqueueRequest(job);
    LOG(VERBOSE) << "JobQueue[+].size:" << jobmgr_.getSize();
    ATRACE_END();
    wake_cond_.signal();
    return true;
}

bool NodeLooperThread::Cancel(const std::vector<NodeAction>& actions,
                              const std::string& hint_type) {
    if (::android::Thread::exitPending()) {
        LOG(WARNING) << "NodeLooperThread is exiting";
        return false;
    }
    if (!::android::Thread::isRunning()) {
        LOG(WARNING) << "NodeLooperThread is not running, cancel " << hint_type;
    }

    Job *job = jobmgr_.getFreeJob();
    job->is_cancel = true;
    job->hint_type = hint_type;
    job->schedule_time = std::chrono::steady_clock::now();
    ATRACE_BEGIN(("enq:-" + hint_type).c_str());
    for (const auto& a : actions) {
        std::string act_name = nodes_[a.node_index]->GetName();
        ATRACE_BEGIN(act_name.c_str());
        job->actions.push_back(a);
        ATRACE_END();
    }
    jobmgr_.enqueueRequest(job);
    ATRACE_END();
    wake_cond_.signal();
    return true;
}

void NodeLooperThread::DumpToFd(int fd) {
    ::android::AutoMutex _l(lock_);
    for (auto& n : nodes_) {
        n->DumpToFd(fd);
    }
    jobmgr_.DumpToFd(fd);
}

bool NodeLooperThread::threadLoop() {
    Job *job = jobmgr_.dequeueRequest();
    ::android::AutoMutex _l(lock_);

    if (job != nullptr) {
        ATRACE_BEGIN(("deq:" + job->hint_type + (job->is_cancel ? ":-" : ":+")).c_str());
        for (const auto &a : job->actions) {
            std::string node_name = nodes_[a.node_index]->GetName();
            if (!a.enable_property.empty() &&
                !android::base::GetBoolProperty(a.enable_property, true)) {
                ATRACE_BEGIN((node_name + ":prop:disabled").c_str());
                // Disabled action based on its control property
                ATRACE_END();
                continue;
            }
            if ((a.enable_flag != nullptr && !a.enable_flag()) ||
                (a.disable_flag != nullptr && a.disable_flag())) {
                continue;
            }
            if (a.node_index >= nodes_.size()) {
                LOG(ERROR) << "Node index out of bound: " << a.node_index
                           << " ,size: " << nodes_.size();
                ATRACE_NAME((node_name + ":out-of-bound").c_str());
                continue;
            } else if (job->is_cancel) {
                ATRACE_BEGIN((node_name + ":disable").c_str());
                nodes_[a.node_index]->RemoveRequest(job->hint_type);
                ATRACE_END();
            } else {
                ATRACE_BEGIN((node_name + ":enable").c_str());
                // End time set to steady time point max
                ReqTime end_time = ReqTime::max();
                // Timeout is non-zero
                if (a.timeout_ms != std::chrono::milliseconds::zero()) {
                    auto now = job->schedule_time;  // std::chrono::steady_clock::now();
                    // Overflow protection in case timeout_ms is too big to
                    // overflow time point which is unsigned integer
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(ReqTime::max() -
                                                                              now) > a.timeout_ms) {
                        end_time = now + a.timeout_ms;
                    }
                }
                bool ok = nodes_[a.node_index]->AddRequest(a.value_index, job->hint_type, end_time);
                if (!ok) {
                    LOG(ERROR) << "Node.AddRequest err: Node[" << node_name << "][" << a.value_index
                               << "]";
                }
                ATRACE_END();
            }
        }
        ATRACE_END();
        jobmgr_.returnJob(job);
        LOG(VERBOSE) << "JobQueue[-].size:" << jobmgr_.getSize();
    }

    // Update 2 passes: some node may have dependency in other node
    // e.g. update cpufreq min to VAL while cpufreq max still set to
    // a value lower than VAL, is expected to fail in first pass
    std::chrono::milliseconds timeout_ms = kMaxUpdatePeriod;
    ATRACE_BEGIN("update_nodes");
    for (auto& n : nodes_) {
        n->Update(false);
    }
    for (auto& n : nodes_) {
        timeout_ms = std::min(n->Update(true), timeout_ms);
    }
    ATRACE_END();

    nsecs_t sleep_timeout_ns = std::numeric_limits<nsecs_t>::max();
    if (timeout_ms.count() < sleep_timeout_ns / 1000 / 1000) {
        sleep_timeout_ns = timeout_ms.count() * 1000 * 1000;
    }
    // VERBOSE level won't print by default in user/userdebug build
    LOG(VERBOSE) << "NodeLooperThread will wait for " << sleep_timeout_ns
                 << "ns";
    ATRACE_BEGIN("wait");
    if (jobmgr_.getSize()) {
        LOG(VERBOSE) << "JobQueue not empty, size:" << jobmgr_.getSize()
                     << ". Alter sleep_timeout_ns to 0";
        sleep_timeout_ns = 0;
    }
    wake_cond_.waitRelative(lock_, sleep_timeout_ns);
    ATRACE_END();
    return true;
}

bool NodeLooperThread::Start() {
    auto ret = this->run("NodeLooperThread", PRIORITY_HIGHEST);
    if (ret != NO_ERROR) {
        LOG(ERROR) << "NodeLooperThread start failed: " << ret;
    } else {
        LOG(INFO) << "NodeLooperThread started";
    }
    return ret == NO_ERROR;
}

void NodeLooperThread::Stop() {
    if (::android::Thread::isRunning()) {
        LOG(INFO) << "NodeLooperThread stopping";
        {
            ::android::AutoMutex _l(lock_);
            wake_cond_.signal();
            ::android::Thread::requestExit();
        }
        ::android::Thread::join();
        LOG(INFO) << "NodeLooperThread stopped";
    }
}

}  // namespace perfmgr
}  // namespace android
