/*
 * Copyright © Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3,
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

#include "wl_data_device.h"
#include "wl_data_source.h"
#include "wl_surface.h"

#include "mir/frontend/drag_icon_controller.h"
#include "mir/scene/clipboard.h"
#include "mir/scene/session.h"
#include "mir/scene/surface.h"
#include "mir/shell/surface_specification.h"
#include "mir/wayland/client.h"
#include "mir/wayland/protocol_error.h"

namespace mf = mir::frontend;
namespace ms = mir::scene;
namespace mw = mir::wayland;
using namespace mir::geometry;

class mf::WlDataDevice::ClipboardObserver : public ms::ClipboardObserver
{
public:
    ClipboardObserver(WlDataDevice* device) : device{device}
    {
    }

private:
    void paste_source_set(std::shared_ptr<ms::ClipboardSource> const& source) override
    {
        if (device)
        {
            device.value().paste_source_set(source);
        }
    }

    wayland::Weak<WlDataDevice> const device;
};

class mf::WlDataDevice::Offer : public wayland::DataOffer
{
public:
    Offer(WlDataDevice* device, std::shared_ptr<scene::ClipboardSource> const& source);

    void accept(uint32_t serial, std::optional<std::string> const& mime_type) override
    {
        (void)serial, (void)mime_type;
    }

    void receive(std::string const& mime_type, mir::Fd fd) override;

    void finish() override
    {
    }

    void set_actions(uint32_t dnd_actions, uint32_t preferred_action) override
    {
        (void)dnd_actions, (void)preferred_action;
    }

private:
    friend mf::WlDataDevice;
    wayland::Weak<WlDataDevice> const device;
    std::shared_ptr<scene::ClipboardSource> const source;
};

mf::WlDataDevice::Offer::Offer(WlDataDevice* device, std::shared_ptr<scene::ClipboardSource> const& source) :
    mw::DataOffer(*device),
    device{device},
    source{source}
{
    device->send_data_offer_event(resource);
    for (auto const& type : source->mime_types())
    {
        send_offer_event(type);
    }
}

void mf::WlDataDevice::Offer::receive(std::string const& mime_type, mir::Fd fd)
{
    if (device && device.value().current_offer.is(*this))
    {
        source->initiate_send(mime_type, fd);
    }
}

mf::DragWlSurface::DragWlSurface(WlSurface* icon, std::shared_ptr<DragIconController> drag_icon_controller)
    : NullWlSurfaceRole(icon),
      surface{icon},
      drag_icon_controller{std::move(drag_icon_controller)}
{
    icon->set_role(this);

    auto spec = shell::SurfaceSpecification();
    spec.width = surface.value().buffer_size()->width;
    spec.height = surface.value().buffer_size()->height;
    spec.streams = std::vector<shell::StreamSpecification>{};
    spec.input_shape = std::vector<Rectangle>{};
    spec.depth_layer = mir_depth_layer_overlay;

    // TODO - handle
    surface.value().populate_surface_data(spec.streams.value(), spec.input_shape.value(), {});

    auto const& session = surface.value().session;

    shared_scene_surface =
        session->create_surface(session, wayland::Weak<WlSurface>(surface), spec, nullptr, nullptr);

    DragWlSurface::drag_icon_controller->set_drag_icon(shared_scene_surface);
}

mf::DragWlSurface::~DragWlSurface()
{
    if (surface)
    {
        surface.value().clear_role();

        if (shared_scene_surface)
        {
            auto const& session = surface.value().session;
            session->destroy_surface(shared_scene_surface);
        }
    }
}

auto mf::DragWlSurface::scene_surface() const -> std::optional<std::shared_ptr<scene::Surface>>
{
    return shared_scene_surface;
}

mf::WlDataDevice::WlDataDevice(
    wl_resource* new_resource,
    Executor& wayland_executor,
    scene::Clipboard& clipboard,
    mf::WlSeat& seat,
    std::shared_ptr<DragIconController> drag_icon_controller)
    : mw::DataDevice(new_resource, Version<3>()),
      clipboard{clipboard},
      seat{seat},
      clipboard_observer{std::make_shared<ClipboardObserver>(this)},
      drag_icon_controller{std::move(drag_icon_controller)}
{
    clipboard.register_interest(clipboard_observer, wayland_executor);
    // this will call focus_on() with the initial state
    seat.add_focus_listener(client, this);
}

mf::WlDataDevice::~WlDataDevice()
{
    clipboard.unregister_interest(*clipboard_observer);
    seat.remove_focus_listener(client, this);
}

void mf::WlDataDevice::set_selection(std::optional<wl_resource*> const& source, uint32_t serial)
{
    // TODO: verify serial
    (void)serial;
    if (source)
    {
        auto const wl_source = WlDataSource::from(source.value());
        wl_source->set_clipboard_paste_source();
    }
    else
    {
        clipboard.clear_paste_source();
    }
}

void mf::WlDataDevice::start_drag(
    std::optional<wl_resource*> const& source,
    wl_resource* origin,
    std::optional<wl_resource*> const& icon,
    uint32_t serial)
{
    // TODO: "The [origin surface] and client must have an active implicit grab that matches the serial"
    (void)source;
    if (!origin)
    {
        BOOST_THROW_EXCEPTION(
            mw::ProtocolError(resource, Error::role, "Origin surface does not exist."));
    }

    auto const drag_event = client->event_for(serial);

    if (!drag_event || !drag_event.value() || mir_event_get_type(drag_event.value().get()) != mir_event_type_input)
    {
        BOOST_THROW_EXCEPTION(
            mw::ProtocolError(resource, Error::role, "Serial does not correspond to an input event"));
    }

    auto const input_ev = mir_event_get_input_event(drag_event.value().get());
    if (mir_input_event_get_type(input_ev) != mir_input_event_type_pointer)
    {
        BOOST_THROW_EXCEPTION(
            mw::ProtocolError(resource, Error::role, "Serial does not correspond to a pointer event"));
    }

    // TODO {arg} start the drag logic

    if (icon)
    {
        auto const icon_surface = WlSurface::from(icon.value());

        drag_surface.emplace(icon_surface, drag_icon_controller);
    }
}

void mf::WlDataDevice::focus_on(WlSurface* surface)
{
    has_focus = static_cast<bool>(surface);
    paste_source_set(clipboard.paste_source());
}

void mf::WlDataDevice::paste_source_set(std::shared_ptr<scene::ClipboardSource> const& source)
{
    if (source && has_focus)
    {
        if (!current_offer || current_offer.value().source != source)
        {
            current_offer = wayland::make_weak(new Offer{this, source});
            send_selection_event(current_offer.value().resource);
        }
    }
    else
    {
        if (current_offer)
        {
            current_offer = {};
            send_selection_event(std::nullopt);
        }
    }
}
