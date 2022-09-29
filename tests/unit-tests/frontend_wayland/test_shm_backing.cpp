
/*
 * Copyright © 2022 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "src/server/frontend_wayland/shm_backing.h"

#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <boost/throw_exception.hpp>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

namespace
{
bool error_indicates_tmpfile_not_supported(int error)
{
    return
        error == EISDIR ||        // Directory exists, but no support for O_TMPFILE
        error == ENOENT ||        // Directory doesn't exist, and no support for O_TMPFILE
        error == EOPNOTSUPP ||    // Filesystem that directory resides on does not support O_TMPFILE
        error == EINVAL;          // There apparently exists at least one development board that has a kernel
                                  // that incorrectly returns EINVAL. Yay.
}

int memfd_create(char const* name, unsigned int flags)
{
    return static_cast<int>(syscall(SYS_memfd_create, name, flags));
}

auto make_shm_fd(size_t size) -> mir::Fd
{
    int fd = memfd_create("mir-shm-test", MFD_CLOEXEC);
    if (fd == -1 && errno == ENOSYS)
    {
        fd = open("/dev/shm", O_TMPFILE | O_RDWR | O_EXCL | O_CLOEXEC, S_IRWXU);

        // Workaround for filesystems that don't support O_TMPFILE
        if (fd == -1 && error_indicates_tmpfile_not_supported(errno))
        {
            char template_filename[] = "/dev/shm/wlcs-buffer-XXXXXX";
            fd = mkostemp(template_filename, O_CLOEXEC);
            if (fd != -1)
            {
                if (unlink(template_filename) < 0)
                {
                    close(fd);
                    fd = -1;
                }
            }
        }
    }

    if (fd == -1)
    {
        BOOST_THROW_EXCEPTION(
            std::system_error(errno, std::system_category(), "Failed to open temporary file"));
    }

    if (ftruncate(fd, size) == -1)
    {
        close(fd);
        BOOST_THROW_EXCEPTION(
            std::system_error(errno, std::system_category(), "Failed to resize temporary file"));
    }

    return mir::Fd{fd};
}
}

TEST(ShmBacking, can_get_rw_range_covering_whole_pool)
{
    using namespace testing;

    constexpr size_t const shm_size = 4000;
    auto shm_fd = make_shm_fd(shm_size);
    auto backing = std::make_shared<mir::RWShmBacking>(shm_fd, shm_size);

    auto mappable = mir::RWShmBacking::get_rw_range(backing, 0, shm_size);

    auto mapping = mappable->map_rw();

    constexpr unsigned char const fill_value{0xab};
    ::memset(mapping->data(), fill_value, shm_size);
    for(auto i = 0; i < shm_size; ++i)
    {
        EXPECT_THAT(mapping->data()[i], Eq(fill_value));
    }
}
