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

#include <string.h>
#include <errno.h>
#include <string.h>
#include <linux/videodev2.h>
#include <log/log.h>
#include <utils/Errors.h>
#include "Metadata.h"
#include "ISPWrapper.h"

#define VIV_CTRL_NAME "viv_ext_ctrl"

ISPWrapper::ISPWrapper()
{
    m_fd = -1;
    m_ctrl_id = 0;
    m_awb_mode = ANDROID_CONTROL_AWB_MODE_AUTO;
}

ISPWrapper::~ISPWrapper()
{
    if(m_fd > 0)
        close(m_fd);
}

int ISPWrapper::init(char *devPath)
{
    if(devPath == NULL)
        return BAD_VALUE;

    // already inited
    if(m_ctrl_id > 0)
        return 0;

    int fd = open(devPath, O_RDWR);
    if (fd < 0) {
        ALOGE("%s: open %s failed", __func__, devPath);
        return BAD_VALUE;
    }

    m_fd = fd;

    // get viv ctrl id by it's name "viv_ext_ctrl"
    struct v4l2_queryctrl queryctrl;
    memset(&queryctrl, 0, sizeof(queryctrl));

    queryctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (0 == ioctl(m_fd, VIDIOC_QUERYCTRL, &queryctrl)) {
        if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED)
            continue;

        ALOGI("%s Control %s", __func__, queryctrl.name);
        if (strcmp((char *)queryctrl.name, VIV_CTRL_NAME) == 0) {
            m_ctrl_id = queryctrl.id;
            ALOGI("%s, find viv ctrl id 0x%x", __func__, m_ctrl_id);
            break;
        }

        queryctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }

    return (m_ctrl_id > 0) ? 0 : NO_INIT;
}

int ISPWrapper::setFeature(const char *value)
{
    int ret = 0;
    struct v4l2_ext_controls ctrls;
    struct v4l2_ext_control ctrl;

    if(value == NULL)
        return BAD_VALUE;

    if ((m_fd <= 0) || (m_ctrl_id == 0))
        return NO_INIT;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = m_ctrl_id;
    ctrl.size = strlen (value) + 1;
    ctrl.string = strdup(value);

    memset(&ctrls, 0, sizeof(ctrls));
    ctrls.which = V4L2_CTRL_ID2WHICH(ctrl.id);
    ctrls.count = 1;
    ctrls.controls = &ctrl;

    ALOGI("setFeature, fd %d, id 0x%x, str %s", m_fd, m_ctrl_id, value);

    ret = ioctl(m_fd, VIDIOC_S_EXT_CTRLS, &ctrls);
    if(ret < 0)
        ALOGE("%s VIDIOC_S_EXT_CTRLS failed, value %s, errno %d, %s",
            __func__, value, errno, strerror(errno));

    free(ctrl.string);

    return ret;
}

int ISPWrapper::processAWB(uint8_t mode)
{
    int ret = 0;
    char *value = NULL;

    ALOGV("%s, mode %d", __func__, mode);

    if(mode >= ARRAY_SIZE(g_strWBList)) {
        ALOGW("%s, unsupported awb mode %d", __func__, mode);
        return BAD_VALUE;
    }

    if(mode == m_awb_mode)
        return 0;

    ALOGI("%s, change WB mode from %d to %d", __func__, m_awb_mode, mode);

    // If shift from AWB to MWB, first disable AWB.
    if( (m_awb_mode == ANDROID_CONTROL_AWB_MODE_AUTO) &&
        (mode != ANDROID_CONTROL_AWB_MODE_AUTO) &&
        (mode != ANDROID_CONTROL_AWB_MODE_OFF) ) {
        value = STR_AWB_DISABLE;
        ret = setFeature(value);
        if(ret) {
            ALOGE("%s, mode %d, disable awb failed", __func__, mode);
            return BAD_VALUE;
        }
    }

    value = g_strWBList[mode];

    ret = setFeature(value);
    if(ret) {
        ALOGE("%s, set wb mode %d failed, ret %d", __func__, mode, ret);
        return BAD_VALUE;
    }

    m_awb_mode = mode;

    return 0;
}

// Current tactic: don't return if some meta process failed,
// since may have other meta to process.
int ISPWrapper::process(Metadata *pMeta)
{
    if(pMeta == NULL)
        return BAD_VALUE;

    camera_metadata_entry_t entry = pMeta->find(ANDROID_CONTROL_AWB_MODE);
    if (entry.count > 0)
        processAWB(entry.data.u8[0]);

    // Todo, add other meta process.

    return 0;
}
