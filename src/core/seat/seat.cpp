#include "seat.hpp"
#include "cursor.hpp"
#include "wayfire/compositor-view.hpp"
#include "wayfire/geometry.hpp"
#include "wayfire/opengl.hpp"
#include "../core-impl.hpp"
#include "../view/view-impl.hpp"
#include "keyboard.hpp"
#include "pointer.hpp"
#include "touch.hpp"
#include "input-manager.hpp"
#include "wayfire/render-manager.hpp"
#include "wayfire/output-layout.hpp"
#include <wayfire/util/log.hpp>
#include "wayfire/scene-input.hpp"
#include "wayfire/signal-definitions.hpp"
#include <wayfire/nonstd/wlroots.hpp>

#include "drag-icon.hpp"
#include "wayfire/util.hpp"

/* ----------------------- wf::seat_t implementation ------------------------ */
wf::seat_t::seat_t()
{
    seat     = wlr_seat_create(wf::get_core().display, "default");
    cursor   = std::make_unique<wf::cursor_t>(this);
    lpointer = std::make_unique<wf::pointer_t>(
        wf::get_core_impl().input, nonstd::make_observer(this));
    touch = std::make_unique<wf::touch_interface_t>(cursor->cursor, seat,
        [] (const wf::pointf_t& global) -> wf::scene::node_ptr
    {
        auto value = wf::get_core().scene()->find_node_at(global);
        return value ? value->node->shared_from_this() : nullptr;
    });

    request_start_drag.set_callback([&] (void *data)
    {
        auto ev = static_cast<wlr_seat_request_start_drag_event*>(data);
        validate_drag_request(ev);
    });
    request_start_drag.connect(&seat->events.request_start_drag);

    start_drag.set_callback([&] (void *data)
    {
        auto d = static_cast<wlr_drag*>(data);
        if (d->icon)
        {
            this->drag_icon = std::make_unique<wf::drag_icon_t>(d->icon);
        }

        this->drag_active = true;
        end_drag.set_callback([&] (void*)
        {
            this->drag_active = false;
            end_drag.disconnect();
        });
        end_drag.connect(&d->events.destroy);
    });
    start_drag.connect(&seat->events.start_drag);

    request_set_selection.set_callback([&] (void *data)
    {
        auto ev = static_cast<wlr_seat_request_set_selection_event*>(data);
        wlr_seat_set_selection(wf::get_core().get_current_seat(),
            ev->source, ev->serial);
    });
    request_set_selection.connect(&seat->events.request_set_selection);

    request_set_primary_selection.set_callback([&] (void *data)
    {
        auto ev =
            static_cast<wlr_seat_request_set_primary_selection_event*>(data);
        wlr_seat_set_primary_selection(wf::get_core().get_current_seat(),
            ev->source, ev->serial);
    });
    request_set_primary_selection.connect(
        &seat->events.request_set_primary_selection);

    on_new_device = [&] (wf::input_device_added_signal *ev)
    {
        switch (ev->device->get_wlr_handle()->type)
        {
          case WLR_INPUT_DEVICE_KEYBOARD:
            this->keyboards.emplace_back(std::make_unique<wf::keyboard_t>(
                ev->device->get_wlr_handle()));
            if (this->current_keyboard == nullptr)
            {
                set_keyboard(keyboards.back().get());
            }

            break;

          case WLR_INPUT_DEVICE_TOUCH:
          case WLR_INPUT_DEVICE_POINTER:
          case WLR_INPUT_DEVICE_TABLET_TOOL:
            this->cursor->add_new_device(ev->device->get_wlr_handle());
            break;

          default:
            break;
        }

        update_capabilities();
    };

    on_remove_device = [&] (wf::input_device_removed_signal *ev)
    {
        auto dev = ev->device->get_wlr_handle();
        if (dev->type == WLR_INPUT_DEVICE_KEYBOARD)
        {
            bool current_kbd_destroyed = false;
            if (current_keyboard && (current_keyboard->device == dev))
            {
                current_kbd_destroyed = true;
            }

            auto it = std::remove_if(keyboards.begin(), keyboards.end(),
                [=] (const std::unique_ptr<wf::keyboard_t>& kbd)
            {
                return kbd->device == dev;
            });

            keyboards.erase(it, keyboards.end());

            if (current_kbd_destroyed && keyboards.size())
            {
                set_keyboard(keyboards.front().get());
            } else
            {
                set_keyboard(nullptr);
            }
        }

        update_capabilities();
    };

    wf::get_core().connect(&on_new_device);
    wf::get_core().connect(&on_remove_device);
}

void wf::seat_t::update_capabilities()
{
    uint32_t caps = 0;
    for (const auto& dev : wf::get_core().get_input_devices())
    {
        switch (dev->get_wlr_handle()->type)
        {
          case WLR_INPUT_DEVICE_KEYBOARD:
            caps |= WL_SEAT_CAPABILITY_KEYBOARD;
            break;

          case WLR_INPUT_DEVICE_POINTER:
            caps |= WL_SEAT_CAPABILITY_POINTER;
            break;

          case WLR_INPUT_DEVICE_TOUCH:
            caps |= WL_SEAT_CAPABILITY_TOUCH;
            break;

          default:
            break;
        }
    }

    wlr_seat_set_capabilities(seat, caps);
}

void wf::seat_t::validate_drag_request(wlr_seat_request_start_drag_event *ev)
{
    auto seat = wf::get_core().get_current_seat();

    if (wlr_seat_validate_pointer_grab_serial(seat, ev->origin, ev->serial))
    {
        wlr_seat_start_pointer_drag(seat, ev->drag, ev->serial);
        return;
    }

    struct wlr_touch_point *point;
    if (wlr_seat_validate_touch_grab_serial(seat, ev->origin, ev->serial, &point))
    {
        wlr_seat_start_touch_drag(seat, ev->drag, ev->serial, point);
        return;
    }

    LOGD("Ignoring start_drag request: ",
        "could not validate pointer or touch serial ", ev->serial);
    wlr_data_source_destroy(ev->drag->source);
}

void wf::seat_t::update_drag_icon()
{
    if (drag_icon)
    {
        drag_icon->update_position();
    }
}

void wf::seat_t::set_keyboard(wf::keyboard_t *keyboard)
{
    this->current_keyboard = keyboard;
    wlr_seat_set_keyboard(seat, keyboard ? wlr_keyboard_from_input_device(keyboard->device) : NULL);
}

void wf::seat_t::break_mod_bindings()
{
    for (auto& kbd : this->keyboards)
    {
        kbd->mod_binding_key = 0;
    }
}

uint32_t wf::seat_t::get_modifiers()
{
    return current_keyboard ? current_keyboard->get_modifiers() : 0;
}

void wf::seat_t::force_release_keys()
{
    if (this->keyboard_focus)
    {
        // Release currently pressed buttons
        for (auto key : this->pressed_keys)
        {
            wlr_keyboard_key_event ev;
            ev.keycode = key;
            ev.state   = WL_KEYBOARD_KEY_STATE_RELEASED;
            ev.update_state = true;
            ev.time_msec    = get_current_time();
            this->keyboard_focus->keyboard_interaction().handle_keyboard_key(ev);
        }
    }
}

void wf::seat_t::transfer_grab(wf::scene::node_ptr grab_node, bool retain_pressed_state)
{
    if (this->keyboard_focus == grab_node)
    {
        return;
    }

    force_release_keys();
    if (!retain_pressed_state)
    {
        pressed_keys.clear();
    }

    if (this->keyboard_focus)
    {
        this->keyboard_focus->keyboard_interaction().handle_keyboard_leave();
    }

    this->keyboard_focus = grab_node;
    grab_node->keyboard_interaction().handle_keyboard_enter();

    wf::keyboard_focus_changed_signal data;
    data.new_focus = grab_node;
    wf::get_core().emit(&data);
}

void wf::seat_t::set_keyboard_focus(wf::scene::node_ptr new_focus)
{
    if (this->keyboard_focus == new_focus)
    {
        return;
    }

    force_release_keys();
    pressed_keys.clear();
    if (this->keyboard_focus)
    {
        this->keyboard_focus->keyboard_interaction().handle_keyboard_leave();
    }

    this->keyboard_focus = new_focus;
    if (new_focus)
    {
        new_focus->keyboard_interaction().handle_keyboard_enter();
    }

    wf::keyboard_focus_changed_signal data;
    data.new_focus = new_focus;
    wf::get_core().emit(&data);
}

namespace wf
{
wlr_input_device*input_device_t::get_wlr_handle()
{
    return handle;
}

bool input_device_t::set_enabled(bool enabled)
{
    if (enabled == is_enabled())
    {
        return true;
    }

    if (!wlr_input_device_is_libinput(handle))
    {
        return false;
    }

    auto dev = wlr_libinput_get_device_handle(handle);
    assert(dev);

    libinput_device_config_send_events_set_mode(dev,
        enabled ? LIBINPUT_CONFIG_SEND_EVENTS_ENABLED :
        LIBINPUT_CONFIG_SEND_EVENTS_DISABLED);

    return true;
}

bool input_device_t::is_enabled()
{
    /* Currently no support for enabling/disabling non-libinput devices */
    if (!wlr_input_device_is_libinput(handle))
    {
        return true;
    }

    auto dev = wlr_libinput_get_device_handle(handle);
    assert(dev);

    auto mode = libinput_device_config_send_events_get_mode(dev);

    return mode == LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
}

input_device_t::input_device_t(wlr_input_device *handle)
{
    this->handle = handle;
}
} // namespace wf

wf::input_device_impl_t::input_device_impl_t(wlr_input_device *dev) :
    wf::input_device_t(dev)
{
    on_destroy.set_callback([&] (void*)
    {
        wf::get_core_impl().input->handle_input_destroyed(this->get_wlr_handle());
    });
    on_destroy.connect(&dev->events.destroy);
}

static wf::pointf_t to_local_recursive(wf::scene::node_t *node, wf::pointf_t point)
{
    if (node->parent())
    {
        return node->to_local(to_local_recursive(node->parent(), point));
    }

    return node->to_local(point);
}

wf::pointf_t get_node_local_coords(wf::scene::node_t *node,
    const wf::pointf_t& point)
{
    return to_local_recursive(node, point);
}

bool is_grabbed_node_alive(wf::scene::node_ptr node)
{
    auto cur = node.get();
    while (cur)
    {
        if (!cur->is_enabled())
        {
            return false;
        }

        if (cur == wf::get_core().scene().get())
        {
            return true;
        }

        cur = cur->parent();
    }

    // We did not reach the scenegraph root => we cannot focus the node anymore, it was removed.
    return false;
}
