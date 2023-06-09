#include "wayfire/core.hpp"
#include "wayfire/signal-definitions.hpp"
#include <wayfire/output-layout.hpp>
#include "wayfire/view.hpp"
#include <memory>
#include <wayfire/plugin.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>

#include <wayfire/gtk-shell.hpp>

class wayfire_foreign_toplevel;
using foreign_toplevel_map_type = std::map<wayfire_view, std::unique_ptr<wayfire_foreign_toplevel>>;

class wayfire_foreign_toplevel
{
    wayfire_view view;
    wlr_foreign_toplevel_handle_v1 *handle;
    foreign_toplevel_map_type *view_to_toplevel;

  public:
    wayfire_foreign_toplevel(wayfire_view view, wlr_foreign_toplevel_handle_v1 *handle,
        foreign_toplevel_map_type *view_to_toplevel)
    {
        this->view   = view;
        this->handle = handle;
        this->view_to_toplevel = view_to_toplevel;

        init_request_handlers();
        toplevel_handle_v1_close_request.connect(&handle->events.request_close);
        toplevel_handle_v1_maximize_request.connect(&handle->events.request_maximize);
        toplevel_handle_v1_minimize_request.connect(&handle->events.request_minimize);
        toplevel_handle_v1_activate_request.connect(&handle->events.request_activate);
        toplevel_handle_v1_fullscreen_request.connect(&handle->events.request_fullscreen);
        toplevel_handle_v1_set_rectangle_request.connect(&handle->events.set_rectangle);

        toplevel_send_title();
        toplevel_send_app_id();
        toplevel_send_state();
        toplevel_update_output(view->get_output(), true);

        view->connect(&on_title_changed);
        view->connect(&on_app_id_changed);

        view->connect(&on_set_output);
        view->connect(&on_tiled);
        view->connect(&on_minimized);
        view->connect(&on_fullscreen);
        view->connect(&on_activated);
        view->connect(&on_parent_changed);
    }

    ~wayfire_foreign_toplevel()
    {
        wlr_foreign_toplevel_handle_v1_destroy(handle);
    }

  private:
    void toplevel_send_title()
    {
        wlr_foreign_toplevel_handle_v1_set_title(handle, view->get_title().c_str());
    }

    void toplevel_send_app_id()
    {
        std::string app_id;

        auto default_app_id     = view->get_app_id();
        auto gtk_shell_app_id   = get_gtk_shell_app_id(view);
        std::string app_id_mode = wf::option_wrapper_t<std::string>("workarounds/app_id_mode");

        if ((app_id_mode == "gtk-shell") && (gtk_shell_app_id.length() > 0))
        {
            app_id = gtk_shell_app_id;
        } else if (app_id_mode == "full")
        {
            if (view->get_wlr_surface() && wlr_surface_is_xwayland_surface(view->get_wlr_surface()))
            {
                auto xw_surface = wlr_xwayland_surface_from_wlr_surface(view->get_wlr_surface());
                gtk_shell_app_id = nonull(xw_surface->instance);
            }

            app_id = default_app_id + " " + gtk_shell_app_id;
        } else
        {
            app_id = default_app_id;
        }

        wlr_foreign_toplevel_handle_v1_set_app_id(handle, app_id.c_str());
    }

    void toplevel_send_state()
    {
        wlr_foreign_toplevel_handle_v1_set_maximized(handle, view->tiled_edges == wf::TILED_EDGES_ALL);
        wlr_foreign_toplevel_handle_v1_set_activated(handle, view->activated);
        wlr_foreign_toplevel_handle_v1_set_minimized(handle, view->minimized);
        wlr_foreign_toplevel_handle_v1_set_fullscreen(handle, view->fullscreen);

        /* update parent as well */
        auto it = view_to_toplevel->find(view->parent);
        if (it == view_to_toplevel->end())
        {
            wlr_foreign_toplevel_handle_v1_set_parent(handle, nullptr);
        } else
        {
            wlr_foreign_toplevel_handle_v1_set_parent(handle, it->second->handle);
        }
    }

    void toplevel_update_output(wf::output_t *output, bool enter)
    {
        if (output && enter)
        {
            wlr_foreign_toplevel_handle_v1_output_enter(handle, output->handle);
        }

        if (output && !enter)
        {
            wlr_foreign_toplevel_handle_v1_output_leave(handle, output->handle);
        }
    }

    wf::signal::connection_t<wf::view_title_changed_signal> on_title_changed = [=] (auto)
    {
        toplevel_send_title();
    };

    wf::signal::connection_t<wf::view_app_id_changed_signal> on_app_id_changed = [=] (auto)
    {
        toplevel_send_app_id();
    };

    wf::signal::connection_t<wf::view_set_output_signal> on_set_output = [=] (wf::view_set_output_signal *ev)
    {
        toplevel_update_output(ev->output, false);
        toplevel_update_output(view->get_output(), true);
    };

    wf::signal::connection_t<wf::view_minimized_signal> on_minimized = [=] (auto)
    {
        toplevel_send_state();
    };

    wf::signal::connection_t<wf::view_fullscreen_signal> on_fullscreen = [=] (auto)
    {
        toplevel_send_state();
    };

    wf::signal::connection_t<wf::view_tiled_signal> on_tiled = [=] (auto)
    {
        toplevel_send_state();
    };

    wf::signal::connection_t<wf::view_activated_state_signal> on_activated = [=] (auto)
    {
        toplevel_send_state();
    };

    wf::signal::connection_t<wf::view_parent_changed_signal> on_parent_changed = [=] (auto)
    {
        toplevel_send_state();
    };

    wf::wl_listener_wrapper toplevel_handle_v1_maximize_request;
    wf::wl_listener_wrapper toplevel_handle_v1_activate_request;
    wf::wl_listener_wrapper toplevel_handle_v1_minimize_request;
    wf::wl_listener_wrapper toplevel_handle_v1_set_rectangle_request;
    wf::wl_listener_wrapper toplevel_handle_v1_fullscreen_request;
    wf::wl_listener_wrapper toplevel_handle_v1_close_request;

    void init_request_handlers()
    {
        toplevel_handle_v1_maximize_request.set_callback([&] (void *data)
        {
            auto ev = static_cast<wlr_foreign_toplevel_handle_v1_maximized_event*>(data);
            view->tile_request(ev->maximized ? wf::TILED_EDGES_ALL : 0);
        });

        toplevel_handle_v1_minimize_request.set_callback([&] (void *data)
        {
            auto ev = static_cast<wlr_foreign_toplevel_handle_v1_minimized_event*>(data);
            view->minimize_request(ev->minimized);
        });

        toplevel_handle_v1_activate_request.set_callback([&] (auto)
        {
            view->focus_request();
        });

        toplevel_handle_v1_close_request.set_callback([&] (auto)
        {
            view->close();
        });

        toplevel_handle_v1_set_rectangle_request.set_callback([&] (void *data)
        {
            auto ev = static_cast<wlr_foreign_toplevel_handle_v1_set_rectangle_event*>(data);
            auto surface = wf::wl_surface_to_wayfire_view(ev->surface->resource);
            if (!surface)
            {
                LOGE("Setting minimize hint to unknown surface. Wayfire currently"
                     "supports only setting hints relative to views.");
                return;
            }

            handle_minimize_hint(view.get(), surface.get(), {ev->x, ev->y, ev->width, ev->height});
        });

        toplevel_handle_v1_fullscreen_request.set_callback([&] (
            void *data)
        {
            auto ev = static_cast<wlr_foreign_toplevel_handle_v1_fullscreen_event*>(data);
            auto wo = wf::get_core().output_layout->find_output(ev->output);
            view->fullscreen_request(wo, ev->fullscreen);
        });
    }

    void handle_minimize_hint(wf::view_interface_t *view, wf::view_interface_t *relative_to,
        const wlr_box& hint)
    {
        if (relative_to->get_output() != view->get_output())
        {
            LOGE("Minimize hint set to surface on a different output, " "problems might arise");
            /* TODO: translate coordinates in case minimize hint is on another output */
        }

        view->set_minimize_hint(hint + wf::origin(relative_to->get_output_geometry()));
    }
};

class wayfire_foreign_toplevel_protocol_impl : public wf::plugin_interface_t
{
  public:
    void init() override
    {
        toplevel_manager = wlr_foreign_toplevel_manager_v1_create(wf::get_core().display);
        wf::get_core().connect(&on_view_mapped);
        wf::get_core().connect(&on_view_unmapped);
    }

    void fini() override
    {}

    bool is_unloadable() override
    {
        return false;
    }

  private:
    wf::signal::connection_t<wf::view_mapped_signal> on_view_mapped = [=] (wf::view_mapped_signal *ev)
    {
        if (ev->view->role == wf::VIEW_ROLE_TOPLEVEL)
        {
            auto handle = wlr_foreign_toplevel_handle_v1_create(toplevel_manager);
            handle_for_view[ev->view] =
                std::make_unique<wayfire_foreign_toplevel>(ev->view, handle, &handle_for_view);
        }
    };

    wf::signal::connection_t<wf::view_unmapped_signal> on_view_unmapped = [=] (wf::view_unmapped_signal *ev)
    {
        handle_for_view.erase(ev->view);
    };

    wlr_foreign_toplevel_manager_v1 *toplevel_manager;
    std::map<wayfire_view, std::unique_ptr<wayfire_foreign_toplevel>> handle_for_view;
};

DECLARE_WAYFIRE_PLUGIN(wayfire_foreign_toplevel_protocol_impl);
