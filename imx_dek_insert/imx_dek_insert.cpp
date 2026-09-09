/*
 * Copyright (C) 2020 The Android Open Source Project
 * Copyright 2023 NXP
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

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

#include <android/log.h>
#include <android-base/logging.h>

#include <aidl/nxp/hardware/imx_dek_extractor/IDek_Extractor.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include "imx_dek_insert.h"

using namespace android;
using aidl::nxp::hardware::imx_dek_extractor::IDek_Extractor;

#define SECO_PADING 	(0x400)
#define CONTAINER_TAG 	0x87
#define SIG_BLK_TAG 	0x90
#define IMX_BL_PAYLOAD_SIZE   (96)

/*
 * i.MX9 (i.MX93/937/95/952) AHAB layout.
 *
 * Unlike i.MX8QM/QXP/ULP, the first AHAB container starts at offset 0 of the
 * image (no SECO padding), and the container carrying the DEK blob is not
 * necessarily the first one. The container that holds the DEK is the one whose
 * signature block advertises a non-zero blob_offset. We walk the containers
 * and pick that one, mirroring how CST computes the DEK offset:
 *     dek_offset = container_start + signature_block_offset + blob_offset
 *
 * Containers are laid out back-to-back: the next container starts at the
 * current container_start plus ALIGN(header.length, align). The alignment is
 * fixed by the FIRST container's header version: legacy (v1) uses 0x400, PQC
 * (v2, e.g. i.MX95 B0) uses 0x4000. This mirrors imx-mkimage
 * parse_container_hdrs_qx_qm_b0(): file_off += ALIGN(header.length, align),
 * with align derived from container_headers[0].version.
 *
 * Container tags: 0x87 (ELE/AP, IVT_HEADER_TAG_B0) and 0x82 (V2X,
 * IVT_HEADER_TAG_V2X); both are valid container headers to walk over.
 */
#define IMX9_CONTAINER_ALIGNMENT_V1	(0x400)
#define IMX9_CONTAINER_ALIGNMENT_V2	(0x4000)
#define IMX9_CONTAINER_VERSION_PQC	(2)
#define IMX9_MAX_CONTAINERS		(3)
#define IMX9_CONTAINER_TAG_V2X		(0x82)

/* ALIGN(x, a): round x up to a multiple of a (a must be a power of two). */
#define IMX9_ALIGN(x, a)	(((x) + ((a) - 1)) & ~((off_t)(a) - 1))

static std::shared_ptr<IDek_Extractor> dek_extractor_(nullptr);

static error_t insert_dek_blob_to_fit(image_type_t image_t, char *target_device,
		std::vector<uint8_t> dek_blob, off_t fit_length, int bootloader0_offset_byte) {
	int file_fd = -1;
	error_t ret = NO_ERROR;

	file_fd = open(target_device, O_RDWR);
	if (file_fd < 0) {
		printf("file_fd: can not open %s!\r\n", target_device);
		ret = FILE_ERROR;
		goto fail;
	}

	if (image_t == SPL) {
		lseek(file_fd, fit_length + bootloader0_offset_byte, SEEK_SET);
		LOG(INFO) << "The dek blob offset is: " << fit_length + bootloader0_offset_byte;
	} else if (image_t == BOOTLOADER) {
		lseek(file_fd, fit_length - IMX_BL_PAYLOAD_SIZE, SEEK_SET);
		LOG(INFO) << "The dek blob offset is: " << fit_length - IMX_BL_PAYLOAD_SIZE;
	} else {
		printf("Wrong parameter: image type error!\r\n");
		ret = PARAMETER_ERROR;
		goto file_fail;
	}

	write(file_fd, &dek_blob[0], dek_blob.size());

file_fail:
	close(file_fd);
fail:
	return ret;
}

static error_t insert_dek_blob_to_container(image_type_t image_t, char *target_device,
		std::vector<uint8_t> dek_blob, int bootloader0_offset_byte) {
	int file_fd = -1;
	error_t ret = NO_ERROR;
	void *image_header = NULL;
	byte_str_t image_h = {0, 0, 0};
	off_t blob_offset = 0;
	uint16_t sig_blk_offset = 0, signature_blob_offset = 0;

	file_fd = open(target_device, O_RDWR);
	if (file_fd < 0) {
		printf("file_fd: can not open %s!\r\n", target_device);
		ret = FILE_ERROR;
		goto open_fail;
	}

	if (SPL == image_t) {
		pread(file_fd, &image_h, sizeof(byte_str_t), SECO_PADING + bootloader0_offset_byte);
		lseek(file_fd, SECO_PADING + bootloader0_offset_byte, SEEK_SET);
	} else if (BOOTLOADER == image_t) {
		pread(file_fd, &image_h, sizeof(byte_str_t), bootloader0_offset_byte);
	} else {
		ret = IMAGE_ERROR;
		goto image_fail;
	}

	/*
	 * Check the container tag
	 */
	if (CONTAINER_TAG != image_h.tag) {
		ret = IMAGE_ERROR;
		goto image_fail;
	}

	image_header = (void *)malloc(image_h.length);
	if (!image_header) {
		ret = MEMORY_ERROR;
		goto image_fail;
	}

	read(file_fd, image_header, image_h.length);

	sig_blk_offset = ((ahab_container_header_t *)image_header)->signature_block_offset;

	/*
	 * Check the signature block tag
	 */
	if (SIG_BLK_TAG != ((byte_str_t *)((uint8_t *)image_header + sig_blk_offset))->tag) {
		ret = IMAGE_ERROR;
		goto memory_fail;
	}

	signature_blob_offset = ((ahab_container_signature_block_t *)
			((uint8_t *)image_header + sig_blk_offset))->blob_offset;

	blob_offset = (SPL == image_t) ? (SECO_PADING + sig_blk_offset + signature_blob_offset) :
			(sig_blk_offset + signature_blob_offset);

	pwrite(file_fd, &dek_blob[0], dek_blob.size(), blob_offset);

	ret = NO_ERROR;

memory_fail:
	if (image_header)
		free(image_header);
image_fail:
	close(file_fd);
open_fail:
	return ret;
}

static error_t insert_dek_blob_to_container_imx9(image_type_t image_t, char *target_device,
		std::vector<uint8_t> dek_blob, int bootloader0_offset_byte) {
	int file_fd = -1;
	error_t ret = IMAGE_ERROR;
	off_t image_base = 0;

	if (SPL == image_t) {
		image_base = bootloader0_offset_byte;
	} else if (BOOTLOADER == image_t) {
		image_base = bootloader0_offset_byte;
	} else {
		return IMAGE_ERROR;
	}

	file_fd = open(target_device, O_RDWR);
	if (file_fd < 0) {
		printf("file_fd: can not open %s!\r\n", target_device);
		return FILE_ERROR;
	}

	ahab_container_header_t first_hdr = {0};
	off_t container_alignment = IMX9_CONTAINER_ALIGNMENT_V2;

	if (pread(file_fd, &first_hdr, sizeof(first_hdr), image_base) == (ssize_t)sizeof(first_hdr)
			&& (CONTAINER_TAG == first_hdr.tag || IMX9_CONTAINER_TAG_V2X == first_hdr.tag)) {
		container_alignment = (IMX9_CONTAINER_VERSION_PQC == first_hdr.version) ?
				IMX9_CONTAINER_ALIGNMENT_V2 : IMX9_CONTAINER_ALIGNMENT_V1;
	}

	off_t container_base = image_base;
	for (int slot = 0; slot < IMX9_MAX_CONTAINERS; slot++) {
		ahab_container_header_t container_hdr = {0};
		ahab_container_signature_block_t sig_blk = {0};
		off_t sig_blk_base = 0, blob_offset = 0;

		if (pread(file_fd, &container_hdr, sizeof(container_hdr), container_base)
				!= (ssize_t)sizeof(container_hdr))
			break;

		if (CONTAINER_TAG != container_hdr.tag && IMX9_CONTAINER_TAG_V2X != container_hdr.tag)
			break;

		if (0 != container_hdr.signature_block_offset) {
			sig_blk_base = container_base + container_hdr.signature_block_offset;
			if (pread(file_fd, &sig_blk, sizeof(sig_blk), sig_blk_base)
					== (ssize_t)sizeof(sig_blk)
					&& SIG_BLK_TAG == sig_blk.tag && 0 != sig_blk.blob_offset) {
				blob_offset = sig_blk_base + sig_blk.blob_offset;
				if (pwrite(file_fd, &dek_blob[0], dek_blob.size(), blob_offset)
						== (ssize_t)dek_blob.size()) {
					LOG(INFO) << "dek blob inserted at offset " << blob_offset;
					ret = NO_ERROR;
				} else {
					ret = FILE_ERROR;
				}
				break;
			}
		}

		container_base += IMX9_ALIGN(container_hdr.length, container_alignment);
	}

	if (ret == IMAGE_ERROR)
		printf("No AHAB container with a DEK slot found!\r\n");

	close(file_fd);
	return ret;
}

static int connectAIDL() {
	int ret = 0;

	auto dek_extractor_name = std::string() + IDek_Extractor::descriptor + "/default";
	LOG(INFO) << "connecting to AIDL" << dek_extractor_name.c_str() << "...";

	if (AServiceManager_isDeclared(dek_extractor_name.c_str())) {
		LOG(INFO) << "waiting for AIDL service" << dek_extractor_name.c_str() << "...";
		auto dek_extractor_Binder = ndk::SpAIBinder(AServiceManager_waitForService(dek_extractor_name.c_str()));
		dek_extractor_ = IDek_Extractor::fromBinder(dek_extractor_Binder);
		if (!dek_extractor_) {
			LOG(ERROR) << "Failed to connect to AIDL" << dek_extractor_name.c_str();
			ret = -1;
		}
	} else {
		LOG(ERROR) << "AIDL " << dek_extractor_name.c_str() << " is not declared!";
		ret = -1;
	}

	return ret;
}

static error_t read_dek_blob(std::vector<uint8_t>* dek_blob, image_type_t image_t) {
	int32_t err = 0;


	if (connectAIDL()){
		LOG(ERROR) << "Failed to connect service";
		return CONNECT_ERROR;
	}
	if (!dek_extractor_) {
		LOG(ERROR) << "dek_extractor AIDL service is not initialized!";
		return CONNECT_ERROR;
	} else {
		auto status = dek_extractor_->Dek_ExtractorInit(dek_blob, (int)image_t, &err);
		if ((status.getExceptionCode() != EX_NONE) || (err < 0)) {
			LOG(ERROR) << "AIDL call Dek_ExtractorInit failed!";
			return CONNECT_ERROR;
		}
	}

	return NO_ERROR;
}

static error_t insert_dek_blob(soc_type_t soc, std::vector<uint8_t> dek_blob, image_type_t image_t,
		char *target_device, int bootloader0_offset_byte, off_t file_length) {
	int ret = NO_ERROR;

	switch(soc)
	{
		case MM:
		case MN:
		case MP:
		case MQ:
			ret = insert_dek_blob_to_fit(image_t, target_device, dek_blob, file_length, bootloader0_offset_byte);
			if (ret) {
				printf("Failed to insert dek_blob to fit, exit! %d\r\n", ret);
				return OTHER_ERROR;
			}
			break;
		case QM:
		case QX:
		case ULP:
			ret = insert_dek_blob_to_container(image_t, target_device, dek_blob, bootloader0_offset_byte);
			if (ret) {
				printf("Failed to insert dek_blob to container, exit! %d\r\n", ret);
				return OTHER_ERROR;
			}
			break;
		case IMX9:
			ret = insert_dek_blob_to_container_imx9(image_t, target_device, dek_blob, bootloader0_offset_byte);
			if (ret) {
				printf("Failed to insert dek_blob to imx9 container, exit! %d\r\n", ret);
				return OTHER_ERROR;
			}
			break;
		default:
			printf("soc not found\n");
			return PARAMETER_ERROR;
	}

	return NO_ERROR;
}

int main(int argc, char** argv) {
	std::vector<uint8_t> dek_blob;

	static struct option long_options[] =
	{
		{"soc", required_argument, NULL, 's'},
		{"target", required_argument, NULL, 't'},
		{"bootloader", no_argument, NULL, 'B'},
		{"spl", required_argument, NULL, 'S'},
		{"length", required_argument, NULL, 'l'},
		{NULL, 0, NULL, 0}
	};

	const char *optstring = "s:t:BS:l:";
	int option_index = 0;
	int bootloader0_offset_k = 0, bootloader0_offset_b = 0;
	soc_type_t soc = SOC_NONE;
	image_type_t image_t = IMAGE_NONE;
	int c;
	int ret = NO_ERROR;
	char target_device[50] = {'\0'};
	off_t file_length = 0;

	while ((c = getopt_long(argc, argv, optstring, long_options, &option_index)) != -1) {
		if (c == -1)
			break;
		switch(c)
		{
			case 's':
				if(!strncmp(optarg, "imx8mm", 6))
					soc = MM;
				else if(!strncmp(optarg, "imx8mn", 6))
					soc = MN;
				else if(!strncmp(optarg, "imx8mp", 6))
					soc = MP;
				else if(!strncmp(optarg, "imx8mq", 6))
					soc = MQ;
				else if(!strncmp(optarg, "imx8qm", 6))
					soc = QM;
				else if(!strncmp(optarg, "imx8qxp", 7))
					soc = QX;
				else if(!strncmp(optarg, "imx8ulp", 7))
					soc = ULP;
				else if(!strncmp(optarg, "imx937", 6))
					soc = IMX9;
				else if(!strncmp(optarg, "imx95", 5))
					soc = IMX9;
				else if(!strncmp(optarg, "imx952", 6))
					soc = IMX9;
				else {
					printf("unrecognized SOC: %s \n",optarg);
				}
				break;
			case 't':
				memcpy(target_device, optarg, strlen(optarg));
				break;
			case 'S':
				image_t = SPL;
				bootloader0_offset_k= atoi(optarg);
				bootloader0_offset_b = bootloader0_offset_k * 1024;
				break;
			case 'B':
				image_t = BOOTLOADER;
				break;
			case 'l':
				file_length = (off_t)atoi(optarg);
				break;
		}
	}

	ret = read_dek_blob(&dek_blob, image_t);
	if (ret != NO_ERROR)
		return EXIT_FAILURE;

	ret = insert_dek_blob(soc, dek_blob, image_t, target_device, bootloader0_offset_b, file_length);
	if (ret != NO_ERROR)
		return EXIT_FAILURE;

	printf("Done.\n");

	return ret == NO_ERROR ? EXIT_SUCCESS : EXIT_FAILURE;
}
