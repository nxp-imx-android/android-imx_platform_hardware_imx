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

#include <signal.h>
#include <stdio.h>

#include <hidl/HidlTransportSupport.h>
#include <hwbinder/IPCThreadState.h>
#include <utils/Errors.h>
#include <utils/StrongPointer.h>
#include <utils/Log.h>
#include <android-base/logging.h>
#include <android-base/macros.h>    // arraysize

#include <android/hardware/automotive/evs/1.1/IEvsEnumerator.h>
#include <android/hardware/automotive/evs/1.1/IEvsDisplay.h>

#include <hwbinder/ProcessState.h>

#include "EvsStateControl.h"
#include "EvsVehicleListener.h"
#include "ConfigManager.h"


// libhidl:
using android::hardware::configureRpcThreadpool;
using android::hardware::joinRpcThreadpool;

namespace {

android::sp<IEvsEnumerator> pEvs;
android::sp<IEvsDisplay> pDisplay;
EvsStateControl *pStateController;

void sigHandler(int sig) {
    LOG(ERROR) << "evs_app is being terminated on receiving a signal " << sig;
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
static bool subscribeToVHal(sp<IVehicle> pVnet,
                            sp<IVehicleCallback> listener,
                            VehicleProperty propertyId) {
    assert(pVnet != nullptr);
    assert(listener != nullptr);

    // Register for vehicle state change callbacks we care about
    // Changes in these values are what will trigger a reconfiguration of the EVS pipeline
    SubscribeOptions optionsData[] = {
        {
            .propId = static_cast<int32_t>(propertyId),
            .flags  = SubscribeFlags::EVENTS_FROM_CAR
        },
    };
    hidl_vec <SubscribeOptions> options;
    options.setToExternal(optionsData, arraysize(optionsData));
    StatusCode status = pVnet->subscribe(listener, options);
    if (status != StatusCode::OK) {
        LOG(WARNING) << "VHAL subscription for property " << static_cast<int32_t>(propertyId)
                     << " failed with code " << static_cast<int32_t>(status);
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
        printf("  --test   Do not talk to Vehicle Hal, but simulate 'reverse' instead\n");
        printf("  --hw     Bypass EvsManager by connecting directly to EvsEnumeratorHw\n");
        printf("  --mock   Connect directly to EvsEnumeratorHw-Mock\n");
    }

    // Load our configuration information
    ConfigManager config;
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
    sp<EvsVehicleListener> pEvsListener = new EvsVehicleListener();

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
    sp<IVehicle> pVnet;
    if (useVehicleHal) {
        LOG(INFO) << "Connecting to Vehicle HAL";
        pVnet = IVehicle::getService();
        if (pVnet.get() == nullptr) {
            LOG(ERROR) << "Vehicle HAL getService returned NULL.  Exiting.";
            return EXIT_FAILURE;
        } else {
            // Register for vehicle state change callbacks we care about
            // Changes in these values are what will trigger a reconfiguration of the EVS pipeline
            if (!subscribeToVHal(pVnet, pEvsListener, VehicleProperty::GEAR_SELECTION)) {
                LOG(ERROR) << "Without gear notification, we can't support EVS.  Exiting.";
                return EXIT_FAILURE;
            }
            if (!subscribeToVHal(pVnet, pEvsListener, VehicleProperty::TURN_SIGNAL_STATE)) {
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
