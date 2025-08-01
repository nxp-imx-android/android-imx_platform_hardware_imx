/*
 *  Copyright 2025 NXP
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
 *
 */
#ifndef _OPENCL_CONVERTER_H_
#define _OPENCL_CONVERTER_H_

#include "imx_opencl_converter.h"

#define OCL_LIB_NAME "lib_imx_opencl_converter.so"

namespace aidl::android::hardware::graphics::composer3::impl {

struct G2dBuffer;

class OclConverter {
public:
    OclConverter();
    ~OclConverter();

    bool isValid();
    int openclConvert(G2dBuffer& srcBuf, G2dBuffer& dstBuf);

private:
    static thread_local OCL_HANDLE sHandle;
    void* mLibHandle;

    void* getHandle();
    void G2dBufferToOclFormat(G2dBuffer& buff, OCL_FORMAT& oclFormat);
    void G2dBufferToOclBuffer(G2dBuffer& buff, OCL_BUFFER& oclBuf, OCL_FORMAT& oclFmt);

    typedef OCL_RESULT (*tOCL_Open)(OCL_OPEN_FLAG flag, OCL_HANDLE* handle);
    typedef OCL_RESULT (*tOCL_Close)(OCL_HANDLE handle);
    typedef OCL_RESULT (*tOCL_SetParam)(OCL_HANDLE handle, OCL_PARAM_INDEX index, void* param);
    typedef OCL_RESULT (*tOCL_GetParam)(OCL_HANDLE handle, OCL_PARAM_INDEX index, void* param);
    typedef OCL_RESULT (*tOCL_Convert)(OCL_HANDLE handle, OCL_BUFFER* in_buf, OCL_BUFFER* out_buf);

    tOCL_Open mOpen;
    tOCL_Close mClose;
    tOCL_SetParam mSetParam;
    tOCL_SetParam mGetParam;
    tOCL_Convert mConvert;

    OCL_MEMORY_TYPE mOclBufferType;
};
} // namespace aidl::android::hardware::graphics::composer3::impl
#endif
