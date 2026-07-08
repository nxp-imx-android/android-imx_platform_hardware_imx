/*
 * Copyright 2026 NXP.
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

#include "ImageProcessDevice.h"

#include <cutils/native_handle.h>
#include <hardware/gralloc.h>

#include "gralloc_handle.h"

namespace fsl {

buffer_handle_t ImageProcessDevice::createBufferHandle(ImxImageBuffer& imxBuf) {
    void* mem = native_handle_create(GRALLOC_HANDLE_NUM_FDS, GRALLOC_HANDLE_NUM_INTS);
    if (mem == nullptr) {
        return nullptr;
    }

    buffer_handle_t handle =
            new (mem) gralloc_handle(imxBuf.mFd, imxBuf.mSize, 0, HAL_PIXEL_FORMAT_RGBA_8888,
                                     imxBuf.mSize / 4, 1, 1, imxBuf.mSize / 4);
    gralloc_handle_set_phys(handle, imxBuf.mPhyAddr);
    gralloc_handle_set_base(handle, (uint64_t)imxBuf.mVirtAddr);

    return handle;
}

void ImageProcessDevice::destroyBufferHandle(buffer_handle_t buffer) {
    if (buffer)
        free((void*)buffer);
}

} // namespace fsl
