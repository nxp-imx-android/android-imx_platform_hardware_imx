/*
 *  Copyright 2020 NXP.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#ifndef CAMERA_METADATA_H
#define CAMERA_METADATA_H

#include <string>
#include <stdint.h>
#include <inttypes.h>
#include <log/log.h>
#include <system/graphics.h>
#include <system/camera.h>
#include <hal_types.h>
#include "hal_camera_metadata.h"

namespace android {

class CameraDeviceHwlImpl;

using google_camera_hal::RequestTemplate;
using google_camera_hal::HalCameraMetadata;

class CameraMetadata
{
public:
    CameraMetadata() {}
    CameraMetadata(HalCameraMetadata *request_meta) { m_request_meta = request_meta; }

public:
    status_t createMetadata(CameraDeviceHwlImpl *pDev);
    HalCameraMetadata* GetStaticMeta();

    CameraMetadata* Clone();

    status_t getRequestSettings(RequestTemplate type,
                                       std::unique_ptr<HalCameraMetadata>* default_settings);
    status_t setTemplate();

    //private:
    status_t createSettingTemplate(std::unique_ptr<HalCameraMetadata>& base,
                                          RequestTemplate type);
    //private:
    // std::unique_ptr<HalCameraMetadata> hal_meta[(uint32_t)RequestTemplate::kManual + 1];

    int32_t getGpsCoordinates( double *pCoords, int count);
    int32_t getGpsTimeStamp(int64_t &timeStamp);
    int32_t getGpsProcessingMethod(uint8_t* src, int count);
    int32_t getFocalLength(float &focalLength);
    int32_t getJpegRotation(int32_t &jpegRotation);
    int32_t getJpegQuality(int32_t &quality);
    int32_t getJpegThumbQuality(int32_t &thumb);
    int32_t getJpegThumbSize(int &width, int &height);
    int32_t getMaxJpegSize(int &size);

private:
    std::unique_ptr<HalCameraMetadata> m_static_meta;
    std::unique_ptr<HalCameraMetadata> m_template_meta[(uint32_t)RequestTemplate::kManual + 1];
    HalCameraMetadata *m_request_meta; // meta from framework

    CameraDeviceHwlImpl *mDev;
};

}  // namespace android
#endif