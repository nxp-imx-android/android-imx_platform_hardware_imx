/*
 * Copyright (C) 2022 The Android Open Source Project
 * Copyright 2024 NXP
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

#include "EvsV4l2Enumerator.h"

#include "ConfigManager.h"
#include "EvsGlDisplay.h"
#include "EvsV4l2Camera.h"
#include "RouteMediactl.h"

#include <aidl/android/hardware/automotive/evs/DeviceStatusType.h>
#include <aidl/android/hardware/automotive/evs/EvsResult.h>
#include <aidl/android/hardware/automotive/evs/Rotation.h>
#include <aidl/android/hardware/graphics/common/BufferUsage.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>
#include <android-base/file.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <cutils/android_filesystem_config.h>
#include <cutils/properties.h>

#include <dirent.h>
#include <sys/epoll.h>
#include <sys/inotify.h>

#include <string_view>
#include <string.h>

namespace {

using ::aidl::android::frameworks::automotive::display::ICarDisplayProxy;
using ::aidl::android::hardware::automotive::evs::DeviceStatusType;
using ::aidl::android::hardware::automotive::evs::EvsResult;
using ::aidl::android::hardware::automotive::evs::Rotation;
using ::aidl::android::hardware::graphics::common::BufferUsage;
using ::android::base::EqualsIgnoreCase;
using ::android::base::StringPrintf;
using ::android::base::WriteStringToFd;
using ::ndk::ScopedAStatus;
using std::chrono_literals::operator""s;

// Constants
constexpr std::chrono::seconds kEnumerationTimeout = 10s;
constexpr uint64_t kInvalidDisplayId = std::numeric_limits<uint64_t>::max();
const std::set<uid_t> kAllowedUids = {AID_AUTOMOTIVE_EVS, AID_SYSTEM, AID_ROOT};

#define HWC_PATH_LENGTH 64
#define BUFFER_SIZE 512
#define EPOLL_MAX_EVENTS 8
#define MEDIA_FILE_PATH "/dev"
#define EVS_VIDEO_READY "vendor.evs.video.ready"
#define EVS_VIDEO_DEV   "vendor.evs.video.dev"
#define EVS_ISI_NAME    "vendor.evs.isi.name"
#define EVS_FAKE_SENSOR "mxc_isi.0.capture"
#define EVS_FAKE_LOGIC_CAMERA "group0"
#define EVS_FAKE_NAME   "fake.camera"
#define EVS_FAKE_LOGIC_NAME   "fake.logic.camera"
#define FAKE_CAMERA_WIDTH 1920
#define FAKE_CAMERA_HEIGHT 1024

}  // namespace

namespace aidl::android::hardware::automotive::evs::implementation {

// NOTE:  All members values are static so that all clients operate on the same state
//        That is to say, this is effectively a singleton despite the fact that HIDL
//        constructs a new instance for each client.
std::list<EvsEnumerator::CameraRecord> EvsEnumerator::sCameraList;
std::mutex EvsEnumerator::sLock;
std::condition_variable EvsEnumerator::sCameraSignal;
std::unique_ptr<ConfigManager> EvsEnumerator::sConfigManager;
std::shared_ptr<ICarDisplayProxy> EvsEnumerator::sDisplayProxy;
std::unordered_map<uint8_t, uint64_t> EvsEnumerator::sDisplayPortList;

EvsEnumerator::ActiveDisplays& EvsEnumerator::mutableActiveDisplays() {
    static ActiveDisplays active_displays;
    return active_displays;
}

void EvsEnumerator::EvsHotplugThread(std::shared_ptr<EvsEnumerator> service,
                                     std::atomic<bool>& running) {
    // Watch new video devices
    if (!service) {
        LOG(ERROR) << "EvsEnumerator is invalid";
        return;
    }
    epoll_event eventItem;
    int mINotifyFd = inotify_init();
    if (mINotifyFd < 0) {
        ALOGE("Fail to initialize inotify fd, error:%s",strerror(errno));
        return;
    }
    int mINotifyWd = inotify_add_watch(mINotifyFd, MEDIA_FILE_PATH, IN_CREATE);
    if (mINotifyWd < 0) {
        ALOGE("Fail to add watch for %s,error:%s", MEDIA_FILE_PATH, strerror(errno));
        close(mINotifyFd);
        return;
    }

    int mEpollFd = epoll_create(1);
    if (mEpollFd == -1) {
        ALOGE("Fail to create epoll instance, error:%s",strerror(errno));
        inotify_rm_watch(mINotifyFd,mINotifyWd);
        close(mINotifyFd);
        return;
    }

    memset(&eventItem, 0, sizeof(epoll_event));
    eventItem.events = EPOLLIN;
    eventItem.data.fd = mINotifyFd;
    int result = epoll_ctl(mEpollFd, EPOLL_CTL_ADD, mINotifyFd, &eventItem);
    if (result == -1) {
        ALOGE("Fail to add inotify to epoll instance, error:%s",strerror(errno));
        inotify_rm_watch(mINotifyFd,mINotifyWd);
        close(mINotifyFd);
        close(mEpollFd);
        return;
    }

    int numEpollEvent;
    epoll_event epollItems[EPOLL_MAX_EVENTS];
    bool foundMediaDevice = false;

    while (running) {
        numEpollEvent = epoll_wait(mEpollFd, epollItems, EPOLL_MAX_EVENTS, -1);
        if (numEpollEvent <= 0) {
            ALOGE("Fail to wait requested events,numEpollEvent:%d,error:%s",numEpollEvent,strerror(errno));
        } else {
            for (int i=0; i < numEpollEvent; i++) {
                if (epollItems[i].events & (EPOLLERR|EPOLLHUP)) {
                    continue;
                }
                if (epollItems[i].events & EPOLLIN) {
                    char buf[BUFFER_SIZE];
                    int numINotifyItem = read(mINotifyFd, buf, BUFFER_SIZE);
                    if (numINotifyItem < 0) {
                        ALOGE("Fail to read from INotifyFd,error:%s",strerror(errno));
                        continue;
                    }

                    //Each successful read returns a buffer containing one or more of struct inotify_event
                    //The length of each inotify_event structure is sizeof(struct inotify_event)+len.
                    for (char *inotifyItemBuf = buf; inotifyItemBuf < buf+numINotifyItem;) {
                        struct inotify_event *inotifyItem = (struct inotify_event *)inotifyItemBuf;
                        if (strstr(inotifyItem->name,"media") ||
                             ((foundMediaDevice == 1) && strstr(inotifyItem->name,"v4l")) ||
                             ((foundMediaDevice == 1) && strstr(inotifyItem->name,"video"))
                        ) {
                                foundMediaDevice = 1;
                            //detect /dev/media* has been created
                            if(enumerateCameras()) {
                                inotify_rm_watch(mINotifyFd,mINotifyWd);
                            }
                        }
                        inotifyItemBuf += sizeof(struct inotify_event) + inotifyItem->len;
                    }
                }
            }
        }
    }
}

bool EvsEnumerator::filterVideoFromConfigure(char *deviceName) {
    if (sConfigManager == nullptr)
        return true;

    std::vector<std::string>::iterator index;
    std::vector<std::string> cameraList =
                sConfigManager->getCameraIdList();
    index = find(cameraList.begin(), cameraList.end(), deviceName);
    if(index != cameraList.end())
        return true;
    else
        return false;
}

EvsEnumerator::EvsEnumerator(const std::shared_ptr<ICarDisplayProxy>& proxyService) {
    LOG(DEBUG) << "EvsEnumerator is created.";

    if (!sConfigManager) {
        /* loads and initializes ConfigManager in a separate thread */
        sConfigManager = ConfigManager::Create();
    }

    if (!sDisplayProxy) {
        /* sets a car-window service handle */
        sDisplayProxy = proxyService;
    }

    // Enumerate existing devices
    enumerateCameras();
    mInternalDisplayId = enumerateDisplays();
}

bool EvsEnumerator::checkPermission() {
    const auto uid = AIBinder_getCallingUid();
    if (kAllowedUids.find(uid) == kAllowedUids.end()) {
        LOG(ERROR) << "EVS access denied: "
                   << "pid = " << AIBinder_getCallingPid() << ", uid = " << uid;
        return false;
    }

    return true;
}

bool EvsEnumerator::enumerateCameras() {
    if (sConfigManager == nullptr) {
        /* loads and initializes ConfigManager in a separate thread */
        sConfigManager =
            ConfigManager::Create();
    }

    auto videoCount = 0;
    bool videoReady = false;

    int enableFake = property_get_int32(EVS_FAKE_PROP, 0);
    if (enableFake != 0) {
        /* Support of FAKE camera... TODO */
    } else {
        char camera[PROPERTY_VALUE_MAX];
        char isi[PROPERTY_VALUE_MAX];
        if ((property_get(EVS_VIDEO_DEV, camera, NULL) > 0) && (property_get(EVS_ISI_NAME, isi, NULL) > 0)) {
            ALOGI("Using camera provided by prop: name:%s path:%s", isi, camera);
            sCameraList.emplace_back(isi, camera, hwCam);
            videoCount++;
        } else {
            // For every video* entry in the dev folder, see if it reports suitable capabilities
            // WARNING:  Depending on the driver implementations this could be slow, especially if
            //           there are timeouts or round trips to hardware required to collect the needed
            //           information.  Platform implementers should consider hard coding this list of
            //           known good devices to speed up the startup time of their EVS implementation.
            //           For example, this code might be replaced with nothing more than:
            //                   sCameraList.emplace_back("/dev/video0");
            //                   sCameraList.emplace_back("/dev/video1");
            LOG(INFO) << __FUNCTION__ << ": Starting dev/video* enumeration";
            DIR* dir = opendir("/sys/class/video4linux");
            if (!dir) {
                LOG_FATAL("Failed to open /sys/class/video4linux folder\n");
                goto found;
            }

            struct dirent* entry;
            FILE *fp;
            char devPath[HWC_PATH_LENGTH];
            char value[HWC_PATH_LENGTH];
            int len_val;
            while ((entry = readdir(dir)) != nullptr) {
                // We're only looking for entries starting with 'video'
                char *name = entry->d_name;
                std::string devNode("/dev/");
                devNode += name;
                videoCount++;
                snprintf(devPath, HWC_PATH_LENGTH, "/sys/class/video4linux/%s/name", entry->d_name);
                if ((fp = fopen(devPath, "r")) == nullptr) {
                    ALOGE("can't open %s", devPath);
                    continue;
                }
                if(fgets(value, sizeof(value), fp) == nullptr) {
                    fclose(fp);
                    ALOGE("can't read %s", devPath);
                    continue;
                }
                // last byte is '\n' if get the string through fgets
                // it cause issue that can't find item for camera. set the last byte as '\0'
                len_val = strlen(value) - 1;
                fclose(fp);
                value[len_val] = '\0';
                ALOGI("enum name:%s path:%s", value, devNode.c_str());

                registerDevnode(value, devNode);
                ALOGE("Dev %s : %s", value, devNode.c_str());
                if (!filterVideoFromConfigure(value)) {
                    continue;
                }
                // Skip already enumerated cameras from previous call of `enumerateCameras()`
                bool enumerated = false;
                for (const EvsEnumerator::CameraRecord& item : sCameraList) {
                    if (item.name == value) {
                        enumerated = true;
                        break;
                    }
                }
                if (!enumerated) {
                    if (qualifyCaptureDevice(devNode.c_str())) {
                        sCameraList.emplace_back(value, devNode.c_str(), hwCam);
                    }
                }
            }
            closedir(dir);
        }
    }
    found:
    size_t captureCount = sCameraList.size();

    if (captureCount > 0) {
        if (configure()) {
            videoReady = true;
            if (property_set(EVS_VIDEO_READY, "1") < 0)
                ALOGE("Can not set property %s", EVS_VIDEO_READY);
        }
    }
    LOG(INFO) << "Found " << captureCount << " qualified video capture devices "
              << "of " << videoCount << " checked. Video ready: " << videoReady;
    return videoReady;
}

uint64_t EvsEnumerator::enumerateDisplays() {
    LOG(INFO) << __FUNCTION__ << ": Starting display enumeration";
    uint64_t internalDisplayId = kInvalidDisplayId;
    if (!sDisplayProxy) {
        LOG(ERROR) << "ICarDisplayProxy is not available!";
        return internalDisplayId;
    }

    std::vector<int64_t> displayIds;
    if (auto status = sDisplayProxy->getDisplayIdList(&displayIds); !status.isOk()) {
        LOG(ERROR) << "Failed to retrieve a display id list"
                   << ::android::statusToString(status.getStatus());
        return internalDisplayId;
    }

    if (displayIds.size() > 0) {
        // The first entry of the list is the internal display.  See
        // SurfaceFlinger::getPhysicalDisplayIds() implementation.
        internalDisplayId = displayIds[0];
        for (const auto& id : displayIds) {
            const auto port = id & 0xFF;
            LOG(INFO) << "Display " << std::hex << id << " is detected on the port, " << port;
            sDisplayPortList.insert_or_assign(port, id);
        }
    }

    LOG(INFO) << "Found " << sDisplayPortList.size() << " displays";
    return internalDisplayId;
}

// Methods from ::android::hardware::automotive::evs::IEvsEnumerator follow.
ScopedAStatus EvsEnumerator::getCameraList(std::vector<CameraDesc>* _aidl_return) {
    LOG(DEBUG) << __FUNCTION__;
    if (!checkPermission()) {
        return ScopedAStatus::fromServiceSpecificError(
                static_cast<int>(EvsResult::PERMISSION_DENIED));
    }

    {
        std::unique_lock<std::mutex> lock(sLock);
        if (sCameraList.size() < 1) {
            // No qualified device has been found.  Wait until new device is ready,
            // for 10 seconds.
            if (!sCameraSignal.wait_for(lock, kEnumerationTimeout,
                                        [] { return sCameraList.size() > 0; })) {
                LOG(DEBUG) << "Timer expired.  No new device has been added.";
            }
        }
    }

    // Build up a packed array of CameraDesc for return
    _aidl_return->resize(sCameraList.size());
    if (sConfigManager == nullptr) {
        const unsigned numCameras = sCameraList.size();

        _aidl_return->resize(numCameras);
        unsigned i = 0;
        CameraDesc aCamera;
        for (auto&cam : sCameraList) {
             aCamera.id = cam.name.c_str();
             (*_aidl_return)[i++] = aCamera;
        }
    } else {
        // Build up a packed array of CameraDesc for return
        unsigned i = 0;
        for (auto&cam : sCameraList) {
            CameraDesc aCamera;
            std::unique_ptr<ConfigManager::CameraInfo> &tempInfo =
                sConfigManager->getCameraInfo(cam.name);
            if (tempInfo) {
                uint8_t* ptr = reinterpret_cast<uint8_t*>(tempInfo->characteristics);
                const size_t len = get_camera_metadata_size(tempInfo->characteristics);
                aCamera.metadata.insert(aCamera.metadata.end(), ptr, ptr + len);
            } else {
                continue;
            }
            aCamera.id = cam.name.c_str();
            //_aidl_return->push_back(aCamera);
            (*_aidl_return)[i++] = aCamera;
        }
    }

    // Send back the results
    LOG(DEBUG) << "Reporting " << sCameraList.size() << " cameras available";
    return ScopedAStatus::ok();
}

ScopedAStatus EvsEnumerator::getStreamList(const CameraDesc& desc,
                                           std::vector<Stream>* _aidl_return) {
    using AidlPixelFormat = ::aidl::android::hardware::graphics::common::PixelFormat;

    camera_metadata_t* pMetadata = const_cast<camera_metadata_t*>(
            reinterpret_cast<const camera_metadata_t*>(desc.metadata.data()));
    camera_metadata_entry_t streamConfig;
    if (!find_camera_metadata_entry(pMetadata, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                    &streamConfig)) {
        const unsigned numStreamConfigs = streamConfig.count / sizeof(StreamConfiguration);
        _aidl_return->resize(numStreamConfigs);
        const StreamConfiguration* pCurrentConfig =
                reinterpret_cast<StreamConfiguration*>(streamConfig.data.i32);
        for (unsigned i = 0; i < numStreamConfigs; ++i, ++pCurrentConfig) {
            // Build ::aidl::android::hardware::automotive::evs::Stream from
            // StreamConfiguration.
            Stream current = {
                    .id = pCurrentConfig->id,
                    .streamType = pCurrentConfig->type ==
                                    ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_INPUT
                            ? StreamType::INPUT
                            : StreamType::OUTPUT,
                    .width = pCurrentConfig->width,
                    .height = pCurrentConfig->height,
                    .format = static_cast<AidlPixelFormat>(pCurrentConfig->format),
                    .usage = BufferUsage::CAMERA_INPUT,
                    .rotation = Rotation::ROTATION_0,
            };

            (*_aidl_return)[i] = std::move(current);
        }
    }

    return ScopedAStatus::ok();
}

ScopedAStatus EvsEnumerator::openCamera(const std::string& id, const Stream& cfg,
                                        std::shared_ptr<IEvsCamera>* obj) {
    LOG(DEBUG) << __FUNCTION__;
    if (!checkPermission()) {
        return ScopedAStatus::fromServiceSpecificError(
                static_cast<int>(EvsResult::PERMISSION_DENIED));
    }

    // Is this a recognized camera id?
    CameraRecord* pRecord = findCameraById(id);
    if (!pRecord) {
        LOG(ERROR) << id << " does not exist!";
        return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::INVALID_ARG));
    }

    // Has this camera already been instantiated by another caller?
    std::shared_ptr<EvsV4lCamera> pActiveCamera = pRecord->activeInstance.lock();
    if (pActiveCamera) {
        LOG(WARNING) << "Killing previous camera because of new caller";
        closeCamera(pActiveCamera);
    }

    // Construct a camera instance for the caller
    if (!sConfigManager) {
        pActiveCamera = EvsV4lCamera::Create(id.data());
    } else {
        pActiveCamera = EvsV4lCamera::Create(pRecord->desc.id.data(), sConfigManager->getCameraInfo(id), &cfg);
    }

    pRecord->activeInstance = pActiveCamera;
    if (!pActiveCamera) {
        LOG(ERROR) << "Failed to create new EvsV4lCamera object for " << id;
        return ScopedAStatus::fromServiceSpecificError(
                static_cast<int>(EvsResult::UNDERLYING_SERVICE_ERROR));
    }

    *obj = pActiveCamera;
    return ScopedAStatus::ok();
}

ScopedAStatus EvsEnumerator::closeCamera(const std::shared_ptr<IEvsCamera>& cameraObj) {
    LOG(DEBUG) << __FUNCTION__;

    if (!cameraObj) {
        LOG(ERROR) << "Ignoring call to closeCamera with null camera ptr";
        return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::INVALID_ARG));
    }

    // Get the camera id so we can find it in our list
    CameraDesc desc;
    auto status = cameraObj->getCameraInfo(&desc);
    if (!status.isOk()) {
        LOG(ERROR) << "Failed to read a camera descriptor";
        return ScopedAStatus::fromServiceSpecificError(
                static_cast<int>(EvsResult::UNDERLYING_SERVICE_ERROR));
    }
    auto cameraId = desc.id;
    closeCamera_impl(cameraObj, cameraId);
    return ScopedAStatus::ok();
}

ScopedAStatus EvsEnumerator::openDisplay(int32_t id, std::shared_ptr<IEvsDisplay>* displayObj) {
    LOG(DEBUG) << __FUNCTION__;
    if (!checkPermission()) {
        return ScopedAStatus::fromServiceSpecificError(
                static_cast<int>(EvsResult::PERMISSION_DENIED));
    }

    auto& displays = mutableActiveDisplays();

    if (auto existing_display_search = displays.popDisplay(id)) {
        // If we already have a display active, then we need to shut it down so we can
        // give exclusive access to the new caller.
        std::shared_ptr<EvsGlDisplay> pActiveDisplay = existing_display_search->displayWeak.lock();
        if (pActiveDisplay) {
            LOG(WARNING) << "Killing previous display because of new caller";
            pActiveDisplay->forceShutdown();
        }
    }

    // Create a new display interface and return it
    uint64_t targetDisplayId = mInternalDisplayId;
    auto it = sDisplayPortList.find(id);
    if (it != sDisplayPortList.end()) {
        targetDisplayId = it->second;
    } else {
        LOG(WARNING) << "No display is available on the port " << static_cast<int32_t>(id)
                     << ". The main display " << mInternalDisplayId << " will be used instead";
    }

    // Create a new display interface and return it.
    std::shared_ptr<EvsGlDisplay> pActiveDisplay =
            ndk::SharedRefBase::make<EvsGlDisplay>(sDisplayProxy, targetDisplayId);

    if (auto insert_result = displays.tryInsert(id, pActiveDisplay); !insert_result) {
        LOG(ERROR) << "Display ID " << id << " has been used by another caller.";
        pActiveDisplay->forceShutdown();
        return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::RESOURCE_BUSY));
    }

    LOG(DEBUG) << "Returning new EvsGlDisplay object " << pActiveDisplay.get();
    *displayObj = pActiveDisplay;
    return ScopedAStatus::ok();
}

ScopedAStatus EvsEnumerator::closeDisplay(const std::shared_ptr<IEvsDisplay>& obj) {
    LOG(DEBUG) << __FUNCTION__;

    auto& displays = mutableActiveDisplays();
    const auto display_search = displays.popDisplay(obj);

    if (!display_search) {
        LOG(WARNING) << "Ignoring close of previously orphaned display - why did a client steal?";
        return ScopedAStatus::ok();
    }

    auto pActiveDisplay = display_search->displayWeak.lock();

    if (!pActiveDisplay) {
        LOG(ERROR) << "Somehow a display is being destroyed "
                   << "when the enumerator didn't know one existed";
        return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::OWNERSHIP_LOST));
    }

    pActiveDisplay->forceShutdown();
    return ScopedAStatus::ok();
}

ScopedAStatus EvsEnumerator::getDisplayState(DisplayState* state) {
    LOG(DEBUG) << __FUNCTION__;
    return getDisplayStateImpl(std::nullopt, state);
}

ScopedAStatus EvsEnumerator::getDisplayStateById(int32_t displayId, DisplayState* state) {
    LOG(DEBUG) << __FUNCTION__;
    return getDisplayStateImpl(displayId, state);
}

ScopedAStatus EvsEnumerator::getDisplayStateImpl(std::optional<int32_t> displayId,
                                                 DisplayState* state) {
    if (!checkPermission()) {
        *state = DisplayState::DEAD;
        return ScopedAStatus::fromServiceSpecificError(
                static_cast<int>(EvsResult::PERMISSION_DENIED));
    }

    const auto& all_displays = mutableActiveDisplays().getAllDisplays();

    const auto display_search = displayId ? all_displays.find(*displayId) : all_displays.begin();

    if (display_search == all_displays.end()) {
        *state = DisplayState::NOT_OPEN;
        return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::OWNERSHIP_LOST));
    }

    std::shared_ptr<IEvsDisplay> pActiveDisplay = display_search->second.displayWeak.lock();
    if (pActiveDisplay) {
        return pActiveDisplay->getDisplayState(state);
    } else {
        *state = DisplayState::NOT_OPEN;
        return ScopedAStatus::fromServiceSpecificError(static_cast<int>(EvsResult::OWNERSHIP_LOST));
    }
}

ScopedAStatus EvsEnumerator::getDisplayIdList(std::vector<uint8_t>* list) {
    std::vector<uint8_t>& output = *list;
    if (sDisplayPortList.size() > 0) {
        output.resize(sDisplayPortList.size());
        unsigned i = 0;
        output[i++] = mInternalDisplayId & 0xFF;
        for (const auto& [port, id] : sDisplayPortList) {
            if (mInternalDisplayId != id) {
                output[i++] = port;
            }
        }
    }

    return ScopedAStatus::ok();
}

ScopedAStatus EvsEnumerator::isHardware(bool* flag) {
    *flag = true;
    return ScopedAStatus::ok();
}

void EvsEnumerator::notifyDeviceStatusChange(const std::string_view& deviceName,
                                             DeviceStatusType type) {
    std::lock_guard lock(sLock);
    if (!mCallback) {
        return;
    }

    std::vector<DeviceStatus> status{{.id = std::string(deviceName), .status = type}};
    if (!mCallback->deviceStatusChanged(status).isOk()) {
        LOG(WARNING) << "Failed to notify a device status change, name = " << deviceName
                     << ", type = " << static_cast<int>(type);
    }
}

ScopedAStatus EvsEnumerator::registerStatusCallback(
        const std::shared_ptr<IEvsEnumeratorStatusCallback>& callback) {
    std::lock_guard lock(sLock);
    if (mCallback) {
        LOG(INFO) << "Replacing an existing device status callback";
    }
    mCallback = callback;
    return ScopedAStatus::ok();
}

void EvsEnumerator::closeCamera_impl(const std::shared_ptr<IEvsCamera>& pCamera,
                                     const std::string& cameraId) {
    // Find the named camera
    CameraRecord* pRecord = findCameraById(cameraId);

    // Is the display being destroyed actually the one we think is active?
    if (!pRecord) {
        LOG(ERROR) << "Asked to close a camera whose name isn't recognized";
    } else {
        std::shared_ptr<EvsV4lCamera> pActiveCamera = pRecord->activeInstance.lock();
        if (!pActiveCamera) {
            LOG(WARNING) << "Somehow a camera is being destroyed "
                         << "when the enumerator didn't know one existed";
        } else if (pActiveCamera != pCamera) {
            // This can happen if the camera was aggressively reopened,
            // orphaning this previous instance
            LOG(WARNING) << "Ignoring close of previously orphaned camera "
                         << "- why did a client steal?";
        } else {
            // Shutdown the active camera
            pActiveCamera->shutdown();
        }
    }

    return;
}

bool EvsEnumerator::qualifyCaptureDevice(const char* deviceName) {
    class FileHandleWrapper {
    public:
        FileHandleWrapper(int fd) { mFd = fd; }
        ~FileHandleWrapper() {
            if (mFd > 0) close(mFd);
        }
        operator int() const { return mFd; }

    private:
        int mFd = -1;
    };

    FileHandleWrapper fd = open(deviceName, O_RDWR, 0);
    if (fd < 0) {
        return false;
    }

    v4l2_capability caps;
    int result = ioctl(fd, VIDIOC_QUERYCAP, &caps);
    if (result < 0) {
        return false;
    }
    if (((caps.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) == 0) ||
        ((caps.capabilities & V4L2_CAP_STREAMING) == 0)) {
        return false;
    }

    // Enumerate the available capture formats (if any)
    v4l2_fmtdesc formatDescription;
    formatDescription.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    bool found = false;
    for (int i = 0; !found; ++i) {
        formatDescription.index = i;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &formatDescription) == 0) {
            LOG(DEBUG) << "Format: 0x" << std::hex << formatDescription.pixelformat << " Type: 0x"
                       << std::hex << formatDescription.type
                       << " Desc: " << formatDescription.description << " Flags: 0x" << std::hex
                       << formatDescription.flags;
            switch (formatDescription.pixelformat) {
                case V4L2_PIX_FMT_YUYV:
                    found = true;
                    break;
                case V4L2_PIX_FMT_NV21:
                    found = true;
                    break;
                case V4L2_PIX_FMT_NV16:
                    found = true;
                    break;
                case V4L2_PIX_FMT_YVU420:
                    found = true;
                    break;
                case V4L2_PIX_FMT_RGB32:
                    found = true;
                    break;
#ifdef V4L2_PIX_FMT_ARGB32  // introduced with kernel v3.17
                case V4L2_PIX_FMT_ARGB32:
                    found = true;
                    break;
                case V4L2_PIX_FMT_XRGB32:
                    found = true;
                    break;
#endif  // V4L2_PIX_FMT_ARGB32
                default:
                    LOG(WARNING) << "Unsupported, " << std::hex << formatDescription.pixelformat;
                    break;
            }
        } else {
            // No more formats available.
            break;
        }
    }

    return found;
}

EvsEnumerator::CameraRecord* EvsEnumerator::findCameraById(const std::string& cameraId) {
    // Find the named camera
    for (auto &&cam : sCameraList) {
        if (strstr(cam.name.c_str(), cameraId.c_str()) ||
                (cam.desc.id == cameraId)) {
            // Found a match!
            return &cam;
        }
    }
    // We didn't find a match
    return nullptr;
}

std::optional<EvsEnumerator::ActiveDisplays::DisplayInfo> EvsEnumerator::ActiveDisplays::popDisplay(
        int32_t id) {
    std::lock_guard lck(mMutex);
    const auto search = mIdToDisplay.find(id);
    if (search == mIdToDisplay.end()) {
        return std::nullopt;
    }
    const auto display_info = search->second;
    mIdToDisplay.erase(search);
    mDisplayToId.erase(display_info.internalDisplayRawAddr);
    return display_info;
}

std::optional<EvsEnumerator::ActiveDisplays::DisplayInfo> EvsEnumerator::ActiveDisplays::popDisplay(
        std::shared_ptr<IEvsDisplay> display) {
    const auto display_ptr_val = reinterpret_cast<uintptr_t>(display.get());
    std::lock_guard lck(mMutex);
    const auto display_to_id_search = mDisplayToId.find(display_ptr_val);
    if (display_to_id_search == mDisplayToId.end()) {
        LOG(ERROR) << "Unknown display.";
        return std::nullopt;
    }
    const auto id = display_to_id_search->second;
    const auto id_to_display_search = mIdToDisplay.find(id);
    mDisplayToId.erase(display_to_id_search);
    if (id_to_display_search == mIdToDisplay.end()) {
        LOG(ERROR) << "No correspsonding ID for the display, probably orphaned.";
        return std::nullopt;
    }
    const auto display_info = id_to_display_search->second;
    mIdToDisplay.erase(id);
    return display_info;
}

std::unordered_map<int32_t, EvsEnumerator::ActiveDisplays::DisplayInfo>
EvsEnumerator::ActiveDisplays::getAllDisplays() {
    std::lock_guard lck(mMutex);
    const auto id_to_display_map_copy = mIdToDisplay;
    return id_to_display_map_copy;
}

bool EvsEnumerator::ActiveDisplays::tryInsert(int32_t id, std::shared_ptr<EvsGlDisplay> display) {
    std::lock_guard lck(mMutex);
    const auto display_ptr_val = reinterpret_cast<uintptr_t>(display.get());

    auto id_to_display_insert_result =
            mIdToDisplay.emplace(id,
                                 DisplayInfo{
                                         .id = id,
                                         .displayWeak = display,
                                         .internalDisplayRawAddr = display_ptr_val,
                                 });
    if (!id_to_display_insert_result.second) {
        return false;
    }
    auto display_to_id_insert_result = mDisplayToId.emplace(display_ptr_val, id);
    if (!display_to_id_insert_result.second) {
        mIdToDisplay.erase(id);
        return false;
    }
    return true;
}

ScopedAStatus EvsEnumerator::getUltrasonicsArrayList(
        [[maybe_unused]] std::vector<UltrasonicsArrayDesc>* list) {
    // TODO(b/149874793): Add implementation for EVS Manager and Sample driver
    return ScopedAStatus::ok();
}

ScopedAStatus EvsEnumerator::openUltrasonicsArray(
        [[maybe_unused]] const std::string& id,
        [[maybe_unused]] std::shared_ptr<IEvsUltrasonicsArray>* obj) {
    // TODO(b/149874793): Add implementation for EVS Manager and Sample driver
    return ScopedAStatus::ok();
}

ScopedAStatus EvsEnumerator::closeUltrasonicsArray(
        [[maybe_unused]] const std::shared_ptr<IEvsUltrasonicsArray>& obj) {
    // TODO(b/149874793): Add implementation for EVS Manager and Sample driver
    return ScopedAStatus::ok();
}

binder_status_t EvsEnumerator::dump(int fd, const char** args, uint32_t numArgs) {
    std::vector<std::string> options(args, args + numArgs);
    return parseCommand(fd, options);
}

binder_status_t EvsEnumerator::parseCommand(int fd, const std::vector<std::string>& options) {
    if (options.size() < 1) {
        WriteStringToFd("No option is given.\n", fd);
        cmdHelp(fd);
        return STATUS_BAD_VALUE;
    }

    const std::string command = options[0];
    if (EqualsIgnoreCase(command, "--help")) {
        cmdHelp(fd);
        return STATUS_OK;
    } else if (EqualsIgnoreCase(command, "--dump")) {
        return cmdDump(fd, options);
    } else {
        WriteStringToFd(StringPrintf("Invalid option: %s\n", command.data()), fd);
        return STATUS_INVALID_OPERATION;
    }
}

void EvsEnumerator::cmdHelp(int fd) {
    WriteStringToFd("--help: shows this help.\n"
                    "--dump [id] [start|stop] [directory]\n"
                    "\tDump camera frames to a target directory\n",
                    fd);
}

binder_status_t EvsEnumerator::cmdDump(int fd, const std::vector<std::string>& options) {
    if (options.size() < 3) {
        WriteStringToFd("Necessary argument is missing\n", fd);
        cmdHelp(fd);
        return STATUS_BAD_VALUE;
    }

    EvsEnumerator::CameraRecord* pRecord = findCameraById(options[1]);
    if (pRecord == nullptr) {
        WriteStringToFd(StringPrintf("%s is not active\n", options[1].data()), fd);
        return STATUS_BAD_VALUE;
    }

    auto device = pRecord->activeInstance.lock();
    if (device == nullptr) {
        WriteStringToFd(StringPrintf("%s seems dead\n", options[1].data()), fd);
        return STATUS_DEAD_OBJECT;
    }

    const std::string command = options[2];
    if (EqualsIgnoreCase(command, "start")) {
        // --dump [device id] start [path]
        if (options.size() < 4) {
            WriteStringToFd("Necessary argument is missing\n", fd);
            cmdHelp(fd);
            return STATUS_BAD_VALUE;
        }

        const std::string path = options[3];
        auto ret = device->startDumpFrames(path);
        if (!ret.ok()) {
            WriteStringToFd(StringPrintf("Failed to start storing frames: %s\n",
                                         ret.error().message().data()),
                            fd);
            return STATUS_FAILED_TRANSACTION;
        }
    } else if (EqualsIgnoreCase(command, "stop")) {
        // --dump [device id] stop
        auto ret = device->stopDumpFrames();
        if (!ret.ok()) {
            WriteStringToFd(StringPrintf("Failed to stop storing frames: %s\n",
                                         ret.error().message().data()),
                            fd);
            return STATUS_FAILED_TRANSACTION;
        }
    } else {
        WriteStringToFd(StringPrintf("Unknown command: %s", command.data()), fd);
        cmdHelp(fd);
    }

    return STATUS_OK;
}

}  // namespace aidl::android::hardware::automotive::evs::implementation
