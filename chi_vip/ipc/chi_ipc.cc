// chi_ipc.cc — ChiShmHandle implementation (POSIX shm_open / mmap)

#include "chi_ipc.hh"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <stdexcept>

namespace gem5 { namespace ruby { namespace chi_ipc {

static constexpr size_t SHM_SIZE = sizeof(ChiShmLayout);

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
ChiShmHandle::ChiShmHandle(const std::string& name, bool create)
    : name_(name), owner_(create)
{
    if (create) {
        // Remove any stale segment from a previous run.
        shm_unlink(name_.c_str());

        fd_ = shm_open(name_.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd_ < 0) {
            throw std::runtime_error(
                std::string("chi_ipc: shm_open(create) failed for '") +
                name_ + "': " + strerror(errno));
        }
        if (ftruncate(fd_, static_cast<off_t>(SHM_SIZE)) < 0) {
            close(fd_);
            shm_unlink(name_.c_str());
            throw std::runtime_error(
                std::string("chi_ipc: ftruncate failed for '") +
                name_ + "': " + strerror(errno));
        }

        void* p = mmap(nullptr, SHM_SIZE,
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) {
            close(fd_);
            shm_unlink(name_.c_str());
            throw std::runtime_error(
                std::string("chi_ipc: mmap failed for '") +
                name_ + "': " + strerror(errno));
        }

        // Placement-new: constructs atomics and sets counters to 0.
        layout_ = new (p) ChiShmLayout();

        fprintf(stderr, "[chi_ipc] created shm '%s' (%zu bytes)\n",
                name_.c_str(), SHM_SIZE);
    }
    // For create=false (questa side), call tryOpen() externally in a loop.
}

// ─────────────────────────────────────────────────────────────────────────────
// tryOpen — non-blocking attach for the Questa side
// ─────────────────────────────────────────────────────────────────────────────
bool ChiShmHandle::tryOpen()
{
    if (layout_) return true;  // already open

    int fd = shm_open(name_.c_str(), O_RDWR, 0);
    if (fd < 0) return false;

    // Verify size before mapping.
    struct stat st;
    if (fstat(fd, &st) < 0 || static_cast<size_t>(st.st_size) < SHM_SIZE) {
        close(fd);
        return false;
    }

    void* p = mmap(nullptr, SHM_SIZE,
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return false;
    }

    fd_     = fd;
    layout_ = static_cast<ChiShmLayout*>(p);

    fprintf(stderr, "[chi_ipc] attached shm '%s'\n", name_.c_str());
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Destructor
// ─────────────────────────────────────────────────────────────────────────────
ChiShmHandle::~ChiShmHandle()
{
    if (layout_) {
        munmap(layout_, SHM_SIZE);
        layout_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    if (owner_) {
        shm_unlink(name_.c_str());
        fprintf(stderr, "[chi_ipc] unlinked shm '%s'\n", name_.c_str());
    }
}

}}} // namespace gem5::ruby::chi_ipc
