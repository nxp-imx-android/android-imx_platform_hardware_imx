/*
 *  Copyright 2020-2025 NXP.
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

#ifndef CAMERA_DEVICE_SESSION_HWL_IMPL_H
#define CAMERA_DEVICE_SESSION_HWL_IMPL_H

#include <camera_device_session_hwl.h>
#include <hardware/gralloc1.h>
#include <utils/Condition.h>
#include <utils/Mutex.h>
// #include <utils/Thread.h>

#include <list>
#include <map>
#include <set>

#include "CameraConfigurationParser.h"
#include "CameraDeviceHWLImpl.h"
#include "CameraMetadata.h"
#include "ISPWrapper.h"
#include "JpegBuilder.h"

using namespace fsl;

namespace android {

using google_camera_hal::CameraDeviceHwl;
using google_camera_hal::CameraDeviceSessionHwl;
using google_camera_hal::ErrorCode;
using google_camera_hal::HalStream;
using google_camera_hal::HwlOfflinePipelineRole;
using google_camera_hal::HwlPipelineCallback;
using google_camera_hal::HwlPipelineRequest;
using google_camera_hal::HwlPipelineResult;
using google_camera_hal::HwlSessionCallback;
using google_camera_hal::IMulticamCoordinatorHwl;
using google_camera_hal::MessageType;
using google_camera_hal::NotifyMessage;
using google_camera_hal::RequestTemplate;
using google_camera_hal::SessionDataKey;
using google_camera_hal::Stream;
using google_camera_hal::StreamBuffer;
using google_camera_hal::StreamConfiguration;

// 3aState
struct autoState {
    uint8_t aeMode;
    uint8_t afMode;
    uint8_t awbMode;
    uint8_t aeState;
    uint8_t afState;
    uint8_t awbState;
    int32_t afTriggerId;
    int32_t aeTriggerId;
};

typedef struct tag_pipeline_info {
    uint32_t pipeline_id;
    uint32_t physical_camera_id;
    HwlPipelineCallback pipeline_callback;
    // StreamConfiguration request_config;
    std::vector<Stream> *streams;
    std::vector<HalStream> *hal_streams;
} PipelineInfo;

typedef struct tag_fence_fd_info {
    int acquire_fence_fd;
    int release_fence_fd;
} FenceFdInfo;

typedef struct tag_request {
    uint32_t frame_number;
    HwlPipelineRequest hwlReq;
    std::vector<FenceFdInfo> outBufferFences;
    std::vector<uint32_t> camera_ids;

    // In SubmitRequests(), FrameRequest is a vector, although till now the vector size is 1.
    // In imgProc::HandleImage(), FrameRequest is proced one by one.
    // So need free the vector when last FrameRequest is processed.
    int idx;
    int num;
    void *vector; // save "std::vector<FrameRequest> *frame_request"
} FrameRequest;

// Implementation of CameraDeviceSessionHwl interface
class CameraDeviceSessionHwlImpl : public CameraDeviceSessionHwl {
public:
    static std::unique_ptr<CameraDeviceSessionHwlImpl> Create(
            uint32_t camera_id, std::unique_ptr<HalCameraMetadata> static_meta,
            CameraDeviceHwlImpl *pDev, PhysicalMetaMapPtr physical_devices);
    virtual ~CameraDeviceSessionHwlImpl();

    // Override functions in CameraDeviceSessionHwl
    status_t ConstructDefaultRequestSettings(
            RequestTemplate type, std::unique_ptr<HalCameraMetadata> *default_settings) override;

    status_t PrepareConfigureStreams(const StreamConfiguration & /*request_config*/) override {
        return OK;
    } // Noop for now

    status_t ConfigurePipeline(uint32_t physical_camera_id,
                               HwlPipelineCallback hwl_pipeline_callback,
                               const StreamConfiguration &request_config,
                               const StreamConfiguration &overall_config,
                               uint32_t *pipeline_id) override;

    status_t BuildPipelines() override;

    status_t PreparePipeline(uint32_t /*pipeline_id*/, uint32_t /*frame_number*/) override {
        return OK;
    } // Noop for now

    status_t GetRequiredIntputStreams(const StreamConfiguration & /*overall_config*/,
                                      HwlOfflinePipelineRole /*pipeline_role*/,
                                      std::vector<Stream> * /*streams*/) override {
        return INVALID_OPERATION;
    }

    status_t GetConfiguredHalStream(uint32_t pipeline_id,
                                    std::vector<HalStream> *hal_streams) const override;

    void DestroyPipelines() override;

    status_t SubmitRequests(uint32_t frame_number,
                            std::vector<HwlPipelineRequest> &requests) override;

    status_t Flush() override;

    uint32_t GetCameraId() const override;

    std::vector<uint32_t> GetPhysicalCameraIds() const override;

    status_t GetCameraCharacteristics(
            std::unique_ptr<HalCameraMetadata> *characteristics) const override;

    status_t GetPhysicalCameraCharacteristics(
            uint32_t physical_camera_id,
            std::unique_ptr<HalCameraMetadata> *characteristics) const override;

    status_t SetSessionData(SessionDataKey /*key*/
                            ,
                            void * /*value*/) override {
        return OK;
    } // Noop for now

    status_t GetSessionData(SessionDataKey /*key*/, void ** /*value*/) const override

    {
        return OK;
    } // Noop for now

    void SetSessionCallback(const HwlSessionCallback & /*hwl_session_callback*/) override {}

    status_t FilterResultMetadata(HalCameraMetadata * /*metadata*/) const override {
        return OK;
    } // Noop for now

    std::unique_ptr<IMulticamCoordinatorHwl> CreateMulticamCoordinatorHwl() override {
        return nullptr;
    }

    status_t IsReconfigurationRequired(const HalCameraMetadata * /*old_session*/,
                                       const HalCameraMetadata * /*new_session*/,
                                       bool *reconfiguration_required) const override {
        if (reconfiguration_required == nullptr) {
            return BAD_VALUE;
        }
        *reconfiguration_required = true;
        return OK;
    }

    std::unique_ptr<google_camera_hal::ZoomRatioMapperHwl> GetZoomRatioMapperHwl() override {
        return nullptr;
    }

    void RepeatingRequestEnd(int32_t frame_number,
                         const std::vector<int32_t>& stream_ids) override;

    // End override functions in CameraDeviceSessionHwl

private:
    status_t Initialize(uint32_t camera_id, std::unique_ptr<HalCameraMetadata> static_meta,
                        CameraDeviceHwlImpl *pDev);

    CameraDeviceSessionHwlImpl(PhysicalMetaMapPtr physical_devices);

    int HandleRequest();
    status_t HandleMetaLocked(std::unique_ptr<HalCameraMetadata> &resultMeta, uint64_t timestamp);

    status_t ProcessCapbuf2MultiOutbuf(ImxStreamBuffer *srcBuf,
                                       std::vector<StreamBuffer> &output_buffers,
                                       std::vector<FenceFdInfo> &outFences,
                                       CameraMetadata &requestMeta);
    status_t ProcessCapbuf2Outbuf(ImxStreamBuffer *srcBuf, StreamBuffer &output_buffers,
                                  FenceFdInfo &outFences, CameraMetadata &requestMeta);

    int32_t processJpegBuffer(ImxStreamBuffer *srcBuf, ImxStreamBuffer *dstBuf,
                              CameraMetadata *meta);
    int32_t processFrameBuffer(ImxStreamBuffer *srcBuf, ImxStreamBuffer *dstBuf,
                               CameraMetadata *meta);

    Stream *GetStreamFromStreamBuffer(StreamBuffer *buf);

    int CleanRequestsLocked();
    status_t PickConfigStreamLocked(uint32_t pipeline_id, uint8_t intent);
    int HandleIntent(HwlPipelineRequest *hwReq);

    status_t CapAndFeed(uint32_t frame, FrameRequest *frameRequest);
    void DumpRequest();
    void ReleaseFrameRequest(FrameRequest &frameRequest);

    PipelineInfo *GetPipelineInfo(uint32_t id);
    void requestComplete(libcamera::Request *request);
    std::unique_ptr<libcamera::FrameBuffer> CreateFrameBuffer(
            const buffer_handle_t hnd, const libcamera::StreamConfiguration &streamConfig);

    Stream *GetStreamById(int32_t stream_id, PipelineInfo *pInfo);
    int32_t GetStreamIdFromLibcameraStream(const libcamera::Stream *libCameraStream);
    uint64_t GetTimestampLocked(libcamera::Request *request);

    void CleanFrameBuffersLocked();
    void WaitRequestsFinishAndCleanResource();
    status_t ConfigLibcameraLocked(uint32_t bufferNum, uint32_t format, uint32_t width,
                                   uint32_t height);
    status_t PickAndConfigLibcamera(std::vector<HwlPipelineRequest> &requests);
    void GetConfigSize(int halFmt, uint32_t refWidth, uint32_t refHeight, uint32_t &configWidth,
                       uint32_t &configHeight);

    void ReturnFrameBufferLocked();
    status_t queueRequestToLibcameraLocked(HalCameraMetadata *cameraMeta);
    void ISPProcess(HalCameraMetadata *cameraMeta, libcamera::Request *request);

public:
    CameraSensorMetadata *getSensorData() { return &mSensorData; }
    char *getDevPath(int i) { return (*mDevPath[i]); }
    int getCapsMode(uint8_t sceneMode);
    int32_t getRawV4l2Format() { return m_raw_v4l2_format; }
    uint32_t cameraId() { return camera_id_; }

public:
    bool mDebug;

private:
    Mutex mLock;
    Condition mCondition;

private:
    // Protects the API entry points
    uint32_t camera_id_ = 0;
    uint32_t pipeline_id_ = 0;

    bool pipelines_built_ = false;
    CameraMetadata *m_meta;
    std::map<uint32_t, PipelineInfo *> map_pipeline_info;
    std::map<uint32_t, std::vector<FrameRequest> *> map_frame_request;

    std::vector<uint32_t> camera_ids;

    // ISPProcess
    autoState m3aState;

    sp<JpegBuilder> mJpegBuilder;

    PhysicalMetaMapPtr physical_meta_map_;

    std::unique_ptr<HalCameraMetadata> static_metadata_;

    std::vector<std::shared_ptr<char *>> mDevPath;

    // Maps particular focal length to physical device id
    std::unordered_map<float, uint32_t> physical_focal_length_map_;
    float current_focal_length_ = 0.f;

    int previewIdx;
    int stillcapIdx;
    int recordIdx;
    int callbackIdx;
    int cameraRWIdx;
    int rawIdx;

    std::unique_ptr<HalCameraMetadata> mSettings;

    ImxEngine mCamBlitCopyType;
    ImxEngine mCamBlitCscType;
    char mJpegHw[JPEG_HW_NAME_LEN] = {0};
    CameraSensorMetadata mSensorData;

    int mPreviewResolutions[MAX_RESOLUTION_SIZE];
    int mPreviewResolutionCount;
    int mPictureResolutions[MAX_RESOLUTION_SIZE];
    int mPictureResolutionCount;

    int mMaxWidth = 0;
    int mMaxHeight = 0;

    uint64_t mPreHandleImageTime;
    uint64_t mPreCapAndFeedTime;
    uint64_t mPreSubmitRequestTime;

    uint64_t mInQueRequestIdx = 0;
    uint64_t mDeQueRequestIdx = 0;
    std::unique_ptr<ISPWrapper> m_IspWrapper;

    enum CameraState {
        Stopped,
        Flushing,
        Running,
    };
    CameraState state_;
    std::shared_ptr<libcamera::Camera> camera_;
    libcamera::StreamFormats mSupportedFormats;
    libcamera::Stream *mLibCameraStream = NULL;
    std::list<std::unique_ptr<libcamera::FrameBuffer>> mFrameBuffersFree;
    std::list<std::unique_ptr<libcamera::FrameBuffer>> mFrameBuffersBusy;
    std::list<std::unique_ptr<libcamera::Request>> requestList;
    std::map<libcamera::FrameBuffer *, ImxImageBuffer> mFrameBufferHandleMap;
    android_pixel_format_t m_libcamera_stream_format = HAL_PIXEL_FORMAT_YCBCR_422_I;
    uint32_t m_libcamera_stream_width = 0;
    uint32_t m_libcamera_stream_height = 0;

    uint8_t mCaptureIntent = -1;
    bool m_bConfigLibcameraByIntent = false;

    uint32_t mOmitFrames;
    uint32_t mOmitFrmCount;

    char mSocType[128];
    uint32_t maxStreamWidth = 0;
    uint32_t maxStreamHeight = 0;

    // dewarp on ox03c10
    ImxImageBuffer mDewarpBuf;

    // process multi-cameras in multi-threads
    class ImgProcThread : public Thread {
    public:
        ImgProcThread(CameraDeviceSessionHwlImpl *pSession) : Thread(false), mSession(pSession) {}

        virtual void onFirstRef() { run("ImgProcThread", PRIORITY_URGENT_DISPLAY); }

        virtual status_t readyToRun() {
            ALOGI("ImgProcThread, readyToRun");
            return 0;
        }

        virtual bool threadLoop() {
            int ret = mSession->HandleImage();
            if (ret != OK) {
                ALOGI("%s exit...", __func__);
                return false;
            }
            return true;
        }

    private:
        CameraDeviceSessionHwlImpl *mSession;
    };

    int HandleImage();
    std::list<libcamera::Request *> mRequestPendingList;
    Mutex mRequestPendingListLock;
    Condition mRequestPendingListCond;

    void requestCompleteDispatch(libcamera::Request *request);

    static std::map<CameraDeviceSessionHwlImpl *, sp<ImgProcThread>> sessionThreadMap;
    static Mutex sessionThreadMapLock;

public:
    int32_t m_raw_v4l2_format = -1;
    int8_t m_color_arrange = -1;
    int mUseCpuEncoder;
};

} // namespace android

#endif // CAMERA_DEVICE_SESSION_HWL_IMPL_H
