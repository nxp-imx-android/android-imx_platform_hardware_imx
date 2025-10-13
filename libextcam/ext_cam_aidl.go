// Copyright 2023 NXP
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package ext_cam_aidl

import (
	"android/soong/android"
	"android/soong/cc"
	"github.com/google/blueprint/proptools"
	"strings"
)

func init() {
	android.RegisterModuleType("imx_ext_cam_aidl_defaults", extCamDefaultsFactory)
}

func extCamDefaultsFactory() android.Module {
	module := cc.DefaultsFactory()
	android.AddLoadHook(module, extCamDefaults)
	return module
}

func extCamDefaults(ctx android.LoadHookContext) {
	type props struct {
		Target struct {
			Android struct {
				Enabled  *bool
				Cppflags []string
				Shared_libs []string
			}
		}
	}
	p := &props{}
	var platform string = ctx.Config().VendorConfig("IMXPLUGIN").String("BOARD_PLATFORM")
	var board string = ctx.Config().VendorConfig("IMXPLUGIN").String("BOARD_SOC_TYPE")
	if strings.Contains(platform, "imx") {
		p.Target.Android.Enabled = proptools.BoolPtr(true)
	} else {
		p.Target.Android.Enabled = proptools.BoolPtr(false)
	}

	if ctx.Config().VendorConfig("IMXPLUGIN").String("TARGET_GRALLOC_VERSION") == "v4" {
		p.Target.Android.Cppflags = append(p.Target.Android.Cppflags, "-DGRALLOC_VERSION=4")
	}

	if (strings.Contains(board, "IMX8MQ") || strings.Contains(board, "IMX8Q") || strings.Contains(board, "IMX95")) {
		p.Target.Android.Cppflags = append(p.Target.Android.Cppflags, "-DIMX_VPU_JPEG_DECODER")
		p.Target.Android.Shared_libs = append(p.Target.Android.Shared_libs, "lib_imx_c2_videodec_common")
		p.Target.Android.Shared_libs = append(p.Target.Android.Shared_libs, "lib_imx_c2_v4l2_dec")
	}

	ctx.AppendProperties(p)
}
