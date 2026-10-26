#pragma once
#include "protocol.hpp"
#include <span>

namespace Offload {
    std::span<const unsigned char> embeddedWorkerImage();
    FD createWorkerExecutable();
}
