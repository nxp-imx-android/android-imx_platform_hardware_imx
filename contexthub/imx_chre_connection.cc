/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "imx_chre_connection.h"
#include "chre_host/file_stream.h"
#include "chre_host/generated/host_messages_generated.h"
#include "chre_host/host_protocol_host.h"

#include <hardware_legacy/power.h>
#include <sys/ioctl.h>
#include <utils/SystemClock.h>
#include <cerrno>
#include <thread>
#include <sys/epoll.h>
#include <linux/rpmsg.h>
/* The definitions below must be the same as the ones defined in kernel. */

namespace aidl::android::hardware::contexthub {

using namespace ::android::chre;
namespace fbs = ::chre::fbs;

namespace {

// Possible states of SCP.
enum ChreState {
  SCP_CHRE_UNINIT = 0,
  SCP_CHRE_STOP = 1,
  SCP_CHRE_START = 2,
};

ChreState chreCurrentState = SCP_CHRE_UNINIT;

int createEpollFd(int fdToEpoll) {
  struct epoll_event event;
  event.data.fd = fdToEpoll;
  event.events = EPOLLIN | EPOLLWAKEUP;
  int epollFd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_ctl(epollFd, EPOLL_CTL_ADD, event.data.fd, &event) != 0) {
    LOGE("Failed to add control interface to msg read fd errno: %s",
         strerror(errno));
    close(epollFd);
    epollFd = -1;
  }
  return epollFd;
}

}  // namespace

bool ImxChreConnection::init() {
  // Make sure the payload size is large enough for nanoapp binary fragment
  static_assert(kMaxSendingPayloadBytes > CHRE_HOST_DEFAULT_FRAGMENT_SIZE &&
                kMaxSendingPayloadBytes - CHRE_HOST_DEFAULT_FRAGMENT_SIZE >
                    kMaxPayloadOverheadBytes);
  mChreFileDescriptor =
      TEMP_FAILURE_RETRY(open(kChreFileDescriptorPath, O_RDWR));
  if (mChreFileDescriptor < 0) {
    LOGE("open chre device failed err=%d errno=%d\n", mChreFileDescriptor,
         errno);
    return false;
  }
  // launch the tasks
  mMessageListener = std::thread(messageListenerTask, this);
  mMessageSender = std::thread(messageSenderTask, this);
  mStateListener = std::thread(chreStateMonitorTask, this);
  return true;
}

[[noreturn]] void ImxChreConnection::messageListenerTask(
    ImxChreConnection *chreConnection) {
  auto chreFd = chreConnection->getChreFileDescriptor();
  int epollFd = createEpollFd(chreFd);

  while (true) {
    struct epoll_event retEvent;
    int nEvents = epoll_wait(epollFd, &retEvent, 1 /* maxEvents */,
                             -1 /* infinite timeout */);
    if (nEvents < 0) {
      // epoll_wait will get interrupted if the CHRE daemon is shutting down,
      // check this condition before logging an error.
      LOGE("no events");
    } else if (nEvents == 0) {
      LOGW("Epoll returned with 0 FDs ready despite no timeout (errno: %s)",
           strerror(errno));
    } else {
      ssize_t payloadSize = read(chreFd, chreConnection->mPayload.get(), kMaxReceivingPayloadBytes);
      if (payloadSize == 0) {
        // Payload size 0 is a fake signal from kernel which is normal if the
        // device is in sleep.
        LOGV("%s: Received a payload size 0. Ignored. errno=%d", __func__,
             errno);
        continue;
      }
      if (payloadSize < 0) {
        LOGE("%s: read failed. payload size: %zu. errno=%d", __func__,
             payloadSize, errno);
        continue;
      }
      handleMessageFromChre(chreConnection, chreConnection->mPayload.get(),
                            payloadSize);
    }
  }
  close(epollFd);
}

[[noreturn]] void ImxChreConnection::chreStateMonitorTask(
    ImxChreConnection *chreConnection) {
  int chreFd = chreConnection->getChreFileDescriptor();
  int nextState = 0;
  while (true) {
    if (TEMP_FAILURE_RETRY(ioctl(chreFd, RPMSG_CHRE_GET_STATE,
                                 &nextState)) < 0) {
      LOGE("Unable to get an update for the CHRE state: errno=%d", errno);
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    auto chreNextState = static_cast<ChreState>(nextState);
    if (chreCurrentState != chreNextState) {
      LOGI("CHRE state changes from %" PRIu32 " to %" PRIu32, chreCurrentState,
           chreNextState);
    }
    if (chreCurrentState == SCP_CHRE_STOP && chreNextState == SCP_CHRE_START) {
      int64_t startTime = ::android::elapsedRealtime();
      // Though usually CHRE is recovered within 1s after SCP is up, in a corner
      // case it can go beyond 5s. Wait for 10s to cover more extreme cases.
      chreConnection->waitChreBackOnline(
          /* timeoutMs= */ std::chrono::milliseconds(10000));
      LOGW("SCP restarted! CHRE recover time: %" PRIu64 "ms.",
           ::android::elapsedRealtime() - startTime);
      chreConnection->getCallback()->onChreReconnected();
    } else if (chreCurrentState == SCP_CHRE_START && chreNextState == SCP_CHRE_STOP) {
      chreConnection->getCallback()->onChreDisconnected();
    }

    chreCurrentState = chreNextState;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

[[noreturn]] void ImxChreConnection::messageSenderTask(
    ImxChreConnection *chreConnection) {
  LOGI("Message sender task is launched.");
  int chreFd = chreConnection->getChreFileDescriptor();
  while (true) {
    chreConnection->mQueue.waitForMessage();
    ChreConnectionMessage &message = chreConnection->mQueue.front();
    auto size =
        TEMP_FAILURE_RETRY(write(chreFd, message.payload, message.payloadSize));
    if (size < 0) {
      LOGE("Failed to write to chre file descriptor. errno=%d\n", errno);
    }
    chreConnection->mQueue.pop();
  }
}

bool ImxChreConnection::sendMessage(void *data, size_t length) {
  if (length <= 0 || length > kMaxSendingPayloadBytes) {
    LOGE("length %zu is not within the accepted range.", length);
    return false;
  }
  return mQueue.emplace(data, length);
}

void ImxChreConnection::handleMessageFromChre(
    ImxChreConnection *chreConnection, const unsigned char *messageBuffer,
    size_t messageLen) {
  // TODO(b/267188769): Move the wake lock acquisition/release to RAII
  // pattern.
  bool isWakelockAcquired =
      acquire_wake_lock(PARTIAL_WAKE_LOCK, kWakeLock) == 0;
  if (!isWakelockAcquired) {
    LOGE("Failed to acquire the wakelock before handling a message.");
  } else {
    LOGV("Wakelock is acquired before handling a message.");
  }
  HalClientId hostClientId;
  fbs::ChreMessage messageType = fbs::ChreMessage::NONE;
  if (!HostProtocolHost::extractHostClientIdAndType(
          messageBuffer, messageLen, &hostClientId, &messageType)) {
    LOGW("Failed to extract host client ID from message - sending broadcast");
    hostClientId = ::chre::kHostClientIdUnspecified;
  }
  LOGV("Received a message (type: %hhu, len: %zu) from CHRE for client %d",
       messageType, messageLen, hostClientId);

  switch (messageType) {
    case fbs::ChreMessage::PulseResponse: {
      chreConnection->notifyChreBackOnline();
      break;
    }
    case fbs::ChreMessage::MetricLog:
    case fbs::ChreMessage::NanConfigurationRequest:
    case fbs::ChreMessage::TimeSyncRequest:
    case fbs::ChreMessage::LogMessage: {
      LOGE("Unsupported message type %hhu received from CHRE.", messageType);
      break;
    }
    default: {
      chreConnection->getCallback()->handleMessageFromChre(messageBuffer,
                                                           messageLen);
      break;
    }
  }
  if (isWakelockAcquired) {
    if (release_wake_lock(kWakeLock)) {
      LOGE("Failed to release the wake lock");
    } else {
      LOGV("The wake lock is released after handling a message.");
    }
  }
}
}  // namespace aidl::android::hardware::contexthub
