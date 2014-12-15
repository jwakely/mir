/*
 * Copyright © 2014 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Kevin DuBois <kevin.dubois@canonical.com>
 */

#ifndef MIR_GRAPHICS_ANDROID_HWC_WRAPPER_H_
#define MIR_GRAPHICS_ANDROID_HWC_WRAPPER_H_

#include "display_name.h"
#include <mir/geometry/size.h>
#include <hardware/hwcomposer.h>
#include <array>
#include <memory>

namespace mir
{
namespace graphics
{
namespace android
{

struct HwcAttribs
{
    geometry::Size pixel_size;
    geometry::Size dpi_mm;
    double vrefresh_hz;
};

struct HWCCallbacks;
class HwcWrapper
{
public:
    virtual ~HwcWrapper() = default;

    virtual void prepare(std::array<hwc_display_contents_1_t*, HWC_NUM_DISPLAY_TYPES> const&) const = 0;
    virtual void set(std::array<hwc_display_contents_1_t*, HWC_NUM_DISPLAY_TYPES> const&) const = 0;
    virtual void register_hooks(std::shared_ptr<HWCCallbacks> const& callbacks) = 0;
    virtual void vsync_signal_on(DisplayName) const = 0;
    virtual void vsync_signal_off(DisplayName) const = 0;
    virtual void display_on(DisplayName) const = 0;
    virtual void display_off(DisplayName) const = 0;
    virtual HwcAttribs display_attribs(DisplayName) const = 0;

protected:
    HwcWrapper() = default;
    HwcWrapper& operator=(HwcWrapper const&) = delete;
    HwcWrapper(HwcWrapper const&) = delete;
};
}
}
}

#endif /* MIR_GRAPHICS_ANDROID_HWC_WRAPPER_H_ */
