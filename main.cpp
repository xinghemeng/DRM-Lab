#include <cstdio>
#include <iostream>

#include "drm_lab/device.h"

int main(int, char**){
    std::cout << "Hello, from drm-lab!\n";
    auto device = drm_lab::DrmDevice::Open(drm_lab::DrmDevice::kDefaultDevicePath);
    if (!device) {
        return 1;
    }
    fprintf(stderr, "[drm-lab] Device opened fd=%d\n", device->getFd());
    return 0;
}
