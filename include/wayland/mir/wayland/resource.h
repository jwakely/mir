/*
 * Copyright © 2022 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 or 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MIR_WAYLAND_RESOURCE_H_
#define MIR_WAYLAND_RESOURCE_H_

#include "lifetime_tracker.h"

struct wl_resource;
struct wl_client;

namespace mir
{
namespace wayland
{
class Client;

class Resource
    : public virtual LifetimeTracker
{
public:
    template<int V>
    struct Version
    {
    };

    Resource(wl_resource* resource);

    wl_resource* const resource;
    wl_client* const client;

private:
    std::shared_ptr<Client> shared_client;
};
}
}

#endif // MIR_WAYLAND_RESOURCE_H_
