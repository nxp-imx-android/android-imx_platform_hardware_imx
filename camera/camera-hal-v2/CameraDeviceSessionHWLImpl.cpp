/*
 *  Copyright 2020-2026 NXP.
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
// #define LOG_NDEBUG 0
#define LOG_TAG "CameraDeviceSessionHwlImpl"

#include "CameraDeviceSessionHWLImpl.h"

#include <binder/MemoryHeapBase.h>
#include <hardware/camera3.h>
#include <hardware/gralloc.h>
#include <inttypes.h>
#include <log/log.h>
#include <sync/sync.h>
#include <ui/GraphicBufferAllocator.h>
#include <utils/Trace.h>

#include "CameraMetadata.h"
#include "CameraUtils.h"

#define DEBUG_FRAME_NUM 10

using namespace cameraconfigparser;
namespace android {
std::map<CameraDeviceSessionHwlImpl *, sp<CameraDeviceSessionHwlImpl::ImgProcThread>>
        CameraDeviceSessionHwlImpl::sessionThreadMap;
Mutex CameraDeviceSessionHwlImpl::sessionThreadMapLock;

static uint32_t importCount;
static uint32_t freeCount;
// ImportFence and closeFence are refed from
// hardware/interfaces/camera/common/1.0/default/HandleImporter.cpp. If use functions from
// HandleImporter.cpp, will lead to add a series of libs.
static int importFence(const native_handle_t *handle, bool debug) {
    if (handle == nullptr || handle->numFds == 0)
        return -1;

    if (handle->numFds != 1) {
        ALOGE("invalid fence handle with %d file descriptors", handle->numFds);
        return -1;
    }

    int fd = dup(handle->data[0]);
    if (fd < 0)
        ALOGE("failed to dup fence fd %d, %s", handle->data[0], strerror(errno));

    importCount++;
    if (debug)
        ALOGI("importFence, fd %d, importCount %u, freeCount %u", fd, importCount, freeCount);

    return fd;
}

static void closeFence(int fd, bool debug) {
    if (fd >= 0) {
        freeCount++;
        if (debug)
            ALOGI("freeFence, fd %d, importCount %u, freeCount %u", fd, importCount, freeCount);

        close(fd);
    }
}

static void ItvlStat(uint64_t &preTime, char *name) {
    uint64_t curTime = systemTime();

    if (preTime > 0)
        ALOGI("%s itvl is %ld ms", name, (curTime - preTime) / 1000000);

    preTime = curTime;

    return;
}

std::unique_ptr<CameraDeviceSessionHwlImpl> CameraDeviceSessionHwlImpl::Create(
        uint32_t camera_id, std::unique_ptr<HalCameraMetadata> pMeta, CameraDeviceHwlImpl *pDev,
        PhysicalMetaMapPtr physical_devices) {
    if (pMeta.get() == nullptr) {
        return nullptr;
    }

    auto session = std::unique_ptr<CameraDeviceSessionHwlImpl>(
            new CameraDeviceSessionHwlImpl(std::move(physical_devices)));
    if (session == nullptr) {
        ALOGE("%s: Creating CameraDeviceSessionHwlImpl failed", __func__);
        return nullptr;
    }

    status_t res = session->Initialize(camera_id, std::move(pMeta), pDev);
    if (res != OK) {
        ALOGE("%s: session->Initialize  failed: %s(%d)", __func__, strerror(-res), res);
        return nullptr;
    }

    return session;
}

status_t CameraDeviceSessionHwlImpl::Initialize(uint32_t camera_id,
                                                std::unique_ptr<HalCameraMetadata> pMeta,
                                                CameraDeviceHwlImpl *pDev) {
    int ret;
    camera_id_ = camera_id;

    static_metadata_ = std::move(pMeta);

    if (pDev == NULL)
        return BAD_VALUE;

    mCamWrap = pDev->GetCameraWrapper();
    if ((mCamWrap == NULL) || (mCamWrap->camera_ == nullptr)) {
        ALOGE("%s,  camera_null !!!", __func__);
        return BAD_VALUE;
    }

    m_meta = pDev->m_meta->Clone();

    ALOGI("Initialize, meta %p, entry count %zu", static_metadata_.get(),
          static_metadata_->GetEntryCount());

    mDevPath = pDev->mDevPath;

    CameraSensorMetadata *cam_metadata = &(pDev->mSensorData);
    m_IspWrapper = std::make_unique<ISPWrapper>(cam_metadata);

    // create jpeg builder
    mJpegBuilder = new JpegBuilder();

    mInQueRequestIdx = 0;
    mDeQueRequestIdx = 0;

    mMaxWidth = pDev->mMaxWidth;
    mMaxHeight = pDev->mMaxHeight;

    // Device may be destroyed after create session, need copy some members from device.
    memcpy(mJpegHw, pDev->mJpegHw, JPEG_HW_NAME_LEN);
    mUseCpuEncoder = pDev->mUseCpuEncoder;
    mCamBlitCscType = pDev->mCamBlitCscType;
    mSensorData = pDev->mSensorData;

    int halFormat = 0;
    // Per-stream image process devices will be created in ConfigurePipeline()
    if (strcmp(mSensorData.v4l2_format, "nv12") == 0)
        halFormat = HAL_PIXEL_FORMAT_YCBCR_420_888;
    else if (strcmp(mSensorData.v4l2_format, "rgb3") == 0) {
        halFormat = HAL_PIXEL_FORMAT_RGB_888;
    } else
        halFormat = HAL_PIXEL_FORMAT_YCBCR_422_I;

    mCamWrap->SetHalFormat(halFormat);

    m_bConfigLibcameraByIntent = false;

    mPreviewResolutionCount = pDev->mPreviewResolutionCount;
    memcpy(mPreviewResolutions, pDev->mPreviewResolutions, MAX_RESOLUTION_SIZE * sizeof(int));
    mPictureResolutionCount = pDev->mPictureResolutionCount;
    memcpy(mPictureResolutions, pDev->mPictureResolutions, MAX_RESOLUTION_SIZE * sizeof(int));

    memset(&mDewarpBuf, 0, sizeof(mDewarpBuf));
    if (mSensorData.mNeedDewarp) {
        ret = AllocPhyBuffer(mMaxWidth, mMaxHeight, mCamWrap->mHalFormat, mDewarpBuf, true);
        if (ret) {
            ALOGE("%s:%d AllocPhyBuffer for mDewarpBuf failed, width %d, height %d, format 0x%x",
                  __func__, __LINE__, mMaxWidth, mMaxHeight, mCamWrap->mHalFormat);
            return BAD_VALUE;
        }
        mDewarpDevice = ImageProcessFactory::getInstance().createDeviceByEngine(mSensorData.mDewarpEng);
        ALOGI("%s: created dewarp device '%s' (engine %d)", __func__,
              mDewarpDevice ? mDewarpDevice->getName() : "null", mSensorData.mDewarpEng);
    }

    mCamWrap->camera_->acquire();
    mCamWrap->camera_->requestCompleted.connect(this, &CameraDeviceSessionHwlImpl::requestCompleteDispatch);

    property_get("ro.boot.soc_type", mSocType, "");
    ALOGI("%s: mSocType :%s \n", __FUNCTION__, mSocType);

    sessionThreadMapLock.lock();
    auto iter = sessionThreadMap.find(this);
    if (iter == sessionThreadMap.end()) {
        sp<ImgProcThread> imgProcThread = new ImgProcThread(this);
        sessionThreadMap[this] = imgProcThread;
        ALOGI("%s: bind session %p to task %p, tid %d", __func__, this, imgProcThread.get(),
              imgProcThread->getTid());
    } else {
        ALOGW("%s: unexpected! session %p has task %p", __func__, this,
              sessionThreadMap[this].get());
    }
    sessionThreadMapLock.unlock();

    return OK;
}

CameraDeviceSessionHwlImpl::CameraDeviceSessionHwlImpl(PhysicalMetaMapPtr physical_devices) {
    ALOGI("%s: this %p", __func__, this);

    memset(&m3aState, 0, sizeof(m3aState));

    m_meta = NULL;
    mSettings = NULL;
    mDebug = false;
    mPreCapAndFeedTime = 0;
    mPreSubmitRequestTime = 0;
    mCamWrap = nullptr;
    memset(videoPath, 0, sizeof(videoPath));

    physical_meta_map_ = std::move(physical_devices);
}

CameraDeviceSessionHwlImpl::~CameraDeviceSessionHwlImpl() {
    ALOGI("%s: this %p, camera_ %p, %p", __func__, this, mCamWrap->camera_.get());

    sessionThreadMapLock.lock();
    auto iter = sessionThreadMap.find(this);
    if (iter != sessionThreadMap.end()) {
        sp<ImgProcThread> imgProcThread = sessionThreadMap[this];
        if (imgProcThread != NULL) {
            imgProcThread->requestExitAndWait();
            ALOGI("%s, task %p, tid %d, exited for session %p", __func__, imgProcThread.get(),
                  imgProcThread->getTid(), this);
        }
        sessionThreadMap.erase(iter);
    } else {
        ALOGW("%s: unexpected! session %p has no task", __func__, this);
    }
    sessionThreadMapLock.unlock();

    if (mJpegBuilder != NULL)
        mJpegBuilder.clear();

    if (mSettings != NULL)
        mSettings.reset();

    if (m_meta) {
        delete m_meta;
        m_meta = NULL;
    }

    if (mCamWrap->camera_) {
        mCamWrap->camera_->requestCompleted.disconnect();
        mCamWrap->camera_->release();
    }

    if (mDewarpBuf.buffer)
        FreePhyBuffer(mDewarpBuf.buffer);
}

PipelineInfo *CameraDeviceSessionHwlImpl::GetPipelineInfo(uint32_t id) {
    auto it = map_pipeline_info.find(id);
    if (it != map_pipeline_info.end())
        return it->second;

    return NULL;
}

#define WAIT_TIME_OUT 100000000LL // unit ns, wait 100ms

void CameraDeviceSessionHwlImpl::DumpRequest() {
    ALOGI("%s: list_frame_request size %zu, addr %p", __func__, list_frame_request.size(),
          &list_frame_request);

    for (auto it = list_frame_request.begin(); it != list_frame_request.end(); it++) {
        FrameRequest *request = *it;
        if (request)
            ALOGI("%s: frame %u", __func__, request->frame_number);
    }
}

void CameraDeviceSessionHwlImpl::ReleaseFrameRequest(FrameRequest &frameRequest) {
    HwlPipelineRequest &hwReq = frameRequest.hwlReq;

    // clear hwReq
    hwReq.settings.reset();
    hwReq.output_buffers.clear();
    hwReq.input_buffers.clear();
    hwReq.input_buffer_metadata.clear();

    // close fence
    int size = frameRequest.outBufferFences.size();
    for (int i = 0; i < size; i++) {
        int acquire_fence_fd = frameRequest.outBufferFences[i].acquire_fence_fd;
        if (acquire_fence_fd > -1)
            closeFence(acquire_fence_fd, mDebug);

        frameRequest.outBufferFences[i].acquire_fence_fd = -1;
    }
    frameRequest.outBufferFences.clear();

    return;
}

Stream *CameraDeviceSessionHwlImpl::GetStreamFromStreamBuffer(StreamBuffer *buf) {
    if (buf == NULL)
        return NULL;

    ALOGV("%s: buf->stream_id %d", __func__, buf->stream_id);

    for (auto it = map_pipeline_info.begin(); it != map_pipeline_info.end(); it++) {
        PipelineInfo *pInfo = it->second;
        if (pInfo == NULL)
            return NULL;

        std::vector<Stream> *streams = pInfo->streams;
        int size = (int)streams->size();
        for (int i = 0; i < size; i++) {
            ALOGV("pInfo %p, streams[%d].id %d", pInfo, i, streams->at(i).id);
            if (streams->at(i).id == buf->stream_id)
                return &(streams->at(i));
        }
    }

    return NULL;
}

int32_t CameraDeviceSessionHwlImpl::processFrameBuffer(ImxStreamBuffer *srcBuf,
                                                       ImxStreamBuffer *dstBuf,
                                                       CameraMetadata *meta) {
    if ((srcBuf == NULL) || (dstBuf == NULL) || (meta == NULL)) {
        ALOGE("%s srcBuf %p, dstBuf %p, meta %p", __func__, srcBuf, dstBuf, meta);
        return BAD_VALUE;
    }

    int32_t streamId = dstBuf->mStream->id();
    auto it = mStreamDevices.find(streamId);
    if (it == mStreamDevices.end() || !it->second) {
        ALOGE("%s: no image process device for stream id %d", __func__, streamId);
        return -EINVAL;
    }

    return convertImage(it->second, *dstBuf, *srcBuf);
}

int32_t CameraDeviceSessionHwlImpl::convertImage(std::shared_ptr<fsl::ImageProcessDevice> &device,
                                                 ImxStreamBuffer &dstBuf, ImxStreamBuffer &srcBuf) {
    if (!device) {
        ALOGE("%s: image process device is null", __func__);
        return -EINVAL;
    }

    ImxImageBuffer srcImg, dstImg;
    memset(&srcImg, 0, sizeof(srcImg));
    memset(&dstImg, 0, sizeof(dstImg));
    StreamBufferToImageBuffer(srcBuf, srcImg);
    StreamBufferToImageBuffer(dstBuf, dstImg);

    srcImg.mFormat = unifyNV12Format(srcImg.mFormat);
    dstImg.mFormat = unifyNV12Format(dstImg.mFormat);

    device->setDebug(mDebug);
    int ret = device->convert(dstImg, srcImg);
    if (ret != 0)
        ALOGE("%s: device '%s' convert failed, ret=%d", __func__, device->getName(), ret);

    return ret;
}

int32_t CameraDeviceSessionHwlImpl::processJpegBuffer(ImxStreamBuffer *srcBuf,
                                                      ImxStreamBuffer *dstBuf,
                                                      CameraMetadata *meta) {
    int32_t ret = 0;
    int32_t encodeQuality = 100, thumbQuality = 100;
    int32_t thumbWidth = 0, thumbHeight = 0;
    JpegParams *mainJpeg = NULL, *thumbJpeg = NULL;
    void *rawBuf = NULL, *thumbBuf = NULL;
    uint8_t *pDst = NULL;
    struct camera3_jpeg_blob *jpegBlob = NULL;
    uint32_t bufSize = 0;
    int maxJpegSize = mSensorData.maxjpegsize;
    ImxStreamBuffer resizeBuf;
    memset(&resizeBuf, 0, sizeof(resizeBuf));

    if ((srcBuf == NULL) || (dstBuf == NULL) || (meta == NULL)) {
        ALOGE("%s srcBuf %p, dstBuf %p, meta %p", __func__, srcBuf, dstBuf, meta);
        return BAD_VALUE;
    }

    ImxStream *srcStream = srcBuf->mStream;
    ImxStream *capture = dstBuf->mStream;

    if (capture == NULL || srcStream == NULL) {
        ALOGE("%s invalid param, capture %p, srcStream %p", __func__, capture, srcStream);
        return BAD_VALUE;
    }

    if (srcBuf->mVirtAddr == NULL)
        GetBufferInfoFromHandle(srcBuf->buffer, *srcBuf, false, true);

    if (dstBuf->mVirtAddr == NULL)
        GetBufferInfoFromHandle(dstBuf->buffer, *dstBuf, false, true);

    ret = meta->getJpegQuality(encodeQuality);
    if (ret != NO_ERROR) {
        ALOGE("%s getJpegQuality failed", __func__);
        return BAD_VALUE;
    }

    if ((encodeQuality < 0) || (encodeQuality > 100)) {
        encodeQuality = 100;
    }

    ret = meta->getJpegThumbQuality(thumbQuality);
    if (ret != NO_ERROR) {
        ALOGE("%s getJpegThumbQuality failed", __func__);
        return BAD_VALUE;
    }

    if ((thumbQuality < 0) || (thumbQuality > 100)) {
        thumbQuality = 100;
    }

    int captureSize = 0;
    int alignedw, alignedh, c_stride;
    switch (srcStream->format()) {
        case HAL_PIXEL_FORMAT_YCbCr_420_P:
            alignedw = ALIGN_PIXEL_32(capture->mWidth);
            alignedh = ALIGN_PIXEL_4(capture->mHeight);
            c_stride = (alignedw / 2 + 15) / 16 * 16;
            captureSize = alignedw * alignedh + c_stride * alignedh;
            break;
        case HAL_PIXEL_FORMAT_YCbCr_420_SP:
        case HAL_PIXEL_FORMAT_YCbCr_420_888:
            alignedw = ALIGN_PIXEL_16(capture->mWidth);
            alignedh = ALIGN_PIXEL_16(capture->mHeight);
            captureSize = alignedw * alignedh * 3 / 2;
            break;

        case HAL_PIXEL_FORMAT_YCbCr_422_I:
            alignedw = ALIGN_PIXEL_16(capture->mWidth);
            alignedh = ALIGN_PIXEL_16(capture->mHeight);
            captureSize = alignedw * alignedh * 2;
            break;
        case HAL_PIXEL_FORMAT_YCbCr_422_SP:
            alignedw = ALIGN_PIXEL_16(capture->mWidth);
            alignedh = ALIGN_PIXEL_16(capture->mHeight);
            captureSize = alignedw * alignedh * 2;
            break;
        case HAL_PIXEL_FORMAT_YCbCr_444_888:
            alignedw = ALIGN_PIXEL_16(capture->mWidth);
            alignedh = ALIGN_PIXEL_16(capture->mHeight);
            captureSize = alignedw * alignedh * 3;
            break;

        default:
            ALOGE("Error: %s format 0x%x not supported", __func__, srcStream->format());
    }

    // The encoder output buffer must be able to hold the whole encoded JPEG. Sizing it to the
    // raw pixel size (captureSize) is not safe: a high quality or noisy scene can
    // compress to more bytes than the raw output size. When that happens the libjpeg soft
    // encoder destination manager wraps back to the start of the buffer (see
    // jpegBuilder_empty_output_buffer in YuvToJpegEncoder.cpp) and overwrites the SOI (ff d8)
    // header, producing an undecodable JPEG.
    // The issue is easy to occur at low resolutions, so double the jpegBufSize in this case.

    int jpegBufSize =
            (capture->mWidth <= 320 && capture->mHeight <= 240) ? (captureSize * 2) : captureSize;

    sp<MemoryHeapBase> rawFrame(new MemoryHeapBase(jpegBufSize, 0, "rawFrame"));
    rawBuf = rawFrame->getBase();
    if (rawBuf == MAP_FAILED) {
        ALOGE("%s new MemoryHeapBase failed", __func__);
        return BAD_VALUE;
    }

    sp<MemoryHeapBase> thumbFrame(new MemoryHeapBase(jpegBufSize, 0, "thumbFrame"));
    thumbBuf = thumbFrame->getBase();
    if (thumbBuf == MAP_FAILED) {
        ALOGE("%s new MemoryHeapBase failed", __func__);
        return BAD_VALUE;
    }

    // Handle zoom in
    if (srcStream->mZoomRatio > 1.0) {
        resizeBuf.mFormatSize = srcBuf->mFormatSize;
        ret = AllocPhyBuffer(srcBuf->mWidth, srcBuf->mHeight, srcBuf->mFormat, resizeBuf, true, false, true);
        if (ret) {
            ALOGE("%s:%d AllocPhyBuffer failed", __func__, __LINE__);
            return BAD_VALUE;
        }

        resizeBuf.mStream = srcBuf->mStream;
        int32_t streamId = dstBuf->mStream->id();
        auto devIt = mStreamDevices.find(streamId);
        if (devIt == mStreamDevices.end() || !devIt->second) {
            ALOGE("%s: no image process device for stream id %d", __func__, streamId);
            FreePhyBuffer(resizeBuf.buffer);
            return BAD_VALUE;
        }
        convertImage(devIt->second, resizeBuf, *srcBuf);

        SwitchImxBuf(*srcBuf, resizeBuf);
    }

    mainJpeg = new JpegParams((uint8_t*)srcBuf->mVirtAddr, (uint8_t*)(uintptr_t)srcBuf->mPhyAddr,
                              srcBuf->mSize, srcBuf->mFd, srcBuf->buffer, (uint8_t*)rawBuf,
                              jpegBufSize, encodeQuality, srcStream->mWidth, srcStream->mHeight,
                              capture->mWidth, capture->mHeight, srcStream->format());

    ret = meta->getJpegThumbSize(thumbWidth, thumbHeight);
    if (ret != NO_ERROR) {
        ALOGE("%s getJpegThumbSize failed", __func__);
    }

    if ((thumbWidth > 0) && (thumbHeight > 0)) {
        int thumbSize = jpegBufSize;

        thumbJpeg =
                new JpegParams((uint8_t *)srcBuf->mVirtAddr, (uint8_t *)(uintptr_t)srcBuf->mPhyAddr,
                               srcBuf->mSize, srcBuf->mFd, srcBuf->buffer, (uint8_t *)thumbBuf,
                               thumbSize, thumbQuality, srcStream->mWidth, srcStream->mHeight,
                               thumbWidth, thumbHeight, srcStream->format());
    }

    {
        int32_t streamId = dstBuf->mStream->id();
        auto devIt = mStreamDevices.find(streamId);
        auto jpegResizeDevice = (devIt != mStreamDevices.end()) ? devIt->second : nullptr;
        ret = mJpegBuilder->encodeImage(mainJpeg, thumbJpeg, mJpegHw, (*meta), mDebug,
                                        jpegResizeDevice);
    }
    if (ret != NO_ERROR) {
        ALOGE("%s encodeImage failed", __func__);
        goto err_out;
    }

    ret = mJpegBuilder->buildImage(dstBuf, mJpegHw);
    if (ret != NO_ERROR) {
        ALOGE("%s buildImage failed", __func__);
        goto err_out;
    }

    // write jpeg size
    pDst = (uint8_t *)dstBuf->mVirtAddr;
    bufSize = (maxJpegSize <= (int)dstBuf->mSize) ? maxJpegSize : dstBuf->mSize;

    jpegBlob = (struct camera3_jpeg_blob *)(pDst + bufSize - sizeof(struct camera3_jpeg_blob));
    jpegBlob->jpeg_blob_id = CAMERA3_JPEG_BLOB_ID;
    jpegBlob->jpeg_size = mJpegBuilder->getImageSize();

    ALOGI("%s, dstbuf size %d, captureSize %d, jpeg_size %d, jpegBufSize %d, max jpeg size %d",
          __func__, (int)dstBuf->mSize, captureSize, jpegBlob->jpeg_size, jpegBufSize, maxJpegSize);

err_out:
    if (mainJpeg != NULL)
        delete mainJpeg;

    if (thumbJpeg != NULL)
        delete thumbJpeg;

    if (resizeBuf.mPhyAddr > 0) {
        SwitchImxBuf(*srcBuf, resizeBuf);
        FreePhyBuffer(resizeBuf.buffer);
    }

    return ret;
}

status_t CameraDeviceSessionHwlImpl::HandleMetaLocked(
        std::unique_ptr<HalCameraMetadata> &resultMeta, uint64_t timestamp) {
    status_t ret;
    camera_metadata_ro_entry entry;

    ret = resultMeta->Get(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, &entry);
    if (ret != NAME_NOT_FOUND) {
        m3aState.aeState = ANDROID_CONTROL_AE_STATE_CONVERGED;
        ALOGV("ae precature trigger");
    } else {
        m3aState.aeState = ANDROID_CONTROL_AE_STATE_CONVERGED;
    }

    resultMeta->Set(ANDROID_CONTROL_AE_STATE, &m3aState.aeState, 1);

    ret = resultMeta->Get(ANDROID_CONTROL_AE_PRECAPTURE_ID, &entry);
    if (ret != NAME_NOT_FOUND) {
        m3aState.aeTriggerId = entry.data.i32[0];
    }

    resultMeta->Set(ANDROID_CONTROL_AE_PRECAPTURE_ID, &m3aState.aeTriggerId, 1);

    ret = resultMeta->Get(ANDROID_CONTROL_AF_TRIGGER_ID, &entry);
    if (ret != NAME_NOT_FOUND) {
        m3aState.afTriggerId = entry.data.i32[0];
    }

    resultMeta->Set(ANDROID_CONTROL_AF_TRIGGER_ID, &m3aState.afTriggerId, 1);

    resultMeta->Set(ANDROID_SENSOR_TIMESTAMP, (int64_t *)&timestamp, 1);

    // auto focus control.
    m3aState.afState = ANDROID_CONTROL_AF_STATE_INACTIVE;
    resultMeta->Set(ANDROID_CONTROL_AF_STATE, &m3aState.afState, 1);

    // auto white balance control.
    m3aState.awbState = ANDROID_CONTROL_AWB_STATE_CONVERGED;
    resultMeta->Set(ANDROID_CONTROL_AWB_STATE, &m3aState.awbState, 1);

    return OK;
}

void CameraDeviceSessionHwlImpl::CleanFrameBuffersLocked() {
    for (auto &it : mFrameBuffersFree) {
        libcamera::FrameBuffer *frameBuffer = it.get();
        auto iter = mFrameBufferHandleMap.find(frameBuffer);
        if (iter == mFrameBufferHandleMap.end()) {
            ALOGW("%s: !!! unexpected, mFrameBufferHandleMap has no key %p", __func__, frameBuffer);
            continue;
        }

        ImxImageBuffer imgBuf = mFrameBufferHandleMap[frameBuffer];
        FreePhyBuffer(imgBuf.buffer);
        ALOGI("%s: mFrameBufferHandleMap[%p] %p, this %p", __func__, frameBuffer, imgBuf.buffer,
              this);

        it.reset();
    }

    return;
}

/*
In libcamera callback thread where requestComplete() is in, there's nested lock sequence
as libcameraLock -> halLock(mLock). So in HAL command thread, need avoid nested lock sequence
as halLock(mLock) -> libcameraLock.
Thus in this function, mLock protection is splitted to 2 sections(step 1 and step 3). It's not
a conventional way. But since after step 1, requestComplete() will not be trigged, not a big deal.
*/
void CameraDeviceSessionHwlImpl::WaitRequestsFinishAndCleanResource() {
    Mutex::Autolock _l(mLock);

    /*==== step 1: wait all the requests finished ====*/
    // For ov5640, destroy pipeline right after config/build pipeline.
    if ((mFrameBuffersFree.size() == 0) && (mFrameBuffersBusy.size() == 0)) {
        ALOGI("%s: no request submitted in session %p, just return", __func__, this);
        return;
    }

    /* If still have requests hold in libcamera, wait to finish */
    while (mDeQueRequestIdx != mInQueRequestIdx) {
        ALOGW("%s: still has requests to process, wait %d us, DeQueIdx %lu, InQueIdx %lu, session %p",
              __func__, WAIT_ITVL_US, mDeQueRequestIdx, mInQueRequestIdx, this);
        mLock.unlock();
        usleep(WAIT_ITVL_US);
        mLock.lock();
    }

    ALOGI("%s: requests, mDeQueRequestIdx %lu, mInQueRequestIdx %lu", __func__, mDeQueRequestIdx,
          mInQueRequestIdx);

    mInQueRequestIdx = 0;
    mDeQueRequestIdx = 0;

    if (mFrameBuffersFree.size() != mSensorData.mLibcameraBuffers)
        ALOGW("%s: !!! unexpected, mFrameBuffersFree size %lu != %d", __func__,
              mFrameBuffersFree.size(), mSensorData.mLibcameraBuffers);

    mLock.unlock();

    /*==== step 2: stop libcamera::camera ====*/
    mCamWrap->Stop();

    /*==== step 3: clean resource ====*/
    Mutex::Autolock _l2(mLock);
    /* free buffers in mFrameBuffersFree */
    CleanFrameBuffersLocked();

    // Should no buffers in mFrameBuffersBusy. Even so, don't free them since may cause exception.
    // It's the unexpected case, give warning and should refine code.
    if (!mFrameBuffersBusy.empty())
        ALOGW("%s: !!! unexpected, mFrameBuffersBusy size %lu != 0", __func__,
              mFrameBuffersBusy.size());

    mFrameBuffersFree.clear();
    mFrameBuffersBusy.clear();
    mFrameBufferHandleMap.clear();

    CleanRequestsLocked();

    return;
}

status_t CameraDeviceSessionHwlImpl::ConfigurePipeline(
        uint32_t physical_camera_id, HwlPipelineCallback hwl_pipeline_callback,
        const StreamConfiguration &request_config, const StreamConfiguration & /*overall_config*/,
        uint32_t *pipeline_id) {
    Mutex::Autolock _l(mLock);
    if (pipeline_id == nullptr) {
        ALOGE("%s pipeline_id is nullptr", __func__);
        return BAD_VALUE;
    }

    if (pipelines_built_) {
        ALOGE("%s Cannot configure pipelines after calling BuildPipelines()", __func__);
        return ALREADY_EXISTS;
    }

    bool bSupport =
            CameraDeviceHwlImpl::StreamCombJudge(request_config, mPreviewResolutions,
                                                 mPreviewResolutionCount, mPictureResolutions,
                                                 mPictureResolutionCount);

    if (bSupport == false) {
        ALOGI("%s: IsStreamCombinationSupported return false", __func__);
        return BAD_VALUE;
    }

    if ((physical_camera_id != camera_id_) && (physical_meta_map_.get() != nullptr)) {
        if (physical_meta_map_->find(physical_camera_id) == physical_meta_map_->end()) {
            ALOGE("%s: Camera: %d doesn't include physical device with id: %u", __FUNCTION__,
                  camera_id_, physical_camera_id);
            return BAD_VALUE;
        }
    }

    *pipeline_id = pipeline_id_;

    PipelineInfo *pipeline_info = (PipelineInfo *)calloc(1, sizeof(PipelineInfo));
    if (pipeline_info == NULL) {
        ALOGE("%s malloc pipeline_info failed", __func__);
        return BAD_VALUE;
    }

    pipeline_info->pipeline_id = pipeline_id_;
    pipeline_info->pipeline_callback = std::move(hwl_pipeline_callback);

    int ret = 0;
    maxStreamWidth = 0;
    maxStreamHeight = 0;
    bool pipeline_added_to_map = false;

    int stream_num = request_config.streams.size();
    if (stream_num == 0) {
        ALOGE("%s stream num 0", __func__);
        ret = BAD_VALUE;
        goto err_out;
    }

    pipeline_info->streams = new std::vector<Stream>();
    pipeline_info->hal_streams = new std::vector<HalStream>();

    if ((pipeline_info->streams == NULL) || (pipeline_info->hal_streams == NULL)) {
        ALOGE("%s: no memory, pipeline_info->streams %p, pipeline_info->hal_streams %p", __func__,
              pipeline_info->streams, pipeline_info->hal_streams);
        ret = BAD_VALUE;
        goto err_out;
    }

    pipeline_info->streams->assign(request_config.streams.begin(), request_config.streams.end());

    previewIdx = -1;
    stillcapIdx = -1;
    recordIdx = -1;
    callbackIdx = -1;
    cameraRWIdx = -1;
    rawIdx = -1;

    for (int i = 0; i < stream_num; i++) {
        Stream stream = request_config.streams[i];
        ALOGI("%s, stream %d: id %d, type %d, res %dx%d, format 0x%x, usage 0x%llx, space 0x%x, "
              "rot %d, is_phy %d, phy_id %d, size %d",
              __func__, i, stream.id, (int)stream.stream_type, stream.width, stream.height,
              stream.format, (unsigned long long)stream.usage, stream.data_space, (int)stream.rotation,
              stream.is_physical_camera_stream, stream.physical_camera_id, stream.buffer_size);

        uint32_t camera_id = camera_id_;
        camera_ids.push_back(camera_id);

        HalStream hal_stream;
        memset(&hal_stream, 0, sizeof(hal_stream));
        int usage = 0;

        if ((stream.width > maxStreamWidth) && (stream.height > maxStreamHeight)) {
            maxStreamWidth = stream.width;
            maxStreamHeight = stream.height;
        }

        switch (stream.format) {
            case HAL_PIXEL_FORMAT_RAW16:
                ALOGI("%s create raw stream", __func__);
                hal_stream.override_format = stream.format;
                hal_stream.max_buffers = NUM_CAPTURE_BUFFER;
                stream.usage |= GRALLOC_USAGE_SW_WRITE_OFTEN;
                rawIdx = i;
                break;
            case HAL_PIXEL_FORMAT_BLOB:
                ALOGI("%s create capture stream", __func__);
                hal_stream.override_format = stream.format;
                hal_stream.max_buffers = NUM_CAPTURE_BUFFER;
                // fix me, just for cpu jpeg encoder.
                stream.usage |= GRALLOC_USAGE_SW_WRITE_OFTEN;
                stillcapIdx = i;
                break;

            case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
                if (strcmp(mSensorData.v4l2_format, "nv12") == 0) {
                    ALOGI("HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, use nv12");
                    hal_stream.override_format = HAL_PIXEL_FORMAT_YCBCR_420_888;
                } else if (strcmp(mSensorData.v4l2_format, "rgb3") == 0) {
                    ALOGI("HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, use rgb3");
                    hal_stream.override_format = HAL_PIXEL_FORMAT_RGB_888;
                } else
                    hal_stream.override_format = HAL_PIXEL_FORMAT_YCBCR_422_I;

                hal_stream.max_buffers = mSensorData.mPreviewBuffers;
                usage = CAMERA_GRALLOC_USAGE;

                if (stream.usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) {
                    ALOGI("%s create video recording stream", __func__);
                    hal_stream.override_format = HAL_PIXEL_FORMAT_YCBCR_420_888;
                    recordIdx = i;
                } else if (stream.usage &
                           (GRALLOC_USAGE_HW_CAMERA_WRITE | GRALLOC_USAGE_HW_CAMERA_READ)) {
                    ALOGI("%s create camera rw stream", __func__);
                    cameraRWIdx = i;
                } else {
                    ALOGI("%s create preview stream", __func__);
                    previewIdx = i;
                }
                break;

            default:
                ALOGI("%s create callback stream", __func__);
                hal_stream.override_format = stream.format;
                hal_stream.max_buffers = mSensorData.mPreviewBuffers;
                usage = CAMERA_GRALLOC_USAGE;
                callbackIdx = i;
                break;
        }

        // May remove after refine mali gralloc
        stream.usage |= GRALLOC_USAGE_PRIVATE_3;

        // libimageprocess is used to convert v4l2 buffer to output buffer. It requests valid virt
        // addr. Since MALI GPU lock requests CPU usages, so add GRALLOC_USAGE_SW_WRITE_OFTEN.
        stream.usage |= GRALLOC_USAGE_SW_WRITE_OFTEN;

        if (stream.format != HAL_PIXEL_FORMAT_BLOB)
            stream.usage |= GRALLOC_USAGE_HW_CAMERA_WRITE;

        hal_stream.producer_usage = stream.usage | usage;

        if (hal_stream.producer_usage & GRALLOC_USAGE_HW_TEXTURE)
            hal_stream.producer_usage |= GRALLOC_USAGE_SW_READ_OFTEN;

        hal_stream.consumer_usage = 0;
        hal_stream.id = stream.id;
        hal_stream.override_data_space = stream.data_space;
        hal_stream.is_physical_camera_stream = stream.is_physical_camera_stream;
        hal_stream.physical_camera_id = stream.physical_camera_id;

        pipeline_info->hal_streams->push_back(std::move(hal_stream));
    }

    ALOGI("%s: pipeline_id_ %d, info %p, map_pipeline_info %p, this %p", __func__, pipeline_id_,
          pipeline_info, &map_pipeline_info, this);
    map_pipeline_info[pipeline_id_] = pipeline_info;
    pipeline_id_++;
    pipeline_added_to_map = true;

    if (m_bConfigLibcameraByIntent == false) {
        ret = PrepareLibcamera();
        if (ret) {
            ALOGE("%s: PrepareLibcamera failed", __func__);
            return ret;
        }

        // Create per-stream image process devices via factory
        auto &factory = fsl::ImageProcessFactory::getInstance();
        uint32_t srcW = mCamWrap->mStreamWidth;
        uint32_t srcH = mCamWrap->mStreamHeight;
        uint32_t srcFmt = mCamWrap->mHalFormat;

        for (int i = 0; i < stream_num; i++) {
            Stream &stream = pipeline_info->streams->at(i);
            HalStream &halStream = pipeline_info->hal_streams->at(i);
            uint32_t dstFmt = halStream.override_format;
            uint32_t dstW = stream.width;
            uint32_t dstH = stream.height;

            if (dstFmt == HAL_PIXEL_FORMAT_BLOB)
                dstFmt = srcFmt;

            auto device = factory.createDevice(srcFmt, dstFmt, srcW, srcH, dstW, dstH,
                                            mCamBlitCscType);
            mStreamDevices[stream.id] = device;
            ALOGI("%s: stream id %d (%dx%d fmt 0x%x -> %dx%d fmt 0x%x), device '%s'", __func__,
                stream.id, srcW, srcH, srcFmt, dstW, dstH, dstFmt,
                device ? device->getName() : "null");
        }
    }

    return OK;

err_out:
    if (pipeline_added_to_map) {
        pipeline_id_--;
        map_pipeline_info.erase(pipeline_id_);
    }
    if (pipeline_info->streams != nullptr) {
        delete pipeline_info->streams;
    }
    if (pipeline_info->hal_streams != nullptr) {
        delete pipeline_info->hal_streams;
    }
    free(pipeline_info);

    return ret;
}

static int GetVideoDeviceIndexByName(const char* name)
{
    char path[128];
    char buf[256];
    int index;
    FILE* fp;

    for (index = 0; index < 16; index++) {
        snprintf(path, sizeof(path), "/sys/class/video4linux/video%d/name", index);
        fp = fopen(path, "r");
        if (!fp)
            continue;

        if (fgets(buf, sizeof(buf), fp) != NULL) {
            if (strncmp(buf, name, strlen(name)) == 0) {
                fclose(fp);
                return index;
            }
        }
        fclose(fp);
    }

    return -1;
}

#define ISI_VIDEO_DEVICE_NAME "mxc_isi.0.capture"
#define V4L2_CID_ISI_RDC    (V4L2_CID_USER_BASE + 0x1001)
int CameraDeviceSessionHwlImpl::isi_rdc_commit(s_rdc_block_list &rdc_block_list) {
    int isi_video_index = -1;

    if (videoPath[0] != 0)
        goto rdc_commit;

    isi_video_index = GetVideoDeviceIndexByName(ISI_VIDEO_DEVICE_NAME);
    if (isi_video_index == -1) {
        ALOGE("%s: no video device %s", __func__, ISI_VIDEO_DEVICE_NAME);
        return -1;
    }

    snprintf(videoPath, sizeof(videoPath), "/dev/video%d", isi_video_index);

rdc_commit:
    int32_t fd = open(videoPath, O_RDWR);
    if (fd < 0) {
        ALOGE("%s: open %s failed", __func__, videoPath);
        return -1;
    }

    ALOGI("%s: open %s ok, fd %d", __func__, videoPath, fd);

    struct v4l2_ext_control ext_ctrl = {};
    ext_ctrl.id   = V4L2_CID_ISI_RDC;
    ext_ctrl.size = sizeof(s_rdc_block_list);
    ext_ctrl.ptr  = &rdc_block_list;

    struct v4l2_ext_controls ext_ctrls = {};
    ext_ctrls.ctrl_class = V4L2_CTRL_CLASS_USER;
    ext_ctrls.count      = 1;
    ext_ctrls.controls   = &ext_ctrl;

    int ret = ioctl(fd, VIDIOC_S_EXT_CTRLS, &ext_ctrls);
    if (ret == -1) {
        ALOGE("%s: VIDIOC_S_EXT_CTRLS for V4L2_CID_ISI_RDC failed, ret %d, %s",
              __func__, ret, strerror(errno));
        close(fd);
        return ret;
    }

    close(fd);
    ALOGI("%s: VIDIOC_S_CTRL ok", __func__);

    return 0;
}

status_t CameraDeviceSessionHwlImpl::AllocLibcameraBuffer() {
    int ret = 0;
    uint32_t bufferNum = mSensorData.mLibcameraBuffers;
    uint32_t i = 0;
    s_rdc_block_list rdc_block_list = {};

    for (i = 0; i < bufferNum; i++) {
        uint32_t bufferStride;
        ImxImageBuffer srcBuf;
        memset(&srcBuf, 0, sizeof(srcBuf));

        uint32_t allocWidth = mCamWrap->mStreamWidth ;
        bool bDoubleWidth = false;
        if (strstr(mSocType, "imx8qm") ||
            strstr(mSocType, "imx8qxp") || strstr(mSocType, "imx8mp")) {
            allocWidth = mCamWrap->mStreamWidth * 2;
            bDoubleWidth = true;
            ALOGI("%s: double mCamWrap->mStreamWidth from %u to %u", __func__, mCamWrap->mStreamWidth, allocWidth);
        }

        bool bGetPhyAddr = strstr(mSocType, "imx8mn") ? true : false;
        ret = AllocPhyBuffer(allocWidth, mCamWrap->mStreamHeight, mCamWrap->mHalFormat, srcBuf, true, bGetPhyAddr, false);
        if (ret) {
            ALOGE("%s: failed to allocate buffer:%d x %d, format=%x, ret=%d", __func__, allocWidth,
                  mCamWrap->mStreamHeight, mCamWrap->mHalFormat, ret);
            ret = BAD_VALUE;
            goto err_out;
        }

        // Double-width alloc leaves mWidth/mStride at 2x; recover both to the real stream width,
        // else the G2D preview blit reads at the doubled stride -> half-green frame.
        if (bDoubleWidth) {
            srcBuf.mWidth = mCamWrap->mStreamWidth;
            srcBuf.mStride = mCamWrap->mStreamWidth;
        }

        srcBuf.mFormatSize = getSizeByForamtRes(mCamWrap->mHalFormat, mCamWrap->mStreamWidth, mCamWrap->mStreamHeight, false);
        if (srcBuf.mFormatSize == 0)
            srcBuf.mFormatSize = srcBuf.mSize;

        std::unique_ptr<libcamera::FrameBuffer> frameBuffer =
                CreateFrameBuffer(srcBuf.buffer, mCamWrap->mLibCameraStream->configuration());
        if (frameBuffer == nullptr) {
            ALOGE("%s, CreateFrameBuffer faliled, hnd %p, index %d", __func__, srcBuf.buffer, i);
            ret = BAD_VALUE;
            goto err_out;
        }

        libcamera::FrameBuffer *pfb = frameBuffer.get();

        mFrameBufferHandleMap[pfb] = srcBuf;
        mFrameBuffersFree.push_back(std::move(frameBuffer));
        ALOGI("%s: mFrameBufferHandleMap[%p] %p, mFrameBuffersFree size %lu, this %p", __func__,
              pfb, srcBuf.buffer, mFrameBuffersFree.size(), this);
    }

    if (mFrameBufferHandleMap.size() != bufferNum) {
        ALOGE("%s: unexpected! mFrameBufferHandleMap size (%zu) != bufferNum (%u)", __func__, mFrameBufferHandleMap.size(), bufferNum);
        ret = BAD_VALUE;
        goto err_out;
    }


    if (strstr(mSocType, "imx8mn") == NULL)
        return 0;

    // On 8mn, pass v4l2 buffers to ISI driver
    if (bufferNum > MAX_ISI_RDC_BLOCK_US) {
        ALOGE("!!! %s: bufferNum(%u) > MAX_ISI_RDC_BLOCK_US(%u), the extra buffers will not be protected",
            __func__, bufferNum, MAX_ISI_RDC_BLOCK_US);
        ret = BAD_VALUE;
        goto err_out;
    }

    i = 0;
    for (auto &it : mFrameBufferHandleMap) {
        rdc_block_list.phyAddr[i] = it.second.mPhyAddr;
        rdc_block_list.size[i] = it.second.mFormatSize;
        ALOGI("%s: set rdc_block_list, index %u, phyAddr 0x%lx, size %u", __func__, i, rdc_block_list.phyAddr[i], rdc_block_list.size[i]);
        i++;
    }
    rdc_block_list.num = bufferNum;

    ret = isi_rdc_commit(rdc_block_list);
    if (ret) {
        ALOGE("%s: isi_rdc_commit failed, ret %d", __func__, ret);
        goto err_out;
    }

    return 0;

err_out:
    CleanFrameBuffersLocked();
    return ret;
}

status_t CameraDeviceSessionHwlImpl::PrepareLibcamera() {
    int ret = 0;

    ret = mCamWrap->Config(mSensorData.mLibcameraBuffers,
                                maxStreamWidth, maxStreamHeight);
    if (ret) {
        ALOGE("%s: mCamWrap->Config() failed, ret %d, buffers %d, format 0x%x, width %d, height %d",
              __func__, ret, mSensorData.mLibcameraBuffers, mCamWrap->mHalFormat,
              maxStreamWidth, maxStreamHeight);
        return ret;
    }

    ret = AllocLibcameraBuffer();
    if (ret) {
        ALOGE("%s: AllocLibcameraBuffer failed, ret %d", __func__, ret);
        return ret;
    }

    ret = mCamWrap->Start();
    if (ret) {
        ALOGE("%s: mCamWrap->Start() failed, ret %d", __func__, ret);
        CleanFrameBuffersLocked();
        return ret;
    }

    m_IspWrapper->m_SensorData->mExposureNsMin =
            mCamWrap->mExposureMinUs * NS_PER_US;
    m_IspWrapper->m_SensorData->mExposureNsMax =
            mCamWrap->mExposureMaxUs * NS_PER_US;

    mOmitFrames = 0;
    mOmitFrmCount = 0;

    struct OmitFrame *pOmitFrame = mSensorData.omit_frame;
    for (struct OmitFrame *item = pOmitFrame; item < pOmitFrame + OMIT_RESOLUTION_NUM; item++) {
        if ((mCamWrap->mStreamWidth == (uint32_t)item->width) && (mCamWrap->mStreamHeight == (uint32_t)item->height)) {
            mOmitFrmCount = (uint32_t)item->omitnum;
            ALOGI("%s, set omit frames %d for %ux%u", __func__, item->omitnum, mCamWrap->mStreamWidth,
                  mCamWrap->mStreamHeight);
            break;
        }
    }

    return 0;
}

status_t CameraDeviceSessionHwlImpl::GetConfiguredHalStream(
        uint32_t pipeline_id, std::vector<HalStream> *hal_streams) const {
    // fix me, build error as
    // error: 'this' argument to member function 'lock' has type 'const android::Mutex', but
    // function is not marked const Mutex::Autolock _l(mLock);

    if (hal_streams == nullptr) {
        ALOGE("%s hal_streams is nullptr", __func__);
        return BAD_VALUE;
    }

    if (!pipelines_built_) {
        ALOGE("%s No pipeline was built.", __func__);
        return NO_INIT;
    }

    auto iter = map_pipeline_info.find(pipeline_id);
    if (iter == map_pipeline_info.end()) {
        ALOGE("%s: Unknown pipeline ID: %u", __func__, pipeline_id);
        return NAME_NOT_FOUND;
    }

    bool found = false;
    for (auto it = map_pipeline_info.begin(); it != map_pipeline_info.end(); it++) {
        if (pipeline_id != it->first)
            continue;

        found = true;
        std::vector<HalStream> *streams = it->second->hal_streams;
        hal_streams->reserve(streams->size());
        hal_streams->assign(streams->begin(), streams->end());
        break;
    }

    return found ? OK : NAME_NOT_FOUND;
}

status_t CameraDeviceSessionHwlImpl::BuildPipelines() {
    ALOGI("enter %s", __func__);

    Mutex::Autolock _l(mLock);

    if (pipelines_built_) {
        ALOGE("%s Pipelines have already been built!", __func__);
        return ALREADY_EXISTS;
    } else if (map_pipeline_info.size() == 0) {
        ALOGE("%s No pipelines have been configured yet!", __func__);
        return NO_INIT;
    }

    pipelines_built_ = true;

    return OK;
}

void CameraDeviceSessionHwlImpl::DestroyPipelines() {
    ALOGI("enter %s", __func__);

    mLock.lock();
    if (!pipelines_built_) {
        // Not an error - nothing to destroy
        mLock.unlock();
        ALOGV("%s nothing to destroy", __func__);
        return;
    }
    mLock.unlock();

    // camera_->stop() (in command thead) will call d->pipe_->invokeMethod(&PipelineHandler::stop,
    // ConnectionTypeBlocking, this); PipelineHandler::stop() and requestComplete() is in same
    // thread(say v4l2 thread). To avoid dead lock, hold locks with same sequence: Lock for
    // ConnectionTypeBlocking, mLock.
    WaitRequestsFinishAndCleanResource();

    // Clear map_pipeline_info.
    // Since map_pipeline_info is changed in functions in command thread, should ok not protected by
    // mLock.
    for (auto it = map_pipeline_info.begin(); it != map_pipeline_info.end(); it++) {
        auto pInfo = it->second;
        if (pInfo == NULL) {
            ALOGW("%s, no pipeline info for id %u", __func__, it->first);
            continue;
        }

        if (pInfo->streams) {
            pInfo->streams->clear();
            delete pInfo->streams;
        }

        if (pInfo->hal_streams) {
            pInfo->hal_streams->clear();
            delete pInfo->hal_streams;
        }

        free(pInfo);
    }

    map_pipeline_info.clear();
    mStreamDevices.clear();

    pipelines_built_ = false;
}

#define MAX_PLAN 3
typedef struct tagPlanInfo {
    uint32_t size;
    uint32_t offset;
} PlanInfo;

typedef struct tagPlansInfo {
    uint32_t num;
    PlanInfo plans[MAX_PLAN];
} PlansInfo;

static uint32_t GetPlansInfo(const libcamera::StreamConfiguration &streamConfig,
                             PlansInfo &plansInfo) {
    uint32_t width = streamConfig.size.width;
    uint32_t height = streamConfig.size.height;

    switch (streamConfig.pixelFormat) {
        case libcamera::formats::NV12:
            plansInfo.num = 2;
            plansInfo.plans[0].offset = 0;
            plansInfo.plans[0].size = width * height;
            plansInfo.plans[1].offset = plansInfo.plans[0].size;
            plansInfo.plans[1].size = width * height / 2;
            break;
        case libcamera::formats::YUYV:
            plansInfo.num = 1;
            plansInfo.plans[0].offset = 0;
            plansInfo.plans[0].size = width * height * 2;
            break;
        case libcamera::formats::RGB888:
            plansInfo.num = 1;
            plansInfo.plans[0].offset = 0;
            plansInfo.plans[0].size = width * height * 3;
            break;
        default:
            ALOGE("%s: unsupported pixelFormat %s", __func__,
                  streamConfig.pixelFormat.toString().c_str());
            return BAD_VALUE;
    }

    return 0;
}

std::unique_ptr<libcamera::FrameBuffer> CameraDeviceSessionHwlImpl::CreateFrameBuffer(
        const buffer_handle_t hnd, const libcamera::StreamConfiguration &streamConfig) {
    int ret = 0;
    PlansInfo plansInfo;

    ret = GetPlansInfo(streamConfig, plansInfo);
    if (ret) {
        ALOGE("%s: GetPlansInfo failed", __func__);
        return nullptr;
    }

    uint32_t planNum = plansInfo.num;
    std::vector<libcamera::FrameBuffer::Plane> planes(planNum);
    for (size_t i = 0; i < planNum; ++i) {
        libcamera::SharedFD fd{hnd->data[0]};
        if (!fd.isValid()) {
            ALOGE("%s: No valid fd %d", __func__, hnd->data[i]);
            return nullptr;
        }

        planes[i].fd = fd;
        planes[i].offset = plansInfo.plans[i].offset;
        planes[i].length = plansInfo.plans[i].size;

        ALOGI("%s:, plan %lu, fd %d, offset %d, length %d", __func__, i, fd.get(), planes[i].offset,
              planes[i].length);
    }

    return std::make_unique<libcamera::FrameBuffer>(std::move(planes));
}

Stream *CameraDeviceSessionHwlImpl::GetStreamById(int32_t stream_id, PipelineInfo *pInfo) {
    if (pInfo == NULL)
        return NULL;

    uint32_t stream_num = pInfo->streams->size();
    for (uint32_t i = 0; i < stream_num; i++) {
        if (pInfo->streams->at(i).id == stream_id)
            return &pInfo->streams->at(i);
    }

    return NULL;
}

int CameraDeviceSessionHwlImpl::PickConfigStreamLocked(uint32_t pipeline_id, uint8_t intent) {
    PipelineInfo *pipeline_info = GetPipelineInfo(pipeline_id);
    if (pipeline_info == NULL) {
        ALOGE("%s: Unexpected, pipeline %d is invalid", __func__, pipeline_id);
        return -1;
    }

    if ((pipeline_info == NULL) || (pipeline_info->streams == NULL)) {
        ALOGE("%s: pipeline_info or streams is NULL for id %d", __func__, pipeline_id);
        return -1;
    }

    ALOGI("%s: previewIdx %d, callbackIdx %d, stillcapIdx %d, recordIdx %d, cameraRWIdx %d, rawIdx %d, intent %d",
          __func__, previewIdx, callbackIdx, stillcapIdx, recordIdx, cameraRWIdx, rawIdx, intent);

    int configIdx = -1;
    if (intent == ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE) {
        if (rawIdx >= 0) {
            configIdx = rawIdx;
        } else {
            configIdx = stillcapIdx;
        }
    }

    if (configIdx == -1) {
        if (previewIdx >= 0)
            configIdx = previewIdx;
        else if (callbackIdx >= 0)
            configIdx = callbackIdx;
        else if (rawIdx >= 0)
            configIdx = rawIdx;
        else if (stillcapIdx >= 0)
            configIdx = stillcapIdx;
        else if (recordIdx >= 0)
            configIdx = recordIdx;
        else if (cameraRWIdx >= 0)
            configIdx = cameraRWIdx;
        else {
            ALOGE("%s, no stream found to config v4l2", __func__);
            return -1;
        }
    }

    ALOGI("choose stream index %d as config stream", configIdx);

    return configIdx;
}

status_t CameraDeviceSessionHwlImpl::PickAndConfigLibcamera(HwlPipelineRequest &request) {
    if (request.output_buffers.empty() && request.input_buffers.empty()) {
        ALOGW("%s: unexpected request with no buffers", __func__);
        return BAD_VALUE;
    }

    HwlPipelineRequest &hwReq = request;
    uint32_t pipeline_id = hwReq.pipeline_id;

    Mutex::Autolock _l(mLock);
    PipelineInfo *pipeline_info = GetPipelineInfo(pipeline_id);
    if (pipeline_info == NULL) {
        ALOGW("%s: Unexpected, pipeline %d is invalid", __func__, pipeline_id);
        return BAD_VALUE;
    }

    uint8_t captureIntent = mCaptureIntent;
    int ret = 0;
    int configIdx = -1;
    bool bResChange = false;
    camera_metadata_ro_entry entry;
    uint32_t pickedWidth = mMaxWidth;
    uint32_t pickedHeight = mMaxHeight;

    if (hwReq.settings == NULL) {
        if (!mCamWrap->IsRunning()) {
            ALOGW("!!! %s: unexpected, 1st request should have settings", __func__);
            goto config;
        }

        return OK;
    }

    ret = hwReq.settings->Get(ANDROID_CONTROL_CAPTURE_INTENT, &entry);
    if (ret == 0) {
        captureIntent = entry.data.u8[0];
        ALOGI("%s: captureIntent %d", __func__, captureIntent);
        configIdx = PickConfigStreamLocked(pipeline_id, captureIntent);
    }

    if (configIdx >= 0) {
        pickedWidth = pipeline_info->streams->at(configIdx).width;
        pickedHeight = pipeline_info->streams->at(configIdx).height;

        if ((pickedWidth != mCamWrap->mStreamWidth) ||
            (pickedHeight != mCamWrap->mStreamHeight)) {
            ALOGI("%s: res changed from %dx%d to %dx%d", __func__, mCamWrap->mStreamWidth,
                  mCamWrap->mStreamHeight, pickedWidth, pickedHeight);
            bResChange = true;
        }
    }

    if ((!mCamWrap->IsRunning()) || bResChange)
        goto config;

    return OK;

    // If never configured or w/h changed, call ConfigLibcameraLocked()
config:

    ALOGI("%s: libcamera IsRunning %d, bResChange %d", __func__, mCamWrap->IsRunning(), bResChange);
    mLock.unlock();
    if (mCamWrap->IsRunning()) {
        WaitRequestsFinishAndCleanResource();
    }

    Mutex::Autolock _l2(mLock);

    ret = PrepareLibcamera();
    if (ret) {
        ALOGE("%s: PrepareLibcamera failed", __func__);
        return ret;
    }

    return OK;
}

void CameraDeviceSessionHwlImpl::ISPProcess(HalCameraMetadata *cameraMeta,
                                            libcamera::Request *request) {
    libcamera::ControlList &controls = request->controls();
    // sequence and controls.size are always 0, that is, the control commands received from
    // HwlPipelineRequest
    if (mDebug)
        ALOGI("%s: sequence %d, controls size %zu", __func__, request->sequence(), controls.size());
    m_IspWrapper->process(cameraMeta, controls);

    if (mDeQueRequestIdx == 1) {
        // Manual exposure mode, for ExposureGain, process after stream on
        if (m_IspWrapper->m_ae_mode == ANDROID_CONTROL_AE_MODE_ON) {
            m_IspWrapper->processAeMode(ANDROID_CONTROL_AE_MODE_ON, controls, true);
        } else {
            m_IspWrapper->processExposureGain(m_IspWrapper->m_exposure_gain, controls, true);
        }
    }
    return;
}

status_t CameraDeviceSessionHwlImpl::queueRequestToLibcameraLocked(HalCameraMetadata *cameraMeta) {
    std::shared_ptr<libcamera::Camera> camera_ = mCamWrap->camera_;
    uint32_t waitMs = 0;
    while (mFrameBuffersFree.empty()) {
        // unlock, so the in requestComplete() thread, has chance to call
        // mFrameBuffersFree.push_back().
        mLock.unlock();
        ALOGW("%s: mFrameBuffersFree empty, wait %d ms", __func__, WAIT_ITVL_MS);
        usleep(WAIT_ITVL_US);
        waitMs += WAIT_ITVL_MS;
        mLock.lock();
        if (waitMs > 1000) {
            ALOGE("%s: mFrameBuffersFree still empty, wait 1000ms", __func__);
            return BAD_VALUE;
        }
    }

    std::unique_ptr<libcamera::FrameBuffer> uptrFrameBuffer = std::move(mFrameBuffersFree.front());
    libcamera::FrameBuffer *frameBuffer = uptrFrameBuffer.get();
    mFrameBuffersFree.pop_front();
    mFrameBuffersBusy.push_back(std::move(uptrFrameBuffer));
    if (mDebug)
        ALOGI("%s: mFrameBuffersFree size %lu, mFrameBuffersBusy size %lu", __func__,
              mFrameBuffersFree.size(), mFrameBuffersBusy.size());

    std::unique_ptr<libcamera::Request> requestUptr = camera_->createRequest();
    int ret = requestUptr->addBuffer(mCamWrap->mLibCameraStream, frameBuffer);
    if (ret) {
        ALOGE("%s, request->addBuffer failed, ret %d", __func__, ret);
        return ret;
    }

    if (strstr(mSocType, "imx95"))
        ISPProcess(cameraMeta, requestUptr.get());

    ret = camera_->queueRequest(requestUptr.get());
    if (ret) {
        ALOGE("%s, camera_->queueRequest failed, ret %d", __func__, ret);
        return ret;
    }

    requestList.push_back(std::move(requestUptr));
    return 0;
}

status_t CameraDeviceSessionHwlImpl::SubmitRequest(HwlPipelineRequest request) {
    char value[PROPERTY_VALUE_MAX];
    property_get("vendor.rw.camera.test", value, "");
    mDebug = (strcmp(value, "debug") == 0) ? true : false;
    mZoomInByProp = (strcmp(value, "zoomin") == 0) ? true : false;

    if (mDebug || (request.frame_number < DEBUG_FRAME_NUM)) {
        ALOGI("enter %s: session %p, requestList size %zu, frame_number %u",
              __func__, this, requestList.size(), request.frame_number);
    }

    int ret = 0;

    // Although m_bConfigLibcameraByIntent is hard code to false,
    // keep the logic, so can easily switch to it if need.
    if (m_bConfigLibcameraByIntent) {
        ret = PickAndConfigLibcamera(request);
        if (ret) {
            ALOGE("%s: PickAndConfigLibcamzra failed, ret %d", __func__, ret);
            return ret;
        }
    }

    FrameRequest *frame_request = new FrameRequest();
    std::list<std::unique_ptr<libcamera::FrameBuffer>> frameBuffers;

    Mutex::Autolock _l(mLock);
    uint32_t pipeline_id = request.pipeline_id;

    if (mDebug)
        ALOGI("%s, frame_number %d, pipeline_id %d, outbuffer num %d", __func__, request.frame_number,
                (int)pipeline_id, (int)request.output_buffers.size());

    PipelineInfo *pInfo = GetPipelineInfo(pipeline_id);
    if (pInfo == NULL) {
        ALOGE("%s: Unexpected, pipeline %d is invalid", __func__, pipeline_id);
        return BAD_VALUE;
    }

    frame_request->frame_number = request.frame_number;
    frame_request->hwlReq.pipeline_id = pipeline_id;
    frame_request->hwlReq.frame_number = request.frame_number;
    frame_request->hwlReq.settings = HalCameraMetadata::Clone(request.settings.get());
    frame_request->hwlReq.output_buffers.reserve(request.output_buffers.size());
    frame_request->hwlReq.output_buffers.assign(request.output_buffers.begin(),
                                                        request.output_buffers.end());

    frame_request->hwlReq.input_buffers.reserve(request.input_buffers.size());
    frame_request->hwlReq.input_buffers.assign(request.input_buffers.begin(),
                                                        request.input_buffers.end());
    frame_request->hwlReq.input_buffer_metadata.reserve(
            request.input_buffer_metadata.size());

    frame_request->camera_ids.reserve(camera_ids.size());
    frame_request->camera_ids.assign(camera_ids.begin(), camera_ids.end());

    // Record fence fd
    uint32_t outBufNum = request.output_buffers.size();
    frame_request->outBufferFences.resize(outBufNum);
    for (int j = 0; j < (int)outBufNum; j++) {
        FenceFdInfo fenceInfo = {-1, -1};
        fenceInfo.acquire_fence_fd =
                importFence(request.output_buffers[j].acquire_fence, mDebug);
        ALOGV("%s, acquire_fence_fd %d", __func__, fenceInfo.acquire_fence_fd);
        frame_request->outBufferFences[j] = fenceInfo;
        frame_request->hwlReq.output_buffers[j].acquire_fence = NULL;
        frame_request->hwlReq.output_buffers[j].release_fence = NULL;
    }

queue_request:
    ret = queueRequestToLibcameraLocked(request.settings.get());
    if (ret) {
        ALOGE("%s: queueRequestToLibcameraLocked failed, ret %d", __func__, ret);
        return ret;
    }

    list_frame_request.push_back(frame_request);
    mInQueRequestIdx++;

    if (mDebug || (request.frame_number < DEBUG_FRAME_NUM)) {
        DumpRequest();
        ALOGI("leave %s: session %p, frame_number %u, mInQueRequestIdx %lu, mDeQueRequestIdx %lu",
              __func__, this, request.frame_number, mInQueRequestIdx, mDeQueRequestIdx);
        ItvlStat(mPreSubmitRequestTime, (char *)"SubmitRequests");
    }

    return OK;
}

status_t CameraDeviceSessionHwlImpl::SubmitBatchRequest(std::vector<HwlPipelineRequest> requests) {
    for (auto &request : requests) {
        status_t res = SubmitRequest(std::move(request));
        if (res != OK) {
            ALOGE("%s: SubmitRequest failed in batch, ret %d", __func__, res);
            return res;
        }
    }

    return OK;
}

int CameraDeviceSessionHwlImpl::CleanRequestsLocked() {
    if (list_frame_request.empty())
        return 0;

    for (auto it = list_frame_request.begin(); it != list_frame_request.end(); it++) {
        FrameRequest *request = *it;
        if (request == NULL) {
            ALOGW("%s: request is null", __func__);
            continue;
        }

        ALOGV("%s, list_frame_request, frame %u", __func__, request->frame_number);
        ReleaseFrameRequest(*request);
        delete request;
    }

    list_frame_request.clear();

    return 0;
}

status_t CameraDeviceSessionHwlImpl::Flush() {
    // TODO need refine for multi camera??
    return OK;
}

uint32_t CameraDeviceSessionHwlImpl::GetCameraId() const {
    return camera_id_;
}

std::vector<uint32_t> CameraDeviceSessionHwlImpl::GetPhysicalCameraIds() const {
    if ((physical_meta_map_->empty())) {
        ALOGV("%s: GetPhysicalCameraIds is empty", __func__);
        return std::vector<uint32_t>{};
    }

    if ((physical_meta_map_.get() == nullptr)) {
        ALOGW("%s: GetPhysicalCameraIds is null", __func__);
        return std::vector<uint32_t>{};
    }
    std::vector<uint32_t> ret;
    ret.reserve(physical_meta_map_->size());
    for (const auto &it : *physical_meta_map_) {
        ret.push_back(it.first);
    }

    return ret;
}

status_t CameraDeviceSessionHwlImpl::GetCameraCharacteristics(
        std::unique_ptr<HalCameraMetadata> *characteristics) const {
    if (characteristics == nullptr) {
        return BAD_VALUE;
    }

    (*characteristics) = HalCameraMetadata::Clone(static_metadata_.get());
    if (*characteristics == nullptr) {
        ALOGE("%s metadata clone failed", __func__);
        return NO_MEMORY;
    }

    return OK;
}

status_t CameraDeviceSessionHwlImpl::GetPhysicalCameraCharacteristics(
        uint32_t physical_camera_id, std::unique_ptr<HalCameraMetadata> *characteristics) const {
    if (characteristics == nullptr) {
        return BAD_VALUE;
    }

    if (physical_meta_map_.get() == nullptr) {
        ALOGE("%s: Camera: %d doesn't have physical device support!", __FUNCTION__, camera_id_);
        return BAD_VALUE;
    }

    if (physical_meta_map_->find(physical_camera_id) == physical_meta_map_->end()) {
        ALOGE("%s: Camera: %d doesn't include physical device with id: %u", __FUNCTION__,
              camera_id_, physical_camera_id);
        return BAD_VALUE;
    }

    *characteristics = HalCameraMetadata::Clone((physical_meta_map_->at(physical_camera_id)).get());
    return OK;
}

status_t CameraDeviceSessionHwlImpl::ConstructDefaultRequestSettings(
        RequestTemplate type, std::unique_ptr<HalCameraMetadata> *default_settings) {
    Mutex::Autolock _l(mLock);

    return m_meta->getRequestSettings(type, default_settings);
}

status_t CameraDeviceSessionHwlImpl::ProcessCapbuf2Outbuf(ImxStreamBuffer *srcBuf,
                                                          StreamBuffer &output_buffers,
                                                          FenceFdInfo &outFences,
                                                          CameraMetadata &requestMeta) {
    int ret = 0;
    if (srcBuf == NULL)
        return BAD_VALUE;

    StreamBuffer *it = &output_buffers;

    // If fence is valid, wait.  Ref the usage in EmulatedRequestProcessor.cpp.
    int acquire_fence_fd = outFences.acquire_fence_fd;
    if (acquire_fence_fd > -1) {
        ALOGV("%s, before sync_wait fence fd %d", __func__, acquire_fence_fd);
        ret = sync_wait(acquire_fence_fd, CAMERA_SYNC_TIMEOUT);
        ALOGV("%s, after sync_wait fence fd %d", __func__, acquire_fence_fd);
        closeFence(acquire_fence_fd, mDebug);
        outFences.acquire_fence_fd = -1;
        if (ret != OK) {
            ALOGW("%s: Timeout waiting on acquire fence %d, on stream %d, buffer %lu", __func__,
                  acquire_fence_fd, it->stream_id, it->buffer_id);
        }
    }

    Stream *pStream = GetStreamFromStreamBuffer(it);
    if (pStream == NULL) {
        ALOGE("%s, dst buf belong to stream %d, but the stream is not configured", __func__,
              it->stream_id);
        return BAD_VALUE;
    }

    ImxStreamBuffer *dstBuf = CreateImxStreamBufferFromBufferHandle(it->buffer, pStream);
    if (dstBuf == NULL)
        return BAD_VALUE;

    uint64_t t1 = systemTime();

    if (dstBuf->mStream->format() == HAL_PIXEL_FORMAT_BLOB) {
        mJpegBuilder->reset();
        mJpegBuilder->setMetadata(&requestMeta);
        processJpegBuffer(srcBuf, dstBuf, &requestMeta);
    } else {
        processFrameBuffer(srcBuf, dstBuf, &requestMeta);
    }

    uint64_t t2 = systemTime();

    if (mDebug) {
        ALOGI("%s: use %lu ms, src: virt %p, phy 0x%lx, size %dx%d, format 0x%x, dst: virt %p, phy "
              "0x%lx, size %dx%d, format 0x%x",
              __func__, (t2 - t1) / 1000000, srcBuf->mVirtAddr, srcBuf->mPhyAddr,
              srcBuf->mStream->width(), srcBuf->mStream->height(), srcBuf->mStream->format(),
              dstBuf->mVirtAddr, dstBuf->mPhyAddr, dstBuf->mStream->width(),
              dstBuf->mStream->height(), dstBuf->mStream->format());
    }

    DumpStream(srcBuf, dstBuf);

    ReleaseImxStreamBuffer(dstBuf);
    return 0;
}

status_t CameraDeviceSessionHwlImpl::ProcessCapbuf2MultiOutbuf(
        ImxStreamBuffer *srcBuf, std::vector<StreamBuffer> &output_buffers,
        std::vector<FenceFdInfo> &outFences, CameraMetadata &requestMeta) {
    int ret = 0;

    if (srcBuf == NULL)
        return BAD_VALUE;

    int outBufSize = output_buffers.size();
    int outFencesSize = outFences.size();

    if (outBufSize != outFencesSize) {
        ALOGE("%s: outBufSize(%d) != outFencesSize(%d)", __func__, outBufSize, outFencesSize);
        return BAD_VALUE;
    }

    for (int i = 0; i < outBufSize; i++) {
        ImxStreamBuffer *srcBufTmp = srcBuf;
        // If there are requirements for two streams at the same time, and one is for record
        // use preview buffer as srcBuf to save processing. stream as follows:
        // preview stream: 2160p/yuyv(vl42 buffer)    -> 1080p/yuyv(preview buffer)
        // record stream:  1080p/yuyv(preview buffer) -> 1080p/nv12(record buffer)
        if (outBufSize == 2) {
            Stream *pFirstStream = GetStreamFromStreamBuffer(&output_buffers[0]);
            Stream *pSecondStream = GetStreamFromStreamBuffer(&output_buffers[1]);
            // found if there's same res/fmt processed output buffer.
            if ((pFirstStream == NULL) || (pSecondStream == NULL)) {
                ALOGE("%s: unexpected, pFirstStream %p, pSecondStream %p", __func__, pFirstStream, pSecondStream);
                return BAD_VALUE;
            }
            if ((pSecondStream->width == pFirstStream->width) &&
                (pSecondStream->height == pFirstStream->height) &&
                (pSecondStream->format == pFirstStream->format)) {
                if(pFirstStream->usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) {
                    if (mDebug)
                        ALOGI("%s: 1th stream required for record, swap 2 output_buffers, handle the preview stream first", __func__);
                    std::swap(output_buffers[0], output_buffers[1]);
                }

                if (i == 1) {
                    srcBufTmp = CreateImxStreamBufferFromBufferHandle(output_buffers[0].buffer, pFirstStream);
                }

            }
        }

        int status = ProcessCapbuf2Outbuf(srcBufTmp, output_buffers[i], outFences[i], requestMeta);
        if (status)
            ret = BAD_VALUE;

        if (srcBufTmp != srcBuf)
            ReleaseImxStreamBuffer(srcBufTmp);
    }

    return ret;
}

uint64_t CameraDeviceSessionHwlImpl::GetTimestampLocked(libcamera::Request *request) {
    libcamera::Request::BufferMap bufMap = request->buffers();
    for (auto &t : bufMap) {
        libcamera::FrameBuffer *frameBuffer = t.second;
        if (frameBuffer)
            return frameBuffer->metadata().timestamp;
    }

    ALOGW("!!! %s: unexpected, no valid frameBuffer in bufMap, size %lu", __func__, bufMap.size());

    return systemTime(SYSTEM_TIME_MONOTONIC);
}

void CameraDeviceSessionHwlImpl::ReturnFrameBufferLocked() {
    std::unique_ptr<libcamera::FrameBuffer> uptrFrameBuffer = std::move(mFrameBuffersBusy.front());
    libcamera::FrameBuffer *frameBufferFront = uptrFrameBuffer.get();
    mFrameBuffersBusy.pop_front();
    mFrameBuffersFree.push_back(std::move(uptrFrameBuffer));

    if (mDebug)
        ALOGI("%s: mFrameBuffersFree size %lu, mFrameBuffersBusy size %lu", __func__,
              mFrameBuffersFree.size(), mFrameBuffersBusy.size());

    return;
}

void CameraDeviceSessionHwlImpl::requestCompleteDispatch(libcamera::Request *request) {
    if (request == NULL)
        return;

    if (mDebug || (request->sequence() < DEBUG_FRAME_NUM))
        ALOGI("%s: dispatch session %p to task %p, tid %d, sequence %d", __func__, this,
              sessionThreadMap[this].get(), sessionThreadMap[this]->getTid(), request->sequence());

    Mutex::Autolock _l(mRequestPendingListLock);
    mRequestPendingList.push_back(request);
    mRequestPendingListCond.signal();
    return;
}

int CameraDeviceSessionHwlImpl::HandleImage() {
    mRequestPendingListLock.lock();
    while (mRequestPendingList.empty()) {
        mRequestPendingListCond.waitRelative(mRequestPendingListLock, WAIT_TIME_OUT);
        if (mRequestPendingList.empty()) {
            if (mDebug)
                ALOGW("%s: mRequestPendingList still empty after %lld ns", __func__, WAIT_TIME_OUT);
            mRequestPendingListLock.unlock();
            return 0;
        }
    }

    libcamera::Request *request = mRequestPendingList.front();
    mRequestPendingList.pop_front();
    mRequestPendingListLock.unlock();

    requestComplete(request);

    return 0;
}

void CameraDeviceSessionHwlImpl::requestComplete(libcamera::Request *request) {
    if (request == NULL) {
        ALOGE("%s: request NULL", __func__);
        return;
    }

    if (mDebug || (request->sequence() < DEBUG_FRAME_NUM))
        ALOGI("enter %s: session %p, sequence %u", __func__, this, request->sequence());

    Mutex::Autolock _l(mLock);

    std::unique_ptr<libcamera::Request> requestUptr = std::move(requestList.front());
    requestList.pop_front();
    if (requestUptr.get() != request)
        ALOGW("%s: !!! requestUptr.get() (%p) != request (%p)", __func__, requestUptr.get(),
              request);

    if (mOmitFrames < mOmitFrmCount) {
        mOmitFrames++;
        ALOGI("%s: omit frames %u", __func__, mOmitFrames);
        ReturnFrameBufferLocked();
        queueRequestToLibcameraLocked(NULL);
        return;
    }

    if (list_frame_request.size() == 0) {
        ALOGE("%s: unexpect list_frame_request.size() 0", __func__);
        return;
    }

    // currently 1 frame map 1 request
    FrameRequest *frameRequest = *(list_frame_request.begin());
    if (frameRequest == NULL) {
        ALOGE("%s: frameRequest NULL", __func__);
        return;
    }
    uint32_t frame = frameRequest->frame_number;

    HwlPipelineRequest *hwReq = &frameRequest->hwlReq;
    if (hwReq == NULL) {
        ALOGE("%s: hwReq NULL", __func__);
        return;
    }

    // save the latest meta
    if (hwReq->settings != NULL) {
        if (mSettings != NULL)
            mSettings.reset();

        mSettings = HalCameraMetadata::Clone(hwReq->settings.get());
    }

    uint32_t pipeline_id = hwReq->pipeline_id;
    PipelineInfo *pInfo = GetPipelineInfo(pipeline_id);
    if (pInfo == NULL) {
        ALOGE("%s: Unexpected, pipeline %d is invalid", __func__, pipeline_id);
        return;
    }

    // notify shutter
    uint64_t readout_timestamp_ns = GetTimestampLocked(request);
    uint64_t timestamp_ns = readout_timestamp_ns - 33333333;
    if (pInfo->pipeline_callback.notify) {
        NotifyMessage msg = ShutterMessage {.frame_number = frame,
                                            .timestamp_ns = timestamp_ns,
                                            .readout_timestamp_ns = readout_timestamp_ns};

        pInfo->pipeline_callback.notify(pipeline_id, msg);
    }

    // clone latest meta to result->result_metadata
    auto result = std::make_unique<HwlPipelineResult>();
    if (mSettings != NULL)
        result->result_metadata = HalCameraMetadata::Clone(mSettings.get());
    else
        result->result_metadata = HalCameraMetadata::Create(1, 10);

    CameraMetadata requestMeta(result->result_metadata.get());

    // construct result
    result->camera_id = camera_id_;
    result->pipeline_id = pipeline_id;
    result->frame_number = frame;
    result->partial_result = 1;

    result->output_buffers.assign(hwReq->output_buffers.begin(), hwReq->output_buffers.end());
    result->input_buffers.reserve(0);
    result->physical_camera_results.reserve(0);

    libcamera::FrameBuffer *frameBuffer = request->findBuffer(mCamWrap->mLibCameraStream);
    ImxImageBuffer srcImgBuf = mFrameBufferHandleMap[frameBuffer];

    ImxStreamBuffer srcBuf;
    memcpy(&srcBuf, &srcImgBuf, sizeof(srcImgBuf));
    srcBuf.mStream = new ImxStream(mCamWrap->mStreamWidth, mCamWrap->mStreamHeight,
                                   mCamWrap->mHalFormat, srcBuf.mUsage, 0, false);
    ALOGV("srcBuf %dx%d, 0x%x", srcBuf.mWidth, srcBuf.mHeight, srcBuf.mFormat);

    if (mCamBlitCscType == ENG_G2D) {
        camera_metadata_ro_entry entry;
        int ret = requestMeta.Get(ANDROID_CONTROL_ZOOM_RATIO, &entry);
        if ((ret == 0) && srcBuf.mStream)
            srcBuf.mStream->mZoomRatio = entry.data.f[0];

        if (mZoomInByProp && srcBuf.mStream)
            srcBuf.mStream->mZoomRatio = 2.0;
    }

    if (!mSensorData.mNeedDewarp) {
        ProcessCapbuf2MultiOutbuf(&srcBuf, hwReq->output_buffers, frameRequest->outBufferFences,
                                  requestMeta);
    } else {
        ImxStreamBuffer dewarpStreamBuf;
        memcpy(&dewarpStreamBuf, &mDewarpBuf, sizeof(mDewarpBuf));
        dewarpStreamBuf.mStream =
                new ImxStream(mCamWrap->mStreamWidth, mCamWrap->mStreamHeight,
                              mCamWrap->mHalFormat, mDewarpBuf.mUsage, 0, false);

        srcBuf.mDewarp = true;
        convertImage(mDewarpDevice, dewarpStreamBuf, srcBuf);
        ProcessCapbuf2MultiOutbuf(&dewarpStreamBuf, hwReq->output_buffers,
                                  frameRequest->outBufferFences, requestMeta);

        delete dewarpStreamBuf.mStream;
    }

    delete srcBuf.mStream;

    HandleMetaLocked(result->result_metadata, timestamp_ns);

    // call back to process result
    if (pInfo->pipeline_callback.process_pipeline_result) {
        pInfo->pipeline_callback.process_pipeline_result(std::move(result));
    }

    if (mDebug)
        ItvlStat(mPreHandleImageTime, (char *)"requestComplete(), process_pipeline_result");

    // erase the item in list_frame_request.
    list_frame_request.pop_front();
    delete frameRequest;

    ReturnFrameBufferLocked();

    mDeQueRequestIdx++;
    if (mDebug || (request->sequence() < DEBUG_FRAME_NUM))
        ALOGI("leave %s: session %p, sequence %u, mInQueRequestIdx %lu, mDeQueRequestIdx %lu", __func__, this,
              request->sequence(), mInQueRequestIdx, mDeQueRequestIdx);

    return;
}

void CameraDeviceSessionHwlImpl::RepeatingRequestEnd(
    int32_t /*frame_number*/, const std::vector<int32_t>& /*stream_ids*/) {
}

} // namespace android
