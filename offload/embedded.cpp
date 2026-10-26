#include "embedded.hpp"
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/stat.h>

extern "C" {
    extern const unsigned char hdrfix_worker_start[];
    extern const unsigned char hdrfix_worker_end[];
}

namespace Offload {
std::span<const unsigned char> embeddedWorkerImage() {
    return {hdrfix_worker_start, reinterpret_cast<uintptr_t>(hdrfix_worker_end) - reinterpret_cast<uintptr_t>(hdrfix_worker_start)};
}

FD createWorkerExecutable() {
    unsigned flags = MFD_CLOEXEC | MFD_ALLOW_SEALING;
#ifdef MFD_EXEC
    flags |= MFD_EXEC;
#endif
    FD fd(memfd_create("hyprland-fix-hdr-worker", flags));
#ifdef MFD_EXEC
    // Older kernels do not recognize MFD_EXEC.
    if (fd.value < 0 && errno == EINVAL)
        fd.reset(memfd_create("hyprland-fix-hdr-worker", flags & ~MFD_EXEC));
#endif
    if (fd.value < 0) return {};
    const auto image = embeddedWorkerImage();
    size_t offset = 0;
    while (offset < image.size()) {
        const auto written = write(fd.value, image.data() + offset, image.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return {};
        offset += written;
    }
    if (fchmod(fd.value, 0500) < 0 ||
        fcntl(fd.value, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0)
        return {};
    return fd;
}
}
