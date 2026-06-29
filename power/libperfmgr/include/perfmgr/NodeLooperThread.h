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

#ifndef ANDROID_LIBPERFMGR_NODELOOPERTHREAD_H_
#define ANDROID_LIBPERFMGR_NODELOOPERTHREAD_H_

#include <utils/Thread.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfmgr/FlagProvider.h"
#include "perfmgr/JobQueueManager.h"
#include "perfmgr/Node.h"

namespace android {
namespace perfmgr {

// The NodeAction specifies the sysfs node, the value to be assigned, and the
// timeout for this action:
struct NodeAction {
    NodeAction(std::size_t node_index, std::size_t value_index,
               std::chrono::milliseconds timeout_ms, const std::string &enable_property = "",
               const std::string &enable_flag_str = "", const std::string &disable_flag_str = "")
        : node_index(node_index),
          value_index(value_index),
          timeout_ms(timeout_ms),
          enable_property(enable_property) {
        if (!enable_flag_str.empty()) {
            enable_flag = FlagProvider::GetInstance().GetterFromString(enable_flag_str);
        }
        if (!disable_flag_str.empty()) {
            disable_flag = FlagProvider::GetInstance().GetterFromString(disable_flag_str);
        }
    }
    std::size_t node_index;
    std::size_t value_index;
    std::chrono::milliseconds timeout_ms;  // 0ms for forever
    std::string enable_property;           // boolean property to control action on/off.
    bool (*enable_flag)() = nullptr;
    bool (*disable_flag)() = nullptr;
};

// The NodeLooperThread is responsible for managing each of the sysfs nodes
// specified in the configuration. At initialization, the NodeLooperThrea holds
// a vector containing the nodes defined in the configuration. The NodeManager
// gets powerhint requests and cancellations from the HintManager, maintains
// state about the current set of powerhint requests on each sysfs node, and
// decides how to apply the requests. The NodeLooperThread contains a ThreadLoop
// to maintain the sysfs nodes, and that thread is woken up both to handle
// powerhint requests and when the timeout expires for an in-progress powerhint.
class NodeLooperThread : public ::android::Thread {
  public:
    explicit NodeLooperThread(std::vector<std::unique_ptr<Node>> nodes)
        : Thread(false), nodes_(std::move(nodes)) {}
    virtual ~NodeLooperThread() { Stop(); }

    // Need call Stop() as the threadloop will hold a strong pointer
    // itself and wait for Condition fired or timeout (60s) before
    // the out looper can call deconstructor to Stop() thread
    void Stop();

    // Return true when successfully adds request from actions for the hint_type
    // in each individual node. Return false if any of the actions has either
    // invalid node index or value index.
    bool Request(const std::vector<NodeAction>& actions,
                 const std::string& hint_type);
    // Return when successfully cancels request from actions for the hint_type
    // in each individual node. Return false if any of the actions has invalid
    // node index.
    bool Cancel(const std::vector<NodeAction>& actions,
                const std::string& hint_type);

    // Dump all nodes to fd
    void DumpToFd(int fd);

    // Return true when successfully started the looper thread
    bool Start();

  private:
    NodeLooperThread(NodeLooperThread const&) = delete;
    NodeLooperThread &operator=(NodeLooperThread const &) = delete;

    status_t readyToRun() override;
    bool threadLoop() override;

    static constexpr auto kMaxUpdatePeriod = std::chrono::milliseconds::max();

    std::vector<std::unique_ptr<Node>> nodes_;  // parsed from Config

    // conditional variable from C++ standard library can be affected by wall
    // time change as it is using CLOCK_REAL (b/35756266). The component should
    // not be impacted by wall time, thus need use Android specific Condition
    // class for waking up threadloop.
    ::android::Condition wake_cond_;

    // lock to protect nodes_
    ::android::Mutex lock_;

    // Job queue for threadloop to process
    JobQueueManager jobmgr_;
};

}  // namespace perfmgr
}  // namespace android

#endif  // ANDROID_LIBPERFMGR_NODELOOPERTHREAD_H_
