#include <aidl/android/hardware/usb/gadget/GadgetFunction.h>
#include <aidl/android/hardware/usb/gadget/IUsbGadget.h>
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/hardware/usb/gadget/1.0/IUsbGadget.h>
#include <gtest/gtest.h>
#include <stdio.h>
#include <unistd.h>

using android::hardware::Return;

using aidl::android::hardware::usb::gadget::GadgetFunction;
using ndk::ScopedAStatus;
using std::shared_ptr;

std::atomic<int> sUsbOperationCount{};

void UsbGtest() {
    LOG(INFO) << "Enter the UsbModeTest Gtest!!!";
    int operationId = sUsbOperationCount++;
    uint8_t sleep_delay = 3;
    uint32_t iteration_rounds = 2500;
    uint32_t i = 0;

    const std::string service_name =
            std::string(aidl::android::hardware::usb::gadget::IUsbGadget::descriptor)
                    .append("/default");

    if (AServiceManager_isDeclared(service_name.c_str())) {
        shared_ptr<aidl::android::hardware::usb::gadget::IUsbGadget> gadget_aidl =
                aidl::android::hardware::usb::gadget::IUsbGadget::fromBinder(
                        ndk::SpAIBinder(AServiceManager_waitForService(service_name.c_str())));
        ScopedAStatus ret;
        if (gadget_aidl != nullptr) {
            LOG(INFO) << "Usb AIDL HAL found.";

            i = 0;
            while (i < iteration_rounds) {
                ret = gadget_aidl->setCurrentUsbFunctions(GadgetFunction::MTP | GadgetFunction::ADB,
                                                          nullptr, 0, operationId);
                if (!ret.isOk())
                    LOG(ERROR) << "Failed to switch to MTP";
                sleep(sleep_delay);

                ret = gadget_aidl->setCurrentUsbFunctions(GadgetFunction::RNDIS |
                                                                  GadgetFunction::ADB,
                                                          nullptr, 0, operationId);
                if (!ret.isOk())
                    LOG(ERROR) << "Failed to switch to RNDIS";
                sleep(sleep_delay);

                ret = gadget_aidl->setCurrentUsbFunctions(GadgetFunction::MIDI |
                                                                  GadgetFunction::ADB,
                                                          nullptr, 0, operationId);
                if (!ret.isOk())
                    LOG(ERROR) << "Failed to switch to MIDI";
                sleep(sleep_delay);

                ret = gadget_aidl->setCurrentUsbFunctions(GadgetFunction::PTP | GadgetFunction::ADB,
                                                          nullptr, 0, operationId);
                if (!ret.isOk())
                    LOG(ERROR) << "Failed to switch to PTP";
                sleep(sleep_delay);

                ret = gadget_aidl->setCurrentUsbFunctions(GadgetFunction::ADB, nullptr, 0,
                                                          operationId);
                if (!ret.isOk())
                    LOG(ERROR) << "Failed to switch to ADB";
                sleep(sleep_delay);

                i = i + 1;
            }

        } else {
            LOG(INFO) << "Usb AIDL HAL not found";
        }
    } else {
        android::sp<android::hardware::usb::gadget::V1_0::IUsbGadget> gadget =
                android::hardware::usb::gadget::V1_0::IUsbGadget::getService();
        Return<void> ret;

        if (gadget != nullptr) {
            LOG(INFO) << "Usb HIDL HAL found.";

            i = 0;
            while (i < iteration_rounds) {
                ret = gadget->setCurrentUsbFunctions(static_cast<uint64_t>(GadgetFunction::MTP),
                                                     nullptr, 0);
                if (!ret.isOk()) {
                    LOG(INFO) << "Failed to switch to MTP: " << ret.description().c_str();
                }
                sleep(sleep_delay);
                ret = gadget->setCurrentUsbFunctions(static_cast<uint64_t>(GadgetFunction::RNDIS),
                                                     nullptr, 0);
                if (!ret.isOk()) {
                    LOG(INFO) << "Failed to switch to RNDIS: " << ret.description().c_str();
                }
                sleep(sleep_delay);
                ret = gadget->setCurrentUsbFunctions(static_cast<uint64_t>(GadgetFunction::MIDI),
                                                     nullptr, 0);
                if (!ret.isOk()) {
                    LOG(INFO) << "Failed to switch to MIDI: " << ret.description().c_str();
                }
                sleep(sleep_delay);
                ret = gadget->setCurrentUsbFunctions(static_cast<uint64_t>(GadgetFunction::PTP),
                                                     nullptr, 0);
                if (!ret.isOk()) {
                    LOG(INFO) << "Failed to switch to PTP: " << ret.description().c_str();
                }
                sleep(sleep_delay);
                ret = gadget->setCurrentUsbFunctions(static_cast<uint64_t>(GadgetFunction::ADB),
                                                     nullptr, 0);
                if (!ret.isOk()) {
                    LOG(INFO) << "Failed to switch to ADB: " << ret.description().c_str();
                }
                sleep(sleep_delay);
                i = i + 1;
            }

        } else {
            LOG(INFO) << "Usb HIDL HAL not found";
        }
    }
}

TEST(UsbModeTest, SetUsbMode) {
    UsbGtest();
}
