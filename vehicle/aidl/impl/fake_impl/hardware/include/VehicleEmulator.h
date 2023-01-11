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

#ifndef android_hardware_automotive_vehicle_aidl_impl_VehicleHalEmulator_H_
#define android_hardware_automotive_vehicle_aidl_impl_VehicleHalEmulator_H_

#include <log/log.h>
#include <memory>
#include <thread>
#include <vector>

#include "CommConn.h"
#include "SocketComm.h"
#include "VehicleHalProto.pb.h"
#include "FakeVehicleHardware.h"

namespace android {
namespace hardware {
namespace automotive {
namespace vehicle {
namespace fake {

/**
 * Emulates vehicle by providing controlling interface from host side either through ADB or Pipe.
 */
class VehicleEmulator : public MessageProcessor {
   public:
    VehicleEmulator();
    virtual ~VehicleEmulator();

    void doSetValueFromClient(const aidl::android::hardware::automotive::vehicle::VehiclePropValue& propValue);
    void processMessage(vhal_proto::EmulatorMessage const& rxMsg,
                        vhal_proto::EmulatorMessage& respMsg) override;
    void setHardware(FakeVehicleHardware* hw);

   private:
    friend class ConnectionThread;
    using EmulatorMessage = vhal_proto::EmulatorMessage;

    void doGetConfig(EmulatorMessage const& rxMsg, EmulatorMessage& respMsg);
    void doGetConfigAll(EmulatorMessage const& rxMsg, EmulatorMessage& respMsg);
    void doGetProperty(EmulatorMessage const& rxMsg, EmulatorMessage& respMsg);
    void doGetPropertyAll(EmulatorMessage const& rxMsg, EmulatorMessage& respMsg);
    void doSetProperty(EmulatorMessage const& rxMsg, EmulatorMessage& respMsg);
    void populateProtoVehicleConfig(vhal_proto::VehiclePropConfig* protoCfg,
                                    const aidl::android::hardware::automotive::vehicle::VehiclePropConfig& cfg);
    void populateProtoVehiclePropValue(vhal_proto::VehiclePropValue* protoVal,
                                       const aidl::android::hardware::automotive::vehicle::VehiclePropValue* val);
private:
    FakeVehicleHardware* mHardware;
    std::mutex mLock;
    std::unique_ptr<SocketComm> mSocketComm;
};

}  // namespace fake
}  // namespace vehicle
}  // namespace automotive
}  // namespace hardware
}  // namespace android

#endif // android_hardware_automotive_vehicle_aidl_impl_VehicleHalEmulator_H_
