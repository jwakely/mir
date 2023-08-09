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

#include "input_method_v1.h"
#include "mir/scene/text_input_hub.h"
#include "mir/shell/surface_specification.h"
#include "mir/shell/shell.h"
#include "mir/scene/session.h"
#include "mir/log.h"
#include "wl_surface.h"
#include "output_manager.h"
#include "window_wl_surface_role.h"
#include "input_method_common.h"
#include "input-method-unstable-v1_wrapper.cpp" // TODO: Super temporary. Can't figure out why link is broken.
#include <deque>

namespace mf = mir::frontend;
namespace ms = mir::scene;
namespace mw = mir::wayland;

/// Handles activation and deactivation of the InputMethodContextV1
class mf::InputMethodV1::Instance : wayland::InputMethodV1
{
private:
    class InputMethodContextV1;

public:
    Instance(wl_resource* new_resource,
         std::shared_ptr<scene::TextInputHub> const text_input_hub,
         std::shared_ptr<Executor> const wayland_executor)
        : InputMethodV1{new_resource, Version<1>()},
          text_input_hub(text_input_hub),
          state_observer{std::make_shared<StateObserver>(this)}
    {
        text_input_hub->register_interest(state_observer, *wayland_executor);
    }

    ~Instance()
    {
        text_input_hub->unregister_interest(*state_observer);
    }

    void activated(
        scene::TextInputStateSerial serial,
        bool new_input_field,
        scene::TextInputState const& state)
    {
        // Create the new context if we have a new field or if we're not yet activated
        if (!is_activated || new_input_field)
        {
            deactivated();

            context = std::make_shared<InputMethodContextV1>(
                this,
                text_input_hub);
            is_activated = true;
            cached_state = ms::TextInputState{};
            send_activate_event(context->resource);
            context->send_reset_event();
        }

        if (cached_state.change_cause != state.change_cause)
        {
            cached_state.change_cause = state.change_cause;
            if (cached_state.change_cause.has_value())
            {
                switch (cached_state.change_cause.value())
                {
                    case mir::scene::TextInputChangeCause::other:
                        context->reset_pending_change();
                        context->send_reset_event();
                        break;
                    default:
                        break;
                }
            }
        }

        // Notify about the surrounding text changing
        if (cached_state.surrounding_text != state.surrounding_text ||
            cached_state.cursor != state.cursor ||
            cached_state.anchor != state.anchor)
        {
            cached_state.surrounding_text = state.surrounding_text;
            cached_state.cursor = state.cursor;
            cached_state.anchor = state.anchor;
            context->send_surrounding_text_event(
                state.surrounding_text.value_or(""),
                state.cursor.value_or(0),
                state.anchor.value_or(0));
        }

        // Notify about the new content type
        if (cached_state.content_hint != state.content_hint || cached_state.content_purpose != state.content_purpose)
        {
            cached_state.content_hint = state.content_hint;
            cached_state.content_purpose = state.content_purpose;
            context->send_content_type_event(
                mir_to_wayland_content_hint(state.content_hint.value_or(ms::TextInputContentHint::none)),
                mir_to_wayland_content_purpose(state.content_purpose.value_or(ms::TextInputContentPurpose::normal)));
        }

        context->add_serial(serial);
    }

    void deactivated()
    {
        if (is_activated)
        {
            is_activated = false;
            if (context)
            {
                context->cleanup();
                auto resource = context->resource;
                context_on_deathbed = context;
                send_deactivate_event(resource);
                context = nullptr;
            }
        }
    }

private:
    struct StateObserver : ms::TextInputStateObserver
    {
        StateObserver(Instance* input_method)
            : input_method{input_method}
        {
        }

        void activated(
            scene::TextInputStateSerial serial,
            bool new_input_field,
            scene::TextInputState const& state) override
        {
            input_method->activated(serial, new_input_field, state);
        }

        void deactivated() override
        {
            input_method->deactivated();
        }

        Instance* const input_method;
    };

    /// https://wayland.app/protocols/input-method-unstable-v1
    /// The InputMethodContextV1 is associated with a single TextInput
    /// and will be destroyed when that text input is no longer receiving text.
    class InputMethodContextV1 : public wayland::InputMethodContextV1
    {
    public:
        InputMethodContextV1(
            mf::InputMethodV1::Instance* method,
            std::shared_ptr<scene::TextInputHub> const text_input_hub)
            : wayland::InputMethodContextV1(*static_cast<wayland::InputMethodV1*>(method)),
              text_input_hub(text_input_hub)
        {
        }

        ~InputMethodContextV1()
        {
        }

        void add_serial(ms::TextInputStateSerial serial)
        {
            send_commit_state_event(done_event_count);
            serials.push_back({done_event_count, serial});
            while (serials.size() > max_remembered_serials)
            {
                serials.pop_front();
            }
            done_event_count++;
        }

        void cleanup()
        {
            if (!change.fallback_commit.empty())
            {
                // TODO: Commit the fallback
            }
        }

        void reset_pending_change()
        {
            change = InputMethodV1Change();
        }

    private:
        enum class InputMethodV1ChangeWaitingStatus
        {
            none,
            commit_string,
            preedit_string
        };

        struct InputMethodV1Change
        {
            std::string fallback_commit;
            scene::TextInputChange pending_change{{}};
            InputMethodV1ChangeWaitingStatus waiting_status;

            void reset()
            {
                pending_change = ms::TextInputChange{{}};
                waiting_status = InputMethodV1ChangeWaitingStatus::none;
                fallback_commit.clear();
            }

            /// TODO: Verify if this behavior is correct
            /// If a change is waiting to be sent to the text input BUT
            /// we encounter another change beforehand, we will nullify the
            /// pending change.
            void check_waiting_status(InputMethodV1ChangeWaitingStatus expected)
            {
                if (waiting_status != InputMethodV1ChangeWaitingStatus::none
                    && expected != waiting_status)
                {
                    reset();
                }
            }
        };

        /// The input method client will be sending up the "done_count" as their serial.
        /// We will map this done_count back to the serial of the text input.
        /// \param done_count Serial of the input method
        /// \returns A text input state serial
        auto find_serial(uint32_t done_count) const -> std::optional<ms::TextInputStateSerial>
        {
            // Loop in reverse order because the serial we're looking for will generally be at the end
            for (auto it = serials.rbegin(); it != serials.rend(); it++)
            {
                if (it->first == done_count)
                {
                    return it->second;
                }
            }
            return std::nullopt;
        }

        void on_text_changed(uint32_t serial)
        {
            auto const mir_serial = find_serial(serial);
            if (mir_serial)
            {
                change.pending_change.serial = *mir_serial;
                text_input_hub->text_changed(change.pending_change);
            }
            else
            {
                log_warning("%s: invalid commit serial %d", interface_name, serial);
            }

            change.reset();
        }

        /// Commit the provided string to the text input immediately.
        /// \param serial The serial
        /// \param text Text to commit
        void commit_string(uint32_t serial, const std::string &text) override
        {
            change.check_waiting_status(InputMethodV1ChangeWaitingStatus::commit_string);
            change.pending_change.commit_text = text;
            on_text_changed(serial);
        }

        /// Creates a "tentative" input value that will be overridden when a string is "committed".
        /// This let's the user see what they're typing, while also allowing for autocomplete.
        /// \param serial The serial
        /// \param text The preedit text
        /// \param commit Fallback text in the event that a user unfocuses the text input
        void preedit_string(uint32_t serial, const std::string &text, const std::string &commit) override
        {
            change.check_waiting_status(InputMethodV1ChangeWaitingStatus::preedit_string);
            change.pending_change.preedit_text = text;
            change.fallback_commit = commit;
            on_text_changed(serial);
        }

        /// Only defined for text inputs v1 and v2. This request will be fulfilled when
        //  preedit_string is next called.
        void preedit_styling(uint32_t index, uint32_t length, uint32_t style) override
        {
            change.pending_change.preedit_style = { index, length, style };
            change.waiting_status = InputMethodV1ChangeWaitingStatus::preedit_string;
        }

        /// Set the cursor position to the index. This request will be fulfilled when
        /// preedit_string is next called.
        /// \param index New cursor position
        void preedit_cursor(int32_t index) override
        {
            change.pending_change.preedit_cursor_begin = index;
            change.pending_change.preedit_cursor_end = index;
            change.waiting_status = InputMethodV1ChangeWaitingStatus::preedit_string;
        }

        /// Deletes the text starting from index to length. This request will be fulfilled when
        //  commit_string is next called.
        /// \param index Start of the deletion
        /// \param length Length of the deletion
        void delete_surrounding_text(int32_t index, uint32_t length) override
        {
            // First, we move the cursor position to index
            change.pending_change.preedit_cursor_begin = index;
            change.pending_change.preedit_cursor_end = index;

            // Then we set delete_after to 0 (representing the start of the cursor position)
            // and delete_before to the length
            change.pending_change.delete_after = 0;
            change.pending_change.delete_before = length;

            change.waiting_status = InputMethodV1ChangeWaitingStatus::commit_string;
        }

        /// Changes the cursor position. This request will be fulfilled when
        //  commit_string is next called.
        /// \param index New cursor position
        /// \param anchor New anchor position. An anchor is used to define the end of the selected region of text.
        void cursor_position(int32_t index, int32_t anchor) override
        {
            change.pending_change.preedit_cursor_begin = index;
            change.pending_change.preedit_cursor_end = index + anchor;
            change.waiting_status = InputMethodV1ChangeWaitingStatus::commit_string;
        }

        void modifiers_map(struct wl_array */*map*/) override
        {
            // TODO
        }

        void keysym(uint32_t serial, uint32_t time, uint32_t sym, uint32_t state, uint32_t modifiers) override
        {
            change.pending_change.keysym = { time, sym, state, modifiers };
            on_text_changed(serial);
        }

        void grab_keyboard(struct wl_resource */*keyboard*/) override
        {
            // TODO
        }

        void key(uint32_t /*serial*/, uint32_t /*time*/, uint32_t /*key*/, uint32_t /*state*/) override
        {
            // TODO
        }

        void modifiers(uint32_t /*serial*/, uint32_t /*mods_depressed*/, uint32_t /*mods_latched*/, uint32_t /*mods_locked*/,
            uint32_t /*group*/) override
        {
            // TODO
        }

        void language(uint32_t /*serial*/, const std::string &/*language*/) override
        {
            // TODO
        }

        void text_direction(uint32_t /*serial*/, uint32_t /*direction*/) override
        {
            // TODO
        }

        std::shared_ptr<scene::TextInputHub> const text_input_hub;
        InputMethodV1Change change;
        static size_t constexpr max_remembered_serials{10};
        std::deque<std::pair<uint32_t , ms::TextInputStateSerial>> serials;
        uint32_t done_event_count{0};
    };

    std::shared_ptr<scene::TextInputHub> const text_input_hub;
    std::shared_ptr<StateObserver> const state_observer;
    bool is_activated = false;
    std::shared_ptr<InputMethodContextV1> context = nullptr;
    std::shared_ptr<InputMethodContextV1> context_on_deathbed = nullptr;
    scene::TextInputState cached_state{};
};

mf::InputMethodV1::InputMethodV1(
    wl_display *display,
    std::shared_ptr<Executor> const wayland_executor,
    std::shared_ptr<scene::TextInputHub> const text_input_hub)
    : Global(display, Version<1>()),
      display(display),
      wayland_executor(wayland_executor),
      text_input_hub(text_input_hub)
{
}

void mf::InputMethodV1::bind(wl_resource *new_resource)
{
    new Instance{new_resource, text_input_hub, wayland_executor};
}

class mf::InputPanelV1::Instance : wayland::InputPanelV1
{
public:
    Instance(
        std::shared_ptr<Executor> const wayland_executor,
        std::shared_ptr<shell::Shell> const shell,
        WlSeat* seat,
        OutputManager* const output_manager,
        wl_resource* new_resource,
        std::shared_ptr<scene::TextInputHub> const text_input_hub)
        : InputPanelV1{new_resource, Version<1>()},
          wayland_executor{wayland_executor},
          shell{shell},
          seat{seat},
          output_manager{output_manager},
          text_input_hub{text_input_hub}
    {}

private:

    class InputPanelSurfaceV1 : public wayland::InputPanelSurfaceV1,
        public WindowWlSurfaceRole
    {
    public:
        InputPanelSurfaceV1(
            wl_resource* id,
            std::shared_ptr<Executor> const wayland_executor,
            WlSeat* seat,
            WlSurface* surface,
            std::shared_ptr<shell::Shell> shell,
            OutputManager* const output_manager,
            std::shared_ptr<scene::TextInputHub> text_input_hub)
            : wayland::InputPanelSurfaceV1(id, Version<1>()),
              WindowWlSurfaceRole(
                  *wayland_executor,
                  seat,
                  wayland::InputPanelSurfaceV1::client,
                  surface,
                  shell,
                  output_manager),
              output_manager(output_manager),
              text_input_hub{text_input_hub},
              state_observer{std::make_shared<StateObserver>(this)}
        {
            text_input_hub->register_interest(state_observer, *wayland_executor);
            mir::shell::SurfaceSpecification spec;
            spec.state = mir_window_state_attached;
            spec.attached_edges = MirPlacementGravity::mir_placement_gravity_south;
            spec.type = MirWindowType::mir_window_type_inputmethod;
            spec.depth_layer = MirDepthLayer::mir_depth_layer_below;
            apply_spec(spec);
        }

        ~InputPanelSurfaceV1() override
        {
            text_input_hub->unregister_interest(*state_observer);
        }

        void show()
        {
            mir::shell::SurfaceSpecification spec;
            spec.state = mir_window_state_attached;
            apply_spec(spec);
        }

        void hide()
        {
            mir::shell::SurfaceSpecification spec;
            spec.state = mir_window_state_hidden;
            apply_spec(spec);
        }

        virtual void handle_state_change(MirWindowState /*new_state*/) override {};
        virtual void handle_active_change(bool /*is_now_active*/) override {};
        virtual void handle_resize(
            std::optional<geometry::Point> const& /*new_top_left*/,
            geometry::Size const& /*new_size*/) override {};
        virtual void handle_close_request() override {};
        virtual void handle_commit() override {};
        virtual void destroy_role() const override
        {
            wl_resource_destroy(resource);
        };

    private:
        struct StateObserver : ms::TextInputStateObserver
        {
            StateObserver(InputPanelSurfaceV1* input_panel_surface)
                : input_panel_surface{input_panel_surface}
            {
            }

            void activated(
                scene::TextInputStateSerial,
                bool,
                scene::TextInputState const&) override
            {
            }

            void deactivated() override
            {
            }

            void show_input_panel() override
            {
                input_panel_surface->show();
            }

            void hide_input_panel() override
            {
                input_panel_surface->hide();
            }

            InputPanelSurfaceV1* const input_panel_surface;
        };

        void set_toplevel(struct wl_resource* output, uint32_t /*position*/) override
        {
            mir::shell::SurfaceSpecification spec;
            auto const output_id = output_manager->output_id_for(output);
            spec.output_id = output_id.value();
            spec.state = mir_window_state_attached;
            spec.attached_edges = MirPlacementGravity::mir_placement_gravity_south;
            spec.type = MirWindowType::mir_window_type_inputmethod;
            spec.depth_layer = MirDepthLayer::mir_depth_layer_below;
            apply_spec(spec);
        }

        void set_overlay_panel() override
        {
            // TODO: Doesn't seemed to be called by maliit
        }

        OutputManager* output_manager;
        std::shared_ptr<scene::TextInputHub> const text_input_hub;
        std::shared_ptr<StateObserver> const state_observer;
    };

    void get_input_panel_surface(wl_resource* id, wl_resource* surface) override
    {
        new InputPanelSurfaceV1(
            id,
            wayland_executor,
            seat,
            WlSurface::from(surface),
            shell,
            output_manager,
            text_input_hub);
    }

    std::shared_ptr<Executor> const wayland_executor;
    std::shared_ptr<shell::Shell> const shell;
    WlSeat* seat;
    OutputManager* const output_manager;
    std::shared_ptr<scene::TextInputHub> const text_input_hub;
};

mf::InputPanelV1::InputPanelV1(
    wl_display* display,
    std::shared_ptr<Executor> const wayland_executor,
    std::shared_ptr<shell::Shell> const shell,
    WlSeat* seat,
    OutputManager* const output_manager,
    std::shared_ptr<scene::TextInputHub> const text_input_hub)
    : Global(display, Version<1>()),
      display{display},
      wayland_executor{wayland_executor},
      shell{shell},
      seat{seat},
      output_manager{output_manager},
      text_input_hub{text_input_hub}
{}

void mf::InputPanelV1::bind(wl_resource *new_resource)
{
    new Instance{
        wayland_executor,
        shell,
        seat,
        output_manager,
        new_resource,
        text_input_hub
    };
}