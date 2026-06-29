/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef ANDROID_LIBPERFMGR_JOBQUEUEMANAGER_H_
#define ANDROID_LIBPERFMGR_JOBQUEUEMANAGER_H_

#include <utils/Mutex.h>

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <queue>
#include <vector>

namespace android {
namespace perfmgr {

/* Default pool size for JobQueueManager
 *
 * Observed from the log of CM4 on 2025 March, the queue size reaches ~45 during
 * bootup while the NodeLooperThread is not running. Therefore, set the pool
 * size to 64.
 */
const size_t DEFAULT_POOL_SIZE = 64;

struct NodeAction;  // Forward declaration

struct Job {
    std::vector<NodeAction> actions;  // Replace with your action type
    std::string hint_type;            // Replace with your hint type
    std::chrono::time_point<std::chrono::steady_clock> schedule_time;
    bool is_cancel;  // True if this is a cancel request
    void reset();
};

// Custom comparator for priority_queue (earlier schedule_time has higher priority)
struct JobComparator {
    bool operator()(const Job *a, const Job *b) const {
        return a->schedule_time > b->schedule_time;  // Earlier time means higher priority
    }
};

class JobQueueManager {
  public:
    JobQueueManager(size_t poolSize = DEFAULT_POOL_SIZE);  // Constructor with pool size

    // Add a job to the queue
    void enqueueRequest(Job *job);

    // Get the next job from the queue
    Job *dequeueRequest();
    Job *getFreeJob();
    void returnJob(Job *job);
    size_t getSize();

    // Dump messages to fd
    void DumpToFd(int fd);

  private:
    // Job will be auto sorted by JobComparator in priority_queue
    std::priority_queue<Job *, std::vector<Job *>, JobComparator> mJobQueue;
    ::android::Mutex mQueueMutex;  // Mutex to protect the queue
    std::deque<Job *> mJobPool;    // Use deque for efficient push/pop from both ends
    size_t mPoolSize;
};

}  // namespace perfmgr
}  // namespace android

#endif  // ANDROID_LIBPERFMGR_JOBQUEUEMANAGER_H_
