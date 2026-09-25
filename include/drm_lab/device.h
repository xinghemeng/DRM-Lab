#ifndef DRM_DEVICE_H_
#define DRM_DEVICE_H_

#include <memory>

namespace drm_lab {
    class DrmDevice {
    public:
        static constexpr const char* kDefaultDevicePath = "/dev/dri/card1";

        ~DrmDevice();
        DrmDevice(const DrmDevice&) = delete;
        DrmDevice& operator=(const DrmDevice&) = delete;
        DrmDevice(DrmDevice&&) noexcept;
        DrmDevice& operator=(DrmDevice&&) noexcept;

        int getFd() const;
        static std::unique_ptr<DrmDevice> Open(const char* path);
    private:
        explicit DrmDevice(int fd);
        void Close();
        int fd_ = -1;
    };

}

#endif // DRM_DEVICE_H_