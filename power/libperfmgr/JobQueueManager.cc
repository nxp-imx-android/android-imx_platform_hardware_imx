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

#define ATRACE_TAG (ATRACE_TAG_POWER | ATRACE_TAG_HAL)
#define LOG_TAG "libperfmgr"

#include "perfmgr/JobQueueManager.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <utils/Trace.h>

#include "perfmgr/NodeLooperThread.h"

namespace android {
namespace perfmgr {

JobQueueManager::JobQueueManager(size_t poolSize) : mPoolSize(poolSize) {
    for (size_t i = 0; i < mPoolSize; ++i) {
        Job *job = new Job();
        mJobPool.push_back(job);  // Add to the pool
    }
}

void JobQueueManager::enqueueRequest(Job *job) {
    ::android::AutoMutex _l(mQueueMutex);
    // This is a priority_queue(automatically sort the jobs by schedule_time)
    mJobQueue.push(job);
}

Job *JobQueueManager::dequeueRequest() {
    ::android::AutoMutex _l(mQueueMutex);
    if (mJobQueue.empty()) {
        return nullptr;
    }
    Job *job = mJobQueue.top();
    mJobQueue.pop();
    return job;
}

Job *JobQueueManager::getFreeJob() {
    ::android::AutoMutex _l(mQueueMutex);
    if (mJobPool.empty()) {
        // If pool is empty, allocate a new job on the heap.
        // This can happen if the pool size is not sufficient, or
        // if a job is not returned to the pool correctly.
        std::string warning = "PowerHAL:JobPoolEmpty[queue:" + std::to_string(mJobQueue.size()) +
                              ",pool: " + std::to_string(mPoolSize) +
                              ",limit:" + std::to_string(mPoolSize) + "]";
        LOG(WARNING) << warning;
        ATRACE_NAME(warning.c_str());
        return new Job();
    }
    Job *job = mJobPool.front();
    mJobPool.pop_front();
    return job;
}

void JobQueueManager::returnJob(Job *job) {
    ::android::AutoMutex _l(mQueueMutex);
    job->reset();  // Reset the job's content
    mJobPool.push_back(job);
}

size_t JobQueueManager::getSize() {
    ::android::AutoMutex _l(mQueueMutex);
    return mJobQueue.size();
}

void JobQueueManager::DumpToFd(int fd) {
    ::android::AutoMutex _l(mQueueMutex);

    std::string buf = android::base::StringPrintf(
            "Job Queue Dump:\n"
            "-------------------\n"
            "Queue Size: %zu\n"
            "Pool Size: %zu\n"
            "-------------------\n",
            mJobQueue.size(), mJobPool.size());
    if (!android::base::WriteStringToFd(buf, fd)) {
        LOG(ERROR) << "Failed to dump queue info to fd: " << fd;
    }

    // Dump Job Queue
    if (!mJobQueue.empty()) {
        buf = "Job Queue:\n";
        if (!android::base::WriteStringToFd(buf, fd)) {
            LOG(ERROR) << "Failed to write queue header to fd: " << fd;
        }

        // Directly dump jobs from mJobQueue and re-push them.
        std::priority_queue<Job *, std::vector<Job *>, JobComparator> tempQueue;
        for (auto it = mJobQueue.size(); it > 0; --it) {
            Job *job = mJobQueue.top();
            mJobQueue.pop();
            buf = android::base::StringPrintf(
                    "  Hint Type: %s, Schedule Time: %lld, Is Cancel: %d\n", job->hint_type.c_str(),
                    job->schedule_time.time_since_epoch().count(), job->is_cancel);
            if (!android::base::WriteStringToFd(buf, fd)) {
                LOG(ERROR) << "Failed to dump job info to fd: " << fd;
            }
            tempQueue.push(job);
        }

        // Restore mJobQueue
        while (!tempQueue.empty()) {
            mJobQueue.push(tempQueue.top());
            tempQueue.pop();
        }
    }
}

void Job::reset() {
    actions.clear();
    hint_type.clear();
    schedule_time = std::chrono::steady_clock::now();
    is_cancel = false;
}

}  // namespace perfmgr
}  // namespace android
