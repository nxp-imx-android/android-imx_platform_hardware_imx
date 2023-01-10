/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "ConfigManager.h"
#include "EvsStateControl.h"
#include "EvsVehicleListener.h"

#include <aidl/android/hardware/automotive/vehicle/SubscribeOptions.h>
#include <aidl/android/hardware/automotive/vehicle/VehicleGear.h>
#include <aidl/android/hardware/automotive/vehicle/VehicleProperty.h>
#include <android-base/logging.h>
#include <android-base/macros.h>    // arraysize
#include <android/hardware/automotive/evs/1.1/IEvsDisplay.h>
#include <android/hardware/automotive/evs/1.1/IEvsEnumerator.h>
#include <hidl/HidlTransportSupport.h>
#include <hwbinder/IPCThreadState.h>
#include <hwbinder/ProcessState.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/StrongPointer.h>
#include <cutils/properties.h>

#include <IVhalClient.h>
#include <signal.h>
#include <stdio.h>

namespace {

using ::aidl::android::hardware::automotive::vehicle::VehicleGear;
using ::aidl::android::hardware::automotive::vehicle::VehicleProperty;
// libhidl:
using ::android::frameworks::automotive::vhal::ISubscriptionClient;
using ::android::frameworks::automotive::vhal::IVhalClient;
using android::hardware::configureRpcThreadpool;
using android::hardware::joinRpcThreadpool;

android::sp<IEvsEnumerator> pEvs;
android::sp<IEvsDisplay> pDisplay;
EvsStateControl *pStateController;

void sigHandler(int sig) {
    LOG(WARNING) << "evs_app is being terminated on receiving a signal " << sig;
    if (pEvs != nullptr) {
        // Attempt to clean up the resources
        pStateController->postCommand({EvsStateControl::Op::EXIT, 0, 0}, true);
        pStateController->terminateUpdateLoop();
        pEvs->closeDisplay(pDisplay);
    }

    android::hardware::IPCThreadState::self()->stopProcess();
    exit(EXIT_FAILURE);
}

void registerSigHandler() {
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = sigHandler;
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT,  &sa, nullptr);
}

} // namespace

// Helper to subscribe to VHal notifications
static bool subscribeToVHal(ISubscriptionClient* client, VehicleProperty propertyId) {
    assert(pVnet != nullptr);
    assert(listener != nullptr);

    // Register for vehicle state change callbacks we care about
    // Changes in these values are what will trigger a reconfiguration of the EVS pipeline
    std::vector<aidl::android::hardware::automotive::vehicle::SubscribeOptions> options = {
        {
            .propId = static_cast<int32_t>(propertyId),
            .areaIds = {},
        },
    };
    if (auto result = client->subscribe(options); !result.ok()) {
        LOG(WARNING) << "VHAL subscription for property " << static_cast<int32_t>(propertyId)
                     << " failed with error " << result.error().message();
        return false;
    }

    return true;
}


// Main entry point
int main(int argc, char** argv)
{
    LOG(INFO) << "EVS app starting";

    // Register a signal handler
    registerSigHandler();

    // Set up default behavior, then check for command line options
    bool useVehicleHal = true;
    bool printHelp = false;
    const char* evsServiceName = "default";
    for (int i=1; i< argc; i++) {
        if (strcmp(argv[i], "--test") == 0) {
            useVehicleHal = false;
        } else if (strcmp(argv[i], "--hw") == 0) {
            evsServiceName = "EvsEnumeratorHw";
        } else if (strcmp(argv[i], "--mock") == 0) {
            evsServiceName = "EvsEnumeratorHw-Mock";
        } else if (strcmp(argv[i], "--help") == 0) {
            printHelp = true;
        } else {
            printf("Ignoring unrecognized command line arg '%s'\n", argv[i]);
            printHelp = true;
        }
    }
    if (printHelp) {
        printf("Options include:\n");
        printf("  --test\n\tDo not talk to Vehicle Hal, "
               "but simulate 'reverse' instead\n");
        printf("  --hw\n\tBypass EvsManager by connecting directly to EvsEnumeratorHw\n");
        printf("  --mock\n\tConnect directly to EvsEnumeratorHw-Mock\n");
    }

    // Load our configuration information
    ConfigManager config;
    if (property_get_int32("vendor.evs.fake.enable", 0)) {
        if (!config.initialize("/system/etc/automotive/evs/ImxFakeCamConfig.json")) {
            LOG(ERROR) << "Missing or improper configuration for the EVS application.  Exiting.";
            return EXIT_FAILURE;
        }
    }
    if (!config.initialize("/system/etc/automotive/evs/ImxConfig.json")) {
        LOG(ERROR) << "Missing or improper configuration for the EVS application.  Exiting.";
        return EXIT_FAILURE;
    }

    // Set thread pool size to one to avoid concurrent events from the HAL.
    // This pool will handle the EvsCameraStream callbacks.
    // Note:  This _will_ run in parallel with the EvsListener run() loop below which
    // runs the application logic that reacts to the async events.
    configureRpcThreadpool(1, false /* callerWillJoin */);

    // Construct our async helper object
    std::shared_ptr<EvsVehicleListener> pEvsListener = std::make_shared<EvsVehicleListener>();

    // Get the EVS manager service
    LOG(INFO) << "Acquiring EVS Enumerator";
    pEvs = IEvsEnumerator::getService(evsServiceName);
    if (pEvs.get() == nullptr) {
        LOG(ERROR) << "getService(" << evsServiceName
                   << ") returned NULL.  Exiting.";
        return EXIT_FAILURE;
    }

    // Request exclusive access to the EVS display
    LOG(INFO) << "Acquiring EVS Display";

    pDisplay = pEvs->openDisplay_1_1(0);
    if (pDisplay.get() == nullptr) {
        LOG(ERROR) << "EVS Display unavailable.  Exiting.";
        return EXIT_FAILURE;
    }

    // Connect to the Vehicle HAL so we can monitor state
    std::shared_ptr<IVhalClient> pVnet;
    if (useVehicleHal) {
        LOG(INFO) << "Connecting to Vehicle HAL";
        pVnet = IVhalClient::create();
        if (pVnet == nullptr) {
            LOG(ERROR) << "Vehicle HAL getService returned NULL.  Exiting.";
            return EXIT_FAILURE;
        } else {
            auto subscriptionClient = pVnet->getSubscriptionClient(pEvsListener);
            // Register for vehicle state change callbacks we care about
            // Changes in these values are what will trigger a reconfiguration of the EVS pipeline
            if (!subscribeToVHal(subscriptionClient.get(), VehicleProperty::GEAR_SELECTION)) {
                LOG(ERROR) << "Without gear notification, we can't support EVS.  Exiting.";
                return EXIT_FAILURE;
            }
            if (!subscribeToVHal(subscriptionClient.get(), VehicleProperty::TURN_SIGNAL_STATE)) {
                LOG(WARNING) << "Didn't get turn signal notifications, so we'll ignore those.";
            }
        }
    } else {
        LOG(WARNING) << "Test mode selected, so not talking to Vehicle HAL";
    }

    // Configure ourselves for the current vehicle state at startup
    LOG(INFO) << "Constructing state controller";
    pStateController = new EvsStateControl(pVnet, pEvs, pDisplay, config);
    if (!pStateController->startUpdateLoop()) {
        LOG(ERROR) << "Initial configuration failed.  Exiting.";
        return EXIT_FAILURE;
    }

    // Run forever, reacting to events as necessary
    LOG(INFO) << "Entering running state";
    pEvsListener->run(pStateController);

    // In normal operation, we expect to run forever, but in some error conditions we'll quit.
    // One known example is if another process preempts our registration for our service name.
    LOG(ERROR) << "EVS Listener stopped.  Exiting.";

    return EXIT_SUCCESS;
}
