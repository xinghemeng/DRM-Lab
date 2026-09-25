#include <memory>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include "drm_lab/device.h"

namespace drm_lab {
    std::unique_ptr<DrmDevice> DrmDevice::Open(const char* path){
        // Open the DRM device file
        if(path == nullptr) {
            path = kDefaultDevicePath;
        }
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        if(fd < 0){
            fprintf(stderr, "[drm-lab] open %s failed errno=%d (%s)\n", path, errno, strerror(errno));
            return nullptr;
        }
        fprintf(stderr, "[drm-lab] open %s fd=%d\n", path, fd);
        return std::unique_ptr<DrmDevice>(new DrmDevice(fd));
    }

    DrmDevice::DrmDevice(int fd) : fd_(fd) {}

    DrmDevice::DrmDevice(DrmDevice&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    DrmDevice& DrmDevice::operator=(DrmDevice&& other) noexcept {
        if (this != &other) {
            Close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    DrmDevice::~DrmDevice() {
        Close();
    }

    int DrmDevice::getFd() const {
        return fd_;
    }

    void DrmDevice::Close() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }
}