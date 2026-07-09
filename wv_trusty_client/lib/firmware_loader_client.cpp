/*
 * Copyright 2023 NXP.
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

#define LOG_TAG "firmware_loader_client"
#include <BufferAllocator/BufferAllocator.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <errno.h>
#include <firmware_loader_client.h>
#include <getopt.h>
#include <log/log.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <trusty/tipc.h>
#include <unistd.h>
#include <assert.h>

#include <algorithm>
#include <utils/Mutex.h>
#include <android-base/file.h>
#include <filesystem>
#include <android-base/properties.h>
#include <cutils/properties.h>
#include <dirent.h>
#include <json/json.h>
#include <json/reader.h>
#include <vector>

#define TRUSTY_DEVICE_NAME "/dev/trusty-ipc-dev0"
using android::base::unique_fd;
using std::string;

constexpr const char kTrustyDefaultDeviceName[] = "/dev/trusty-ipc-dev0";
static const char* dev_name = kTrustyDefaultDeviceName;

using android::Mutex;
static Mutex mLoadLock;

static const char* _sopts = "hD:t:c:";
static const struct option _lopts[] = {
        {"help", no_argument, 0, 'h'},
        {"dev", required_argument, 0, 'D'},
        {"type", required_argument, 0, 't'},
        {"config", required_argument, 0, 'c'},
        {0, 0, 0, 0},
};

static uint32_t firmware_type = FIRMWARE_LOADER_TYPE_VPU;

/* Path of the JSON config file when running in --config mode; null otherwise. */
static const char* config_file = nullptr;

/*
 * Default video decoder node name used to verify a VPU firmware after load.
 * Kept for backward compatibility with the legacy CLI mode (-t vpu). In
 * --config mode this is overridden per-entry by the optional
 * "verify_node_name" field, and an empty name means "skip verification".
 */
static const char* kDefaultVpuNodeName = "amphion-vpu-decoder";

/* One firmware entry as described by the JSON config file. */
struct firmware_entry {
    std::string name;             // human-readable type name (e.g. "vpu")
    uint32_t id;                  // firmware id sent to the Trusty TA
    std::string path;             // path to the .signed firmware package
    std::string verify_node_name; // v4l2 node name to verify; empty = skip
};

static const char* usage = "Usage: %s [options] [package-file]\n"
                           "\n"
                           "options:\n"
                           "  -h, --help            prints this message and exit\n"
                           "  -D, --dev name        Trusty device name\n"
                           "  -t, --type name       firmware type (vpu, dpu)\n"
                           "  -c, --config file     load firmwares listed in a JSON config file\n"
                           "\n"
                           "When --config is given, package-file and --type are ignored and all\n"
                           "firmware entries in the JSON file are loaded in order.\n"
                           "\n";

static bool parse_firmware_type(const char* type_str, uint32_t* out_type) {
    if (strcasecmp(type_str, "vpu") == 0) {
        *out_type = FIRMWARE_LOADER_TYPE_VPU;
        return true;
    }
    return false;
}

static void print_usage_and_exit(const char* prog, int code) {
    fprintf(stderr, usage, prog);
    exit(code);
}

using android::base::unique_fd;
using std::string;

static unique_fd read_file(const char* file_name, off64_t* out_file_size) {
    int rc;
    long page_size = sysconf(_SC_PAGESIZE);
    off64_t file_size, file_page_offset, file_page_size;
    struct stat64 st;

    unique_fd file_fd(TEMP_FAILURE_RETRY(open(file_name, O_RDONLY)));
    if (!file_fd.ok()) {
        ALOGE("Error opening file =%s", file_name);
        return {};
    }

    rc = fstat64(file_fd, &st);
    if (rc < 0) {
        ALOGE("Error calling stat on file %s", file_name);
        return {};
    }

    assert(st.st_size >= 0);
    file_size = st.st_size;

    /* The dmabuf size needs to be a multiple of the page size */
    file_page_offset = file_size & (page_size - 1);
    if (file_page_offset) {
        file_page_offset = page_size - file_page_offset;
    }
    if (__builtin_add_overflow(file_size, file_page_offset, &file_page_size)) {
        ALOGE("Failed to page-align file size");
        return {};
    }

    BufferAllocator alloc;
    unique_fd dmabuf_fd(alloc.Alloc("reserved", file_page_size));
    if (!dmabuf_fd.ok()) {
        ALOGE("Error creating dmabuf: %d", dmabuf_fd.get());
        return dmabuf_fd;
    }

    void* shm = mmap(0, file_page_size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
    if (shm == MAP_FAILED) {
        return {};
    }

    off64_t file_offset = 0;
    while (file_offset < file_size) {
        ssize_t num_read = TEMP_FAILURE_RETRY(
                pread(file_fd, (char*)shm + file_offset, file_size - file_offset, file_offset));

        if (num_read < 0) {
            ALOGE("Error reading firmware file %s", file_name);
            break;
        }

        if (num_read == 0) {
            ALOGE("Unexpected end of file %s", file_name);
            break;
        }

        file_offset += (off64_t)num_read;
    }

    munmap(shm, file_page_size);

    if (file_offset < file_size) {
        return {};
    }

    assert(file_offset == file_size);
    if (out_file_size) {
        *out_file_size = file_size;
    }

    return dmabuf_fd;
}

static ssize_t send_load_message(int tipc_fd, int package_fd, off64_t package_size,
                                  bool secure, uint32_t fw_id) {
    struct firmware_loader_header hdr = {
            .cmd = FIRMWARE_LOADER_CMD_LOAD_FIRMWARE,
    };
    struct firmware_loader_load_firmware_req req = {
            .package_size = static_cast<uint64_t>(package_size),
            .secure = secure,
            .firmware_id = fw_id,
    };
    struct iovec tx[2] = {{&hdr, sizeof(hdr)}, {&req, sizeof(req)}};
    struct trusty_shm shm = {
            .fd = package_fd,
            .transfer = TRUSTY_SHARE,
    };
    return tipc_send(tipc_fd, tx, 2, &shm, 1);
}

static ssize_t read_response(int tipc_fd) {
    struct firmware_loader_resp resp;
    ssize_t rc = read(tipc_fd, &resp, sizeof(resp));
    if (rc < 0) {
        ALOGE("Failed to read response");
        return rc;
    }

    if (rc < sizeof(resp)) {
        ALOGE("Not enough data in response: %zd", rc);
        return -EIO;
    }

    if (resp.hdr.cmd != (FIRMWARE_LOADER_CMD_LOAD_FIRMWARE | FIRMWARE_LOADER_RESP_BIT)) {
        ALOGE("Invalid command in response: %d", resp.hdr.cmd);
        return -EINVAL;
    }

    switch (resp.error) {
        case FIRMWARE_LOADER_NO_ERROR:
            break;
        case FIRMWARE_LOADER_ERR_NONE_KEY:
            ALOGE("there is no firmware key, try to load clear firmware");
            break;
        case FIRMWARE_LOADER_ERR_UNKNOWN_CMD:
            ALOGE("Error: unknown command");
            break;
        default:
            ALOGE("Unrecognized error: %d", resp.error);
            break;
    }
    return static_cast<ssize_t>(resp.error);
}

ssize_t load_clear_firmware_package(int tipc_fd, std::string firmware_name, uint32_t fw_id) {
    ssize_t rc = 0;
    off64_t firmware_size;

    // try to load clear fw
    std::filesystem::path p(firmware_name);
    std::string desired_extension = ".signed";
    if (p.has_filename() && p.extension() == desired_extension) {
        std::filesystem::path parent_dir = p.parent_path();
        std::filesystem::path stem = p.stem();
        std::filesystem::path new_path = parent_dir / stem;
        firmware_name = new_path.string();
    }
    ALOGD("load clear fw name=%s", firmware_name.c_str());
    unique_fd clear_firmware_fd = read_file(firmware_name.c_str(), &firmware_size);
    if (!clear_firmware_fd.ok()) {
        rc = -1;
        return rc;
    }
    rc = send_load_message(tipc_fd, clear_firmware_fd, firmware_size, false, fw_id);
    if (rc < 0) {
        ALOGE("Failed to send clear firmware package: %zd", rc);
        return rc;
    }

    rc = read_response(tipc_fd);
    if (rc != FIRMWARE_LOADER_NO_ERROR) {
        ALOGE("load clear firmware failed rc=%zd", rc);
    }
    return rc;
}

ssize_t load_firmware_package(const char* firmware_file_name, uint32_t firmware_id) {

    Mutex::Autolock autoLock(mLoadLock);
    ssize_t rc = 0;
    int tipc_fd = -1;
    off64_t firmware_size;
    std::string name(firmware_file_name);
    std::string storage_property;

    if (firmware_id >= FIRMWARE_LOADER_TYPE_MAX) {
        ALOGE("Invalid firmware type: %u", firmware_id);
        return -EINVAL;
    }

    ALOGD("load fw name=%s type=%u", name.c_str(), firmware_id);
    unique_fd firmware_fd = read_file(name.c_str(), &firmware_size);
    if (!firmware_fd.ok()) {
        rc = -1;
        goto err_read_file;
    }

    tipc_fd = tipc_connect(TRUSTY_DEVICE_NAME, FIRMWARE_LOADER_PORT);
    if (tipc_fd < 0) {
        ALOGE("Failed to connect to firmware loader: %s", strerror(-tipc_fd));
        rc = tipc_fd;
        goto err_tipc_connect;
    }

    storage_property = ::android::base::GetProperty("vendor.storageproxyd", "");
    if (storage_property == "trusty") {
        rc = send_load_message(tipc_fd, firmware_fd, firmware_size, true, firmware_id);
        if (rc < 0) {
            ALOGE("Failed to send secure firmware package: %zd", rc);
            goto err_send;
        }

        rc = read_response(tipc_fd);
        if (rc != FIRMWARE_LOADER_NO_ERROR) {
            ALOGE("load secure firmware failed rc=%zd", rc);
            if (rc == FIRMWARE_LOADER_ERR_NONE_KEY) {
                ALOGI("try to load clear firmware");
                rc = load_clear_firmware_package(tipc_fd, name, firmware_id);
            }
        } else {
            ALOGI("load secure firmware success");
        }
    } else {
        ALOGI("secure storage is not ready, try to load clear firmware");
        rc = load_clear_firmware_package(tipc_fd, name, firmware_id);
    }

err_send:
    tipc_close(tipc_fd);
err_tipc_connect:
err_read_file:
    return rc;
}

static void parse_options(int argc, char** argv) {
    int c;

    while (1) {
        c = getopt_long(argc, argv, _sopts, _lopts, nullptr);
        if (c == -1) {
            ALOGD("directly get firmware path");
            break;
        }

        switch (c) {
            case 'h':
                print_usage_and_exit(argv[0], EXIT_SUCCESS);
                break;

            case 'D':
                dev_name = strdup(optarg);
                break;

            case 't':
                if (!parse_firmware_type(optarg, &firmware_type)) {
                    ALOGE("Unknown firmware type: %s", optarg);
                    print_usage_and_exit(argv[0], EXIT_FAILURE);
                }
                break;

            case 'c':
                config_file = optarg;
                break;

            default:
                print_usage_and_exit(argv[0], EXIT_FAILURE);
        }
    }
}

static int32_t openVideoDecoderDevices(const char* devNode) {

    ALOGI("getNodeName: dev path:%s", devNode);
    int fd = -1;
    fd = open(devNode, O_RDWR);
    if (fd < 0) {
        ALOGE("%s open dev path:%s failed:%s", __func__, devNode, strerror(errno));
        return -1;
    } else {
        ALOGD("open decoder device successfully");
        close(fd);
    }
    return 0;
}

static int32_t findVideoDecoderNodes(const std::string& video_decoder_name) {
    DIR* vidDir = NULL;
    struct dirent* dirEntry;
    std::string buffer;
    char videoDevice[64];
    char nodeName[64];

    if (video_decoder_name.empty()) {
        ALOGD("no verify node name configured, skip video node verification");
        return 0;
    }

    vidDir = opendir("/sys/class/video4linux");
    if (vidDir == NULL) {
        return -1;
    }
    while ((dirEntry = readdir(vidDir)) != NULL) {
        if (strncmp(dirEntry->d_name, "video", 5)) {
            continue;
        }
        memset(nodeName, 0, 64);
        sprintf(videoDevice, "/sys/class/video4linux/%s/name", dirEntry->d_name);
        if (!android::base::ReadFileToString(std::string(videoDevice), &buffer)) {
            continue;
        }
        // found the video decoder device
        if (video_decoder_name.compare(0, video_decoder_name.length(), buffer, 0, video_decoder_name.length()) == 0) {
            //open video decoder device
            sprintf(nodeName, "/dev/%s", dirEntry->d_name);
            int32_t ret = openVideoDecoderDevices(nodeName);
            if (ret < 0) {
                closedir(vidDir);
                return ret;
            }
            break;
        }
    }
    closedir(vidDir);
    return 0;
}

static bool parse_config_file(const char* path, std::vector<firmware_entry>* out_entries) {
    std::string content;
    if (!android::base::ReadFileToString(path, &content)) {
        ALOGE("Failed to read config file: %s", path);
        return false;
    }

    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errs;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(content.c_str(), content.c_str() + content.size(), &root, &errs)) {
        ALOGE("Failed to parse config file %s: %s", path, errs.c_str());
        return false;
    }

    const Json::Value& firmwares = root["firmwares"];
    if (!firmwares.isArray() || firmwares.empty()) {
        ALOGE("Config file %s has no valid \"firmwares\" array", path);
        return false;
    }

    for (const auto& item : firmwares) {
        if (!item.isMember("id") || !item.isMember("path")) {
            ALOGE("Config entry missing required \"id\" or \"path\" field");
            return false;
        }

        firmware_entry entry;
        entry.name = item.get("name", "").asString();
        entry.id = item["id"].asUInt();
        entry.path = item["path"].asString();
        entry.verify_node_name = item.get("verify_node_name", "").asString();

        if (entry.path.empty()) {
            ALOGE("Config entry \"%s\" has empty path", entry.name.c_str());
            return false;
        }
        if (entry.id >= FIRMWARE_LOADER_TYPE_MAX) {
            ALOGE("Config entry \"%s\" has invalid id %u (max %u)", entry.name.c_str(), entry.id,
                  FIRMWARE_LOADER_TYPE_MAX);
            return false;
        }

        out_entries->push_back(std::move(entry));
    }

    return true;
}

static int load_from_config(const char* path) {
    std::vector<firmware_entry> entries;
    if (!parse_config_file(path, &entries)) {
        return -EINVAL;
    }

    int final_rc = 0;
    for (const auto& entry : entries) {
        ALOGI("loading firmware \"%s\" id=%u path=%s", entry.name.c_str(), entry.id,
              entry.path.c_str());
        int rc = load_firmware_package(entry.path.c_str(), entry.id);
        if (rc != 0) {
            ALOGE("failed to load firmware \"%s\" rc=%d", entry.name.c_str(), rc);
            final_rc = rc;
            continue;
        }

        /* Verify the post-load device node only when the entry asks for it. */
        if (!entry.verify_node_name.empty()) {
            rc = findVideoDecoderNodes(entry.verify_node_name);
            if (rc < 0) {
                ALOGE("find video decoder node \"%s\" failed for firmware \"%s\"",
                      entry.verify_node_name.c_str(), entry.name.c_str());
                final_rc = rc;
            }
        }
    }

    return final_rc;
}

int main(int argc, char** argv) {
    parse_options(argc, argv);

    if (config_file != nullptr) {
        int rc = load_from_config(config_file);
        return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (optind + 1 != argc) {
        print_usage_and_exit(argv[0], EXIT_FAILURE);
    }
    int rc = load_firmware_package(argv[optind], firmware_type);
    if (rc == 0) {
       rc = findVideoDecoderNodes(kDefaultVpuNodeName);
       if (rc < 0)
           ALOGE("find VideoDecoderNodes failed");
    }

    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
