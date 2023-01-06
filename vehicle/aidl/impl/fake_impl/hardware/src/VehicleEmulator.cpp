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
#define LOG_TAG "VehicleEmulator_v2_0"
#include <android/log.h>

#include <android-base/properties.h>
#include <log/log.h>
#include <utils/SystemClock.h>
#include <algorithm>

#include "ProtoMessageConverter.h"
#include "SocketComm.h"

#include "VehicleEmulator.h"

namespace android {
namespace hardware {
namespace automotive {
namespace vehicle {
namespace fake {

using ::aidl::android::hardware::automotive::vehicle::VehiclePropConfig;
using ::aidl::android::hardware::automotive::vehicle::VehicleProperty;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropValue;
using ::aidl::android::hardware::automotive::vehicle::VehiclePropertyType;

VehicleEmulator::VehicleEmulator(FakeVehicleHardware* hw) : mHardware{hw} {
    mHardware->registerEmulator(this);
    ALOGI("Starting SocketComm");
    mSocketComm = std::make_unique<SocketComm>(this);
    mSocketComm->start();
}

VehicleEmulator::~VehicleEmulator() {
    mSocketComm->stop();
}

/**
 * This is called by the HAL when a property changes. We need to notify our clients that it has
 * changed.
 */
void VehicleEmulator::doSetValueFromClient(const VehiclePropValue& aidlPropValue) {
    vhal_proto::EmulatorMessage msg;
    vhal_proto::VehiclePropValue* val = msg.add_value();

    populateProtoVehiclePropValue(val, &aidlPropValue);
    msg.set_status(vhal_proto::RESULT_OK);
    msg.set_msg_type(vhal_proto::SET_PROPERTY_ASYNC);
    mSocketComm->sendMessage(msg);
}

void VehicleEmulator::doGetConfig(VehicleEmulator::EmulatorMessage const& rxMsg,
                                  VehicleEmulator::EmulatorMessage& respMsg) {
    std::vector<VehiclePropConfig> configs = mHardware->getAllPropertyConfigs();
    vhal_proto::VehiclePropGet getProp = rxMsg.prop(0);

    respMsg.set_msg_type(vhal_proto::GET_CONFIG_RESP);
    respMsg.set_status(vhal_proto::ERROR_INVALID_PROPERTY);

    for (auto& config : configs) {
        // Find the config we are looking for
        if (config.prop == getProp.prop()) {
            vhal_proto::VehiclePropConfig* protoCfg = respMsg.add_config();
            populateProtoVehicleConfig(protoCfg, config);
            respMsg.set_status(vhal_proto::RESULT_OK);
            break;
        }
    }
}

void VehicleEmulator::doGetConfigAll(VehicleEmulator::EmulatorMessage const& /* rxMsg */,
                                     VehicleEmulator::EmulatorMessage& respMsg) {
    std::vector<VehiclePropConfig> configs = mHardware->getAllPropertyConfigs();

    respMsg.set_msg_type(vhal_proto::GET_CONFIG_ALL_RESP);
    respMsg.set_status(vhal_proto::RESULT_OK);

    for (auto& config : configs) {
        vhal_proto::VehiclePropConfig* protoCfg = respMsg.add_config();
        populateProtoVehicleConfig(protoCfg, config);
    }
}

void VehicleEmulator::doGetProperty(VehicleEmulator::EmulatorMessage const& rxMsg,
                                    VehicleEmulator::EmulatorMessage& respMsg) {
    int32_t areaId = 0;
    vhal_proto::VehiclePropGet getProp = rxMsg.prop(0);
    int32_t propId = getProp.prop();
    vhal_proto::Status status = vhal_proto::ERROR_INVALID_PROPERTY;

    respMsg.set_msg_type(vhal_proto::GET_PROPERTY_RESP);

    if (getProp.has_area_id()) {
        areaId = getProp.area_id();
    }

    {
        VehiclePropValue request = {
                .areaId = areaId,
                .prop = propId,
        };
        auto val = mHardware->getValue(request);
        if (val.ok()) {
            vhal_proto::VehiclePropValue* protoVal = respMsg.add_value();
            populateProtoVehiclePropValue(protoVal, val.value().get());
            status = vhal_proto::RESULT_OK;
        }
    }

    respMsg.set_status(status);
}

void VehicleEmulator::doGetPropertyAll(VehicleEmulator::EmulatorMessage const& /* rxMsg */,
                                       VehicleEmulator::EmulatorMessage& respMsg) {
    respMsg.set_msg_type(vhal_proto::GET_PROPERTY_ALL_RESP);
    respMsg.set_status(vhal_proto::RESULT_OK);

    {
        for (const auto& prop : mHardware->getAllProperties()) {
            vhal_proto::VehiclePropValue* protoVal = respMsg.add_value();
            populateProtoVehiclePropValue(protoVal, &prop);
        }
    }
}

void VehicleEmulator::doSetProperty(VehicleEmulator::EmulatorMessage const& rxMsg,
                                    VehicleEmulator::EmulatorMessage& respMsg) {
    vhal_proto::VehiclePropValue protoVal = rxMsg.value(0);
    aidl::android::hardware::automotive::vehicle::VehiclePropValue val = {
            .timestamp = elapsedRealtimeNano(),
            .areaId = protoVal.area_id(),
            .prop = protoVal.prop(),
            .status = (aidl::android::hardware::automotive::vehicle::VehiclePropertyStatus)protoVal.status(),
    };

    respMsg.set_msg_type(vhal_proto::SET_PROPERTY_RESP);

    // Copy value data if it is set.  This automatically handles complex data types if needed.
    if (protoVal.has_string_value()) {
        val.value.stringValue = protoVal.string_value().c_str();
    }

    if (protoVal.has_bytes_value()) {
        val.value.byteValues = std::vector<uint8_t> { protoVal.bytes_value().begin(),
                                                 protoVal.bytes_value().end() };
    }

    if (protoVal.int32_values_size() > 0) {
        val.value.int32Values = std::vector<int32_t> { protoVal.int32_values().begin(),
                                                       protoVal.int32_values().end() };
    }

    if (protoVal.int64_values_size() > 0) {
        val.value.int64Values = std::vector<int64_t> { protoVal.int64_values().begin(),
                                                       protoVal.int64_values().end() };
    }

    if (protoVal.float_values_size() > 0) {
        val.value.floatValues = std::vector<float> { protoVal.float_values().begin(),
                                                     protoVal.float_values().end() };
    }

    bool halRes = mHardware->setPropertyFromVehicle(val);
    respMsg.set_status(halRes ? vhal_proto::RESULT_OK : vhal_proto::ERROR_INVALID_PROPERTY);
}

void VehicleEmulator::processMessage(vhal_proto::EmulatorMessage const& rxMsg,
                                     vhal_proto::EmulatorMessage& respMsg) {
    switch (rxMsg.msg_type()) {
        case vhal_proto::GET_CONFIG_CMD:
            doGetConfig(rxMsg, respMsg);
            break;
        case vhal_proto::GET_CONFIG_ALL_CMD:
            doGetConfigAll(rxMsg, respMsg);
            break;
        case vhal_proto::GET_PROPERTY_CMD:
            doGetProperty(rxMsg, respMsg);
            break;
        case vhal_proto::GET_PROPERTY_ALL_CMD:
            doGetPropertyAll(rxMsg, respMsg);
            break;
        case vhal_proto::SET_PROPERTY_CMD:
            doSetProperty(rxMsg, respMsg);
            break;
        default:
            ALOGW("%s: Unknown message received, type = %d", __func__, rxMsg.msg_type());
            respMsg.set_status(vhal_proto::ERROR_UNIMPLEMENTED_CMD);
            break;
    }
}

void VehicleEmulator::populateProtoVehicleConfig(vhal_proto::VehiclePropConfig* protoCfg,
                                                 const VehiclePropConfig& cfg) {
    protoCfg->set_prop(cfg.prop);
    protoCfg->set_access(toInt(cfg.access));
    protoCfg->set_change_mode(toInt(cfg.changeMode));
    protoCfg->set_value_type(toInt(getPropType(cfg.prop)));

    for (auto& configElement : cfg.configArray) {
        protoCfg->add_config_array(configElement);
    }

    if (cfg.configString.size() > 0) {
        protoCfg->set_config_string(cfg.configString.c_str(), cfg.configString.size());
    }
    
    protoCfg->clear_area_configs();
    for (auto& areaConfig : cfg.areaConfigs) {
        auto* protoACfg = protoCfg->add_area_configs();
        protoACfg->set_area_id(areaConfig.areaId);

        switch (getPropType(cfg.prop)) {
            case VehiclePropertyType::STRING:
            case VehiclePropertyType::BOOLEAN:
            case VehiclePropertyType::INT32_VEC:
            case VehiclePropertyType::INT64_VEC:
            case VehiclePropertyType::FLOAT_VEC:
            case VehiclePropertyType::BYTES:
            case VehiclePropertyType::MIXED:
                // Do nothing.  These types don't have min/max values
                break;
            case VehiclePropertyType::INT64:
                protoACfg->set_min_int64_value(areaConfig.minInt64Value);
                protoACfg->set_max_int64_value(areaConfig.maxInt64Value);
                break;
            case VehiclePropertyType::FLOAT:
                protoACfg->set_min_float_value(areaConfig.minFloatValue);
                protoACfg->set_max_float_value(areaConfig.maxFloatValue);
                break;
            case VehiclePropertyType::INT32:
                protoACfg->set_min_int32_value(areaConfig.minInt32Value);
                protoACfg->set_max_int32_value(areaConfig.maxInt32Value);
                break;
            default:
                ALOGW("%s: Unknown property type:  0x%x", __func__, toInt(getPropType(cfg.prop)));
                break;
        }
    }

    protoCfg->set_min_sample_rate(cfg.minSampleRate);
    protoCfg->set_max_sample_rate(cfg.maxSampleRate);
}

void VehicleEmulator::populateProtoVehiclePropValue(vhal_proto::VehiclePropValue* val,
                                                    const VehiclePropValue* aidlPropValue) {
    val->set_prop(aidlPropValue->prop);
    val->set_value_type(toInt(getPropType(aidlPropValue->prop)));
    val->set_timestamp(aidlPropValue->timestamp);
    val->set_status((vhal_proto::VehiclePropStatus)(aidlPropValue->status));
    val->set_area_id(aidlPropValue->areaId);
    
    if (aidlPropValue->value.stringValue.size() > 0) {
        val->set_string_value(aidlPropValue->value.stringValue);
    }
    
    if (aidlPropValue->value.byteValues.size() > 0) {
        val->set_bytes_value(aidlPropValue->value.byteValues.data(), aidlPropValue->value.byteValues.size());
    }

    for (auto& int32Value : aidlPropValue->value.int32Values) {
        val->add_int32_values(int32Value);
    }
    
    for (auto& int64Value : aidlPropValue->value.int64Values) {
        val->add_int64_values(int64Value);
    }

    for (auto& floatValue : aidlPropValue->value.floatValues) {
        val->add_float_values(floatValue);
    }
}

}  // namespace fake
}  // namespace vehicle
}  // namespace automotive
}  // namespace hardware
}  // namespace android
