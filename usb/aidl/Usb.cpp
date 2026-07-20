/*
 * Copyright (C) 2021 The Android Open Source Project
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

#define LOG_TAG "android.hardware.usb.aidl-service"

#include "Usb.h"

#include <aidl/android/hardware/usb/PortRole.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android_hardware_usb_flags.h>
#include <assert.h>
#include <cutils/uevent.h>
#include <dirent.h>
#include <hardware_legacy/power.h>
#include <libusbutils/CommonUtils.h>
#include <libusbutils/UsbPowerProfileMonitor.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <unistd.h>
#include <utils/Errors.h>
#include <utils/StrongPointer.h>

#include <chrono>
#include <regex>
#include <thread>
#include <unordered_map>


namespace usb_flags = android::hardware::usb::flags;

using aidl::android::hardware::usb::UsbPowerProfileMonitor;
using android::base::GetProperty;
using android::base::Trim;

namespace aidl {
namespace android {
namespace hardware {
namespace usb {

constexpr char kTypecPath[] = "/sys/class/typec/";
constexpr char kDataRoleNode[] = "/data_role";
constexpr char kPowerRoleNode[] = "/power_role";

static const char *udc_wakelock = "usb_wakelock";

// Set by the signal handler to destroy the thread
volatile bool destroyThread;

void queryVersionHelper(android::hardware::usb::Usb *usb,
                        std::vector<PortStatus> *currentPortStatus);

ScopedAStatus Usb::enableUsbData(const string &in_portName, bool in_enable,
                                 int64_t in_transactionId) {
    bool result = true;
    std::vector<PortStatus> currentPortStatus;
    string pullup;

    ALOGI("Userspace turn %s USB data signaling. opID:%ld", in_enable ? "on" : "off",
          in_transactionId);

    if (in_enable) {
        if (ReadFileToString(PULLUP_PATH, &pullup)) {
            pullup = Trim(pullup);
            if (pullup != GADGET_NAME) {
                if (!WriteStringToFile(GADGET_NAME, PULLUP_PATH)) {
                    ALOGE("Gadget cannot be pulled up");
                    result = false;
                }
            }
        }
    } else {
        if (ReadFileToString(PULLUP_PATH, &pullup)) {
            pullup = Trim(pullup);
            if (pullup == GADGET_NAME) {
                if (!WriteStringToFile("none", PULLUP_PATH)) {
                    ALOGE("Gadget cannot be pulled down");
                    result = false;
                }
            }
        }
    }

    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret =
                mCallback->notifyEnableUsbDataStatus(in_portName, in_enable,
                                                     result ? Status::SUCCESS : Status::ERROR,
                                                     in_transactionId);
        if (!ret.isOk())
            ALOGE("notifyEnableUsbDataStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);
    queryVersionHelper(this, &currentPortStatus);

    return ScopedAStatus::ok();
}

ScopedAStatus Usb::enableUsbDataWhileDocked(const string &in_portName, int64_t in_transactionId) {
    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret =
                mCallback->notifyEnableUsbDataWhileDockedStatus(in_portName, Status::NOT_SUPPORTED,
                                                                in_transactionId);
        if (!ret.isOk())
            ALOGE("notifyEnableUsbDataWhileDockedStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    return ScopedAStatus::ok();
}

ScopedAStatus Usb::resetUsbPort(const string &in_portName, int64_t in_transactionId) {
    bool result = true;
    std::vector<PortStatus> currentPortStatus;

    ALOGI("Userspace reset USB Port. opID:%ld", in_transactionId);

    if (!WriteStringToFile("none", PULLUP_PATH)) {
        ALOGI("Gadget cannot be pulled down");
        result = false;
    }

    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret =
                mCallback->notifyResetUsbPortStatus(in_portName,
                                                    result ? Status::SUCCESS : Status::ERROR,
                                                    in_transactionId);
        if (!ret.isOk())
            ALOGE("notifyResetUsbPortStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    return ScopedAStatus::ok();
}

Status queryMoistureDetectionStatus(std::vector<PortStatus> *currentPortStatus) {
    string enabled, status, path, DetectedPath;

    for (int i = 0; i < currentPortStatus->size(); i++) {
        (*currentPortStatus)[i].supportedContaminantProtectionModes.push_back(
                ContaminantProtectionMode::NONE);
        (*currentPortStatus)[i].contaminantProtectionStatus = ContaminantProtectionStatus::NONE;
        (*currentPortStatus)[i].contaminantDetectionStatus =
                ContaminantDetectionStatus::NOT_SUPPORTED;
        (*currentPortStatus)[i].supportsEnableContaminantPresenceDetection = false;
        (*currentPortStatus)[i].supportsEnableContaminantPresenceProtection = false;
    }

    return Status::SUCCESS;
}

Status queryNonCompliantChargerStatus(std::vector<PortStatus> *currentPortStatus) {
    string reasons, path;

    for (int i = 0; i < currentPortStatus->size(); i++) {
        (*currentPortStatus)[i].supportsComplianceWarnings = false;
    }
    return Status::SUCCESS;
}

string appendRoleNodeHelper(const string &portName, PortRole::Tag tag) {
    string node(kTypecPath + portName);

    switch (tag) {
        case PortRole::dataRole:
            return node + kDataRoleNode;
        case PortRole::powerRole:
            return node + kPowerRoleNode;
        case PortRole::mode:
            return node + "/port_type";
        default:
            return "";
    }
}

string convertRoletoString(PortRole role) {
    if (role.getTag() == PortRole::powerRole) {
        if (role.get<PortRole::powerRole>() == PortPowerRole::SOURCE)
            return "source";
        else if (role.get<PortRole::powerRole>() == PortPowerRole::SINK)
            return "sink";
    } else if (role.getTag() == PortRole::dataRole) {
        if (role.get<PortRole::dataRole>() == PortDataRole::HOST) return "host";
        if (role.get<PortRole::dataRole>() == PortDataRole::DEVICE) return "device";
    } else if (role.getTag() == PortRole::mode) {
        if (role.get<PortRole::mode>() == PortMode::UFP) return "sink";
        if (role.get<PortRole::mode>() == PortMode::DFP) return "source";
    }
    return "none";
}

void extractRole(string *roleName) {
    std::size_t first, last;

    first = roleName->find("[");
    last = roleName->find("]");

    if (first != string::npos && last != string::npos) {
        *roleName = roleName->substr(first + 1, last - first - 1);
    }
}

void switchToDrp(const string &portName) {
    string filename = appendRoleNodeHelper(string(portName.c_str()), PortRole::mode);
    FILE *fp;

    if (filename != "") {
        fp = fopen(filename.c_str(), "w");
        if (fp != NULL) {
            int ret = fputs("dual", fp);
            fclose(fp);
            if (ret == EOF) ALOGE("Fatal: Error while switching back to drp");
        } else {
            ALOGE("Fatal: Cannot open file to switch back to drp");
        }
    } else {
        ALOGE("Fatal: invalid node type");
    }
}

bool switchMode(const string &portName, const PortRole &in_role, struct Usb *usb) {
    string filename = appendRoleNodeHelper(string(portName.c_str()), in_role.getTag());
    string written;
    FILE *fp;
    bool roleSwitch = false;

    if (filename == "") {
        ALOGE("Fatal: invalid node type");
        return false;
    }

    fp = fopen(filename.c_str(), "w");
    if (fp != NULL) {
        // Hold the lock here to prevent loosing connected signals
        // as once the file is written the partner added signal
        // can arrive anytime.
        pthread_mutex_lock(&usb->mPartnerLock);
        usb->mPartnerUp = false;
        int ret = fputs(convertRoletoString(in_role).c_str(), fp);
        fclose(fp);

        if (ret != EOF) {
            struct timespec to;
            struct timespec now;

        wait_again:
            clock_gettime(CLOCK_MONOTONIC, &now);
            to.tv_sec = now.tv_sec + PORT_TYPE_TIMEOUT;
            to.tv_nsec = now.tv_nsec;

            int err = pthread_cond_timedwait(&usb->mPartnerCV, &usb->mPartnerLock, &to);
            // There are no uevent signals which implies role swap timed out.
            if (err == ETIMEDOUT) {
                ALOGI("uevents wait timedout");
                // Validity check.
            } else if (!usb->mPartnerUp) {
                goto wait_again;
                // Role switch succeeded since usb->mPartnerUp is true.
            } else {
                roleSwitch = true;
            }
        } else {
            ALOGI("Role switch failed while wrting to file");
        }
        pthread_mutex_unlock(&usb->mPartnerLock);
    }

    if (!roleSwitch) switchToDrp(string(portName.c_str()));

    return roleSwitch;
}

void updatePortStatus(android::hardware::usb::Usb* usb) {
    std::vector<PortStatus> currentPortStatus;

    queryVersionHelper(usb, &currentPortStatus);
}

Usb::Usb()
      : mLock(PTHREAD_MUTEX_INITIALIZER),
        mRoleSwitchLock(PTHREAD_MUTEX_INITIALIZER),
        mPartnerLock(PTHREAD_MUTEX_INITIALIZER),
        mPartnerUp(false),
        mPowerMonitor(new UsbPowerProfileMonitor(true, true)) {
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr)) {
        ALOGE("pthread_condattr_init failed: %s", strerror(errno));
        abort();
    }
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) {
        ALOGE("pthread_condattr_setclock failed: %s", strerror(errno));
        abort();
    }
    if (pthread_cond_init(&mPartnerCV, &attr)) {
        ALOGE("pthread_cond_init failed: %s", strerror(errno));
        abort();
    }
    if (pthread_condattr_destroy(&attr)) {
        ALOGE("pthread_condattr_destroy failed: %s", strerror(errno));
        abort();
    }
    mPoll = pthread_t();
}

ScopedAStatus Usb::switchRole(const string &in_portName, const PortRole &in_role,
                              int64_t in_transactionId) {
    string filename = appendRoleNodeHelper(string(in_portName.c_str()), in_role.getTag());
    string written;
    FILE *fp;
    bool roleSwitch = false;

    if (filename == "") {
        ALOGE("Fatal: invalid node type");
        return ScopedAStatus::ok();
    }

    pthread_mutex_lock(&mRoleSwitchLock);

    ALOGI("filename write: %s role:%s", filename.c_str(), convertRoletoString(in_role).c_str());

    if (in_role.getTag() == PortRole::mode) {
        roleSwitch = switchMode(in_portName, in_role, this);
    } else {
        fp = fopen(filename.c_str(), "w");
        if (fp != NULL) {
            int ret = fputs(convertRoletoString(in_role).c_str(), fp);
            fclose(fp);
            if ((ret != EOF) && ReadFileToString(filename, &written)) {
                written = Trim(written);
                extractRole(&written);
                ALOGI("written: %s", written.c_str());
                if (written == convertRoletoString(in_role)) {
                    roleSwitch = true;
                } else {
                    ALOGE("Role switch failed");
                }
            } else {
                ALOGE("failed to update the new role");
            }
        } else {
            ALOGE("fopen failed");
        }
    }

    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret = mCallback->notifyRoleSwitchStatus(
                in_portName, in_role, roleSwitch ? Status::SUCCESS : Status::ERROR,
                in_transactionId);
        if (!ret.isOk()) ALOGE("RoleSwitchStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);
    pthread_mutex_unlock(&mRoleSwitchLock);

    return ScopedAStatus::ok();
}

ScopedAStatus Usb::limitPowerTransfer(const string &in_portName, bool /*in_limit*/,
                                      int64_t in_transactionId) {
    std::vector<PortStatus> currentPortStatus;

    pthread_mutex_lock(&mLock);
    if (mCallback != NULL && in_transactionId >= 0) {
        ScopedAStatus ret =
                mCallback->notifyLimitPowerTransferStatus(in_portName, false, Status::NOT_SUPPORTED,
                                                          in_transactionId);
        if (!ret.isOk()) ALOGE("limitPowerTransfer error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    return ScopedAStatus::ok();
}

Status getAccessoryConnected(const string &portName, string *accessory) {
    string filename = kTypecPath + portName + "-partner/accessory_mode";

    if (!ReadFileToString(filename, accessory)) {
        ALOGE("getAccessoryConnected: Failed to open filesystem node: %s", filename.c_str());
        return Status::ERROR;
    }
    *accessory = Trim(*accessory);

    return Status::SUCCESS;
}

Status getCurrentRoleHelper(const string &portName, bool connected, PortRole *currentRole) {
    string filename;
    string roleName;
    string accessory;

    // Mode

    if (currentRole->getTag() == PortRole::powerRole) {
        filename = kTypecPath + portName + kPowerRoleNode;
        currentRole->set<PortRole::powerRole>(PortPowerRole::NONE);
    } else if (currentRole->getTag() == PortRole::dataRole) {
        filename = kTypecPath + portName + kDataRoleNode;
        currentRole->set<PortRole::dataRole>(PortDataRole::NONE);
    } else if (currentRole->getTag() == PortRole::mode) {
        filename = kTypecPath + portName + kDataRoleNode;
        currentRole->set<PortRole::mode>(PortMode::NONE);
    } else {
        return Status::ERROR;
    }

    if (!connected) return Status::SUCCESS;

    if (currentRole->getTag() == PortRole::mode) {
        if (getAccessoryConnected(portName, &accessory) != Status::SUCCESS) {
            return Status::ERROR;
        }
        if (accessory == "analog_audio") {
            currentRole->set<PortRole::mode>(PortMode::AUDIO_ACCESSORY);
            return Status::SUCCESS;
        } else if (accessory == "debug") {
            currentRole->set<PortRole::mode>(PortMode::DEBUG_ACCESSORY);
            return Status::SUCCESS;
        }
    }

    if (!ReadFileToString(filename, &roleName)) {
        ALOGE("getCurrentRole: Failed to open filesystem node: %s", filename.c_str());
        return Status::ERROR;
    }

    roleName = Trim(roleName);
    extractRole(&roleName);

    if (roleName == "source") {
        currentRole->set<PortRole::powerRole>(PortPowerRole::SOURCE);
    } else if (roleName == "sink") {
        currentRole->set<PortRole::powerRole>(PortPowerRole::SINK);
    } else if (roleName == "host") {
        if (currentRole->getTag() == PortRole::dataRole)
            currentRole->set<PortRole::dataRole>(PortDataRole::HOST);
        else
            currentRole->set<PortRole::mode>(PortMode::DFP);
    } else if (roleName == "device") {
        if (currentRole->getTag() == PortRole::dataRole)
            currentRole->set<PortRole::dataRole>(PortDataRole::DEVICE);
        else
            currentRole->set<PortRole::mode>(PortMode::UFP);
    } else if (roleName != "none") {
        /* case for none has already been addressed.
         * so we check if the role isn't none.
         */
        return Status::UNRECOGNIZED_ROLE;
    }

    return Status::SUCCESS;
}

Status getTypeCPortNamesHelper(std::unordered_map<string, bool> *names) {
    DIR *dp;

    dp = opendir(kTypecPath);
    if (dp != NULL) {
        struct dirent *ep;

        while ((ep = readdir(dp))) {
            if (ep->d_type == DT_LNK) {
                if (usb_flags::enable_usb_capabilities_reporting()) {
                    string entryName(ep->d_name);
                    size_t partnerPos = entryName.find("-partner");
                    if (partnerPos == string::npos) {
                        // not a partner entry, could be "port0", "port0-cable", "port0-plug", etc.
                        size_t hyphenPos = entryName.find("-");
                        string portName = (hyphenPos == string::npos)
                                                  ? entryName
                                                  : entryName.substr(0, hyphenPos);
                        names->emplace(portName, false);
                    } else {
                        string portName = entryName.substr(0, partnerPos);
                        (*names)[portName] = true;
                    }
                } else {
                    if (string::npos == string(ep->d_name).find("-partner")) {
                        std::unordered_map<string, bool>::const_iterator portName =
                                names->find(ep->d_name);
                        if (portName == names->end()) {
                            names->insert({ep->d_name, false});
                        }
                    } else {
                        char* saveptr;
                        (*names)[strtok_r(ep->d_name, "-", &saveptr)] = true;
                    }
                }
            }
        }
        closedir(dp);
        return Status::SUCCESS;
    }

    ALOGE("Failed to open /sys/class/typec or there is no typec port");
    return Status::ERROR;
}

bool canSwitchRoleHelper(const string &portName) {
    string filename = kTypecPath + portName + "-partner/supports_usb_power_delivery";
    string supportsPD;

    if (ReadFileToString(filename, &supportsPD)) {
        supportsPD = Trim(supportsPD);
        if (supportsPD == "yes") {
            return true;
        }
    }

    return false;
}

Status getPortStatusHelper(std::vector<PortStatus> *currentPortStatus) {
    std::unordered_map<string, bool> names;
    Status result = getTypeCPortNamesHelper(&names);
    int i = -1;

    if (result == Status::SUCCESS) {
        currentPortStatus->resize(names.size());
        for (std::pair<string, bool> port : names) {
            i++;
            ALOGI("%s", port.first.c_str());
            (*currentPortStatus)[i].portName = port.first;

            PortRole currentRole;
            currentRole.set<PortRole::powerRole>(PortPowerRole::NONE);
            if (getCurrentRoleHelper(port.first, port.second, &currentRole) == Status::SUCCESS) {
                (*currentPortStatus)[i].currentPowerRole = currentRole.get<PortRole::powerRole>();
            } else {
                ALOGE("Error while retrieving portNames");
                goto done;
            }

            currentRole.set<PortRole::dataRole>(PortDataRole::NONE);
            if (getCurrentRoleHelper(port.first, port.second, &currentRole) == Status::SUCCESS) {
                (*currentPortStatus)[i].currentDataRole = currentRole.get<PortRole::dataRole>();
            } else {
                ALOGE("Error while retrieving current port role");
                goto done;
            }

            currentRole.set<PortRole::mode>(PortMode::NONE);
            if (getCurrentRoleHelper(port.first, port.second, &currentRole) == Status::SUCCESS) {
                (*currentPortStatus)[i].currentMode = currentRole.get<PortRole::mode>();
            } else {
                ALOGE("Error while retrieving current data role");
                goto done;
            }

            (*currentPortStatus)[i].canChangeMode = true;
            (*currentPortStatus)[i].canChangeDataRole =
                port.second ? canSwitchRoleHelper(port.first) : false;
            (*currentPortStatus)[i].canChangePowerRole =
                port.second ? canSwitchRoleHelper(port.first) : false;

            (*currentPortStatus)[i].supportedModes.push_back(PortMode::DRP);
            (*currentPortStatus)[i].usbDataStatus.push_back(UsbDataStatus::ENABLED);

            /**
             * C++ optional object contains a value when the object is initialized or assigned
             * a value of the optional type. PortPartnerStatus does not have a constructor, so we
             * initialize a local object then assign to set the optional object value().
             */
            PortPartnerStatus partnerStatus;
            (*currentPortStatus)[i].partnerStatus = partnerStatus;

            ALOGI("%d:%s connected:%d canChangeMode:%d canChagedata:%d canChangePower:%d "
                  "usbDataEnabled:%d plugOrientation:%d",
                  i, port.first.c_str(), port.second, (*currentPortStatus)[i].canChangeMode,
                  (*currentPortStatus)[i].canChangeDataRole,
                  (*currentPortStatus)[i].canChangePowerRole, 0,
                  (*currentPortStatus)[i].plugOrientation);
        }

        return Status::SUCCESS;
    } else {
        // For legacy device has no type-c implementation
        ALOGE("Legacy device has no type-c implementation\n");
        currentPortStatus->resize(1);
        (*currentPortStatus)[0].portName = "port0";
        (*currentPortStatus)[0].currentPowerRole = PortPowerRole::SINK;
        (*currentPortStatus)[0].currentDataRole = PortDataRole::DEVICE;
        (*currentPortStatus)[0].currentMode = PortMode::UFP;
        (*currentPortStatus)[0].canChangeMode = false;
        (*currentPortStatus)[0].canChangeDataRole = false;
        (*currentPortStatus)[0].canChangePowerRole = false;
        (*currentPortStatus)[0].supportedModes.push_back(PortMode::DRP);
        (*currentPortStatus)[0].usbDataStatus.push_back(UsbDataStatus::UNKNOWN);

        ALOGI("Legacy: canChangeMode:%d canChagedata:%d canChangePower:%d",
              (*currentPortStatus)[0].canChangeMode, (*currentPortStatus)[0].canChangeDataRole,
              (*currentPortStatus)[0].canChangePowerRole);

        return Status::SUCCESS;
    }
done:
    return Status::ERROR;
}

void queryVersionHelper(android::hardware::usb::Usb *usb,
                        std::vector<PortStatus> *currentPortStatus) {
    Status status;
    pthread_mutex_lock(&usb->mLock);
    status = getPortStatusHelper(currentPortStatus);
    queryMoistureDetectionStatus(currentPortStatus);
    queryNonCompliantChargerStatus(currentPortStatus);
    usb->mPowerMonitor->queryPowerProfileStatus(currentPortStatus);
    if (usb->mCallback != NULL) {
        ScopedAStatus ret = usb->mCallback->notifyPortStatusChange(*currentPortStatus, status);
        if (!ret.isOk()) ALOGE("queryPortStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGI("Notifying userspace skipped. Callback is NULL");
    }
    pthread_mutex_unlock(&usb->mLock);
}

ScopedAStatus Usb::queryPortStatus(int64_t in_transactionId) {
    std::vector<PortStatus> currentPortStatus;

    queryVersionHelper(this, &currentPortStatus);
    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret =
                mCallback->notifyQueryPortStatus("all", Status::SUCCESS, in_transactionId);
        if (!ret.isOk()) ALOGE("notifyQueryPortStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    return ScopedAStatus::ok();
}

ScopedAStatus Usb::enableContaminantPresenceDetection(const string &in_portName, bool /*in_enable*/,
                                                      int64_t in_transactionId) {
    std::vector<PortStatus> currentPortStatus;

    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret = mCallback->notifyContaminantEnabledStatus(
                in_portName, false, Status::ERROR, in_transactionId);
        if (!ret.isOk())
            ALOGE("enableContaminantPresenceDetection  error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    queryVersionHelper(this, &currentPortStatus);
    return ScopedAStatus::ok();
}

ScopedAStatus Usb::queryStaticPortInformation(const string& in_portName, int64_t in_transactionId) {
    shared_ptr<IUsbCallback> callback;

    pthread_mutex_lock(&mLock);
    callback = mCallback;
    pthread_mutex_unlock(&mLock);

    if (callback == nullptr) {
        ALOGE("Not notifying the userspace. Callback is not set");
        return ScopedAStatus::ok();
    }

    StaticPortInformation staticPortInformation;
    staticPortInformation.portName = in_portName;
    staticPortInformation.sysfsPath = kTypecPath + in_portName;
    Status status = Status::ERROR;

    if (!usb_flags::enable_usb_capabilities_reporting()) {
        status = Status::NOT_SUPPORTED;
    } else {
        std::unordered_map<string, bool> portNamesConnectedMap;
        getTypeCPortNamesHelper(&portNamesConnectedMap);
        std::unordered_map<string, bool>::const_iterator matchingPort =
                portNamesConnectedMap.find(in_portName);

        if (matchingPort != portNamesConnectedMap.end()) {
            staticPortInformation.connectorType =
                    ConnectorType::C;  // This HAL implementation only records Type-C ports.
            staticPortInformation.capabilities = {
                    Capability::HOST_MODE};  // GMS-VSR-5.4-001 (Android 16) Devices MUST support
                                             // USB host mode.

            // The AIDL struct defaults powerRolesSupported and dataRolesSupported to [NONE].
            // The kernel shows supported roles if DRD (Dual Role Data) or DRP (Dual Role Power) or
            // fixed roles in drivers/usb/typec/class.c in the functions power_role_show and
            // data_role_show.
            string dataRolePath = appendRoleNodeHelper(in_portName, PortRole::dataRole);
            string powerRolePath = appendRoleNodeHelper(in_portName, PortRole::powerRole);
            string dataRoleStr, powerRoleStr;

            if (ReadFileToString(dataRolePath, &dataRoleStr)) {
                if (dataRoleStr.find("host") != string::npos) {
                    staticPortInformation.dataRolesSupported.push_back(PortDataRole::HOST);
                }
                if (dataRoleStr.find("device") != string::npos) {
                    staticPortInformation.dataRolesSupported.push_back(PortDataRole::DEVICE);
                }
            } else {
                ALOGE("Error reading data role path: %s", dataRolePath.c_str());
            }

            if (ReadFileToString(powerRolePath, &powerRoleStr)) {
                if (powerRoleStr.find("source") != string::npos) {
                    staticPortInformation.powerRolesSupported.push_back(PortPowerRole::SOURCE);
                }
                if (powerRoleStr.find("sink") != string::npos) {
                    staticPortInformation.powerRolesSupported.push_back(PortPowerRole::SINK);
                }
            } else {
                ALOGE("Error reading power role path: %s", powerRolePath.c_str());
            }
            status = Status::SUCCESS;
        } else {
            ALOGE("portName not found: %s", in_portName.c_str());
        }
    }

    ScopedAStatus ret = callback->notifyQueryStaticPortInformation(
            in_portName, staticPortInformation, status, in_transactionId);
    if (!ret.isOk())
        ALOGE("queryStaticPortInformation for queryStaticPortInfo error %s",
              ret.getDescription().c_str());
    return ScopedAStatus::ok();
}

struct data {
    int uevent_fd;
    ::aidl::android::hardware::usb::Usb *usb;
};

static void uevent_event(uint32_t /*epevents*/, struct data *payload) {
    char msg[UEVENT_MSG_LEN + 2];
    char *cp;
    int n;

    std::string udc_device = ::android::base::GetProperty(USB_CONTROLLER, "");

    n = uevent_kernel_multicast_recv(payload->uevent_fd, msg, UEVENT_MSG_LEN);
    if (n <= 0) return;
    if (n >= UEVENT_MSG_LEN) /* overflow -- discard */
        return;

    msg[n] = '\0';
    msg[n + 1] = '\0';
    cp = msg;

    while (*cp) {
        if (std::regex_match(cp, std::regex("(add)(.*)(-partner)"))) {
            ALOGI("partner added");
            pthread_mutex_lock(&payload->usb->mPartnerLock);
            payload->usb->mPartnerUp = true;
            pthread_cond_signal(&payload->usb->mPartnerCV);
            pthread_mutex_unlock(&payload->usb->mPartnerLock);
        } else if (!strncmp(cp, "USB_STATE=CONFIGURED", strlen("USB_STATE=CONFIGURED"))) {
            if (!access(("/sys/class/udc/" + udc_device).c_str(), F_OK))
                acquire_wake_lock(PARTIAL_WAKE_LOCK, udc_wakelock);
        } else if (!strncmp(cp, "USB_STATE=DISCONNECTED", strlen("USB_STATE=DISCONNECTED"))) {
            release_wake_lock(udc_wakelock);
        } else if (!strncmp(cp, "DEVTYPE=typec_", strlen("DEVTYPE=typec_"))) {
            std::vector<PortStatus> currentPortStatus;
            queryVersionHelper(payload->usb, &currentPortStatus);

            // Role switch is not in progress and port is in disconnected state
            if (!pthread_mutex_trylock(&payload->usb->mRoleSwitchLock)) {
                for (unsigned long i = 0; i < currentPortStatus.size(); i++) {
                    DIR *dp = opendir(string(kTypecPath +
                                             string(currentPortStatus[i].portName.c_str()) +
                                             "-partner")
                                              .c_str());
                    if (dp == NULL) {
                        switchToDrp(currentPortStatus[i].portName);
                    } else {
                        closedir(dp);
                    }
                }
                pthread_mutex_unlock(&payload->usb->mRoleSwitchLock);
            }
            break;
        } else if (std::regex_match(cp, std::regex("usb_power_delivery/(pd[0-9]+)"))) {
            if (usb_flags::enable_power_profile_reporting()) {
                armTimerFd(payload->usb->mPowerMonitor->mTimerDebounceFd,
                           POWER_MONITOR_DEBOUNCE_MS);
            }
        } else if (std::regex_search(cp, std::regex("power_supply/usb"))) {
            if (usb_flags::enable_power_profile_reporting()) {
                if (!strncmp(cp, "change@", strlen("change@"))) {
                    armTimerFd(payload->usb->mPowerMonitor->mTimerDebounceFd,
                               POWER_MONITOR_DEBOUNCE_MS);
                }
            }
        } /* advance to after the next \0 */
        while (*cp++) {
        }
    }
}

void *work(void *param) {
    int epoll_fd = 0, uevent_fd = 0;
    struct epoll_event ev;
    int nevents = 0;
    struct data payload;
    int ret;
    unsigned long res;
    ::aidl::android::hardware::usb::Usb* usb = (::aidl::android::hardware::usb::Usb*)param;

    ALOGE("creating thread");
    std::string buffer;
    std::string usb_state = GetProperty(USB_CONTROLLER, "");
    if (usb_state != "") {
        char udc_state[UDC_STATE_VALUE_MAX];
        sprintf(udc_state, "/sys/class/udc/%s/state", usb_state.c_str());

        if (!::android::base::ReadFileToString(std::string(udc_state), &buffer)) {
            ALOGE("can't read udc_state");
        }
    }

    // the usb hal is not started when usb driver probe, the uevent send in usb probe stage is not
    // handled in time. if device is configured when start usb hal, hold one wakelock.
    if (!strncmp(buffer.c_str(), UDC_CONFIGURED, strlen(UDC_CONFIGURED))) {
        acquire_wake_lock(PARTIAL_WAKE_LOCK, udc_wakelock);
    }

    uevent_fd = uevent_open_socket(UEVENT_MAX_EVENTS * UEVENT_MSG_LEN, true);

    if (uevent_fd < 0) {
        ALOGE("uevent_init: uevent_open_socket failed\n");
        return NULL;
    }

    payload.uevent_fd = uevent_fd;
    payload.usb = usb;

    if (fcntl(uevent_fd, F_SETFL, O_NONBLOCK)) {
        ALOGE("fcntl fail to set uevent_fd as nonblock mode");
        goto error;
    }

    ev.events = EPOLLIN;
    ev.data.fd = uevent_fd;
    ev.data.ptr = (void *)uevent_event;

    epoll_fd = epoll_create(UEVENT_MAX_EVENTS + EPOLL_MAX_EVENTS);
    if (epoll_fd == -1) {
        ALOGE("epoll_create failed; errno=%d", errno);
        goto error;
    }

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, uevent_fd, &ev) == -1) {
        ALOGE("epoll_ctl failed; errno=%d", errno);
        goto error;
    }

    if (usb_flags::enable_power_profile_reporting()) {
        usb->mPowerMonitor->setupEpoll(epoll_fd);
    }

    while (!destroyThread) {
        struct epoll_event events[UEVENT_MAX_EVENTS + EPOLL_MAX_EVENTS];

        nevents = epoll_wait(epoll_fd, events, UEVENT_MAX_EVENTS, -1);
        if (nevents == -1) {
            if (errno == EINTR) continue;
            ALOGE("usb epoll_wait failed; errno=%d", errno);
            break;
        }

        for (int n = 0; n < nevents; ++n) {
            if (events[n].data.fd == usb->mPowerMonitor->mTimerDebounceFd.get()) {
                ret = read(usb->mPowerMonitor->mTimerDebounceFd.get(), &res, sizeof(res));
                if (ret < 0) {
                    ALOGW("mPowerMonitor->mTimerDebounceFd debounce read errno %d", errno);
                } else {
                    ALOGI("mPowerMonitor->mTimerDebounceFd debounce triggered");
                    updatePortStatus(usb);
                }
            } else if (usb->mPowerMonitor->isPowerProfileMonitorFd(events[n].data.fd)) {
                armTimerFd(usb->mPowerMonitor->mTimerDebounceFd, POWER_MONITOR_DEBOUNCE_MS);
                continue;
            } else if (events[n].data.ptr) {
                (*(void (*)(int, struct data* payload))events[n].data.ptr)(events[n].events,
                                                                           &payload);
            }
        }
    }

    ALOGI("exiting worker thread");
error:
    close(uevent_fd);

    if (epoll_fd >= 0) close(epoll_fd);

    return NULL;
}

void sighandler(int sig) {
    if (sig == SIGUSR1) {
        destroyThread = true;
        ALOGI("destroy set");
        return;
    }
    signal(SIGUSR1, sighandler);
}

ScopedAStatus Usb::setCallback(const shared_ptr<IUsbCallback> &in_callback) {
    pthread_mutex_lock(&mLock);
    if ((mCallback == NULL && in_callback == NULL) || (mCallback != NULL && in_callback != NULL)) {
        mCallback = in_callback;
        pthread_mutex_unlock(&mLock);
        return ScopedAStatus::ok();
    }

    mCallback = in_callback;
    ALOGI("registering callback");

    if (mCallback == NULL) {
        if (!pthread_kill(mPoll, SIGUSR1)) {
            pthread_join(mPoll, NULL);
            ALOGI("pthread destroyed");
        }
        pthread_mutex_unlock(&mLock);
        return ScopedAStatus::ok();
    }

    destroyThread = false;
    signal(SIGUSR1, sighandler);

    /*
     * Create a background thread if the old callback value is NULL
     * and being updated with a new value.
     */
    if (pthread_create(&mPoll, NULL, work, this)) {
        ALOGE("pthread creation failed %d", errno);
        mCallback = NULL;
    }

    pthread_mutex_unlock(&mLock);
    return ScopedAStatus::ok();
}

} // namespace usb
} // namespace hardware
} // namespace android
} // namespace aidl
