#include <algorithm>
#include <cstring>
#include <cstdlib>

#include <wayfire/signal-definitions.hpp>
#include "wayfire/geometry.hpp"
#include "wayfire/util.hpp"
#include "wayfire/view.hpp"
#include "xdg-shell.hpp"
#include "wayfire/core.hpp"
#include "wayfire/debug.hpp"
#include <wayfire/util/log.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include "wayfire/output.hpp"
#include "wayfire/workspace-manager.hpp"
#include "wayfire/output-layout.hpp"
#include "view-impl.hpp"

static const uint32_t both_vert =
    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
static const uint32_t both_horiz =
    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;

class wayfire_layer_shell_view : public wf::view_interface_t
{
    wf::wl_listener_wrapper on_map;
    wf::wl_listener_wrapper on_unmap;
    wf::wl_listener_wrapper on_destroy;
    wf::wl_listener_wrapper on_new_popup;
    wf::wl_listener_wrapper on_commit_unmapped;

    wf::wl_listener_wrapper on_surface_commit;
    std::shared_ptr<wf::scene::wlr_surface_node_t> main_surface;
    std::unique_ptr<wf::wlr_surface_controller_t> surface_controller;

    /**
     * The bounding box of the view the last time it was rendered.
     *
     * This is used to damage the view when it is resized, because when a
     * transformer changes because the view is resized, we can't reliably
     * calculate the old view region to damage.
     */
    wf::geometry_t last_bounding_box{0, 0, 0, 0};

    /** The output geometry of the view */
    wf::geometry_t geometry{100, 100, 0, 0};

  protected:
    void initialize() override;
    std::string app_id;

  public:
    wlr_layer_surface_v1 *lsurface;
    wlr_layer_surface_v1_state prev_state;

    std::unique_ptr<wf::output_workarea_manager_t::anchored_area> anchored_area;
    void remove_anchored(bool reflow);

    wayfire_layer_shell_view(wlr_layer_surface_v1 *lsurf);
    virtual ~wayfire_layer_shell_view() = default;

    void map();
    void unmap();
    void commit();
    void close() override;

    /* Handle the destruction of the underlying wlroots object */
    void destroy();

    void configure(wf::geometry_t geometry);
    void set_output(wf::output_t *output) override;

    /** Calculate the target layer for this layer surface */
    wf::layer_t get_layer();

    /* Just pass to the default wlr surface implementation */
    bool is_mapped() const override
    {
        return priv->wsurface != nullptr;
    }

    std::string get_app_id() override final
    {
        return app_id;
    }

    std::string get_title() override final
    {
        return "layer-shell";
    }

    /* Functions which are further specialized for the different shells */
    void move(int x, int y) override
    {
        auto old_geometry = geometry;
        this->geometry.x = x;
        this->geometry.y = y;
        wf::emit_geometry_changed_signal(self(), old_geometry);
    }

    wf::geometry_t get_wm_geometry() override
    {
        return geometry;
    }

    wf::geometry_t get_output_geometry() override
    {
        return geometry;
    }

    wlr_surface *get_keyboard_focus_surface() override
    {
        if (is_mapped() && priv->keyboard_focus_enabled)
        {
            return priv->wsurface;
        }

        return NULL;
    }

    bool should_be_decorated() override
    {
        return false;
    }
};

wf::output_workarea_manager_t::anchored_edge anchor_to_edge(uint32_t edges)
{
    if (edges == ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)
    {
        return wf::output_workarea_manager_t::ANCHORED_EDGE_TOP;
    }

    if (edges == ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)
    {
        return wf::output_workarea_manager_t::ANCHORED_EDGE_BOTTOM;
    }

    if (edges == ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)
    {
        return wf::output_workarea_manager_t::ANCHORED_EDGE_LEFT;
    }

    if (edges == ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)
    {
        return wf::output_workarea_manager_t::ANCHORED_EDGE_RIGHT;
    }

    abort();
}

struct wf_layer_shell_manager
{
  private:
    wf::signal::connection_t<wf::output_layout_configuration_changed_signal> on_output_layout_changed =
        [=] (wf::output_layout_configuration_changed_signal *ev)
    {
        auto outputs = wf::get_core().output_layout->get_outputs();
        for (auto wo : outputs)
        {
            arrange_layers(wo);
        }
    };

    wf_layer_shell_manager()
    {
        wf::get_core().output_layout->connect(&on_output_layout_changed);
    }

  public:
    static wf_layer_shell_manager& get_instance()
    {
        /* Delay instantiation until first call, at which point core should
         * have been already initialized */
        static wf_layer_shell_manager instance;

        return instance;
    }

    using layer_t = std::vector<wayfire_layer_shell_view*>;
    static constexpr int COUNT_LAYERS = 4;
    layer_t layers[COUNT_LAYERS];

    void handle_map(wayfire_layer_shell_view *view)
    {
        layers[view->lsurface->current.layer].push_back(view);
        arrange_layers(view->get_output());
    }

    void remove_view_from_layer(wayfire_layer_shell_view *view, uint32_t layer)
    {
        auto& cont = layers[layer];
        auto it    = std::find(cont.begin(), cont.end(), view);
        if (it != cont.end())
        {
            cont.erase(it);
        }
    }

    void handle_move_layer(wayfire_layer_shell_view *view)
    {
        for (int i = 0; i < COUNT_LAYERS; i++)
        {
            remove_view_from_layer(view, i);
        }

        handle_map(view);
    }

    void handle_unmap(wayfire_layer_shell_view *view)
    {
        view->remove_anchored(false);
        remove_view_from_layer(view, view->lsurface->current.layer);
        arrange_layers(view->get_output());
    }

    layer_t filter_views(wf::output_t *output, int layer)
    {
        layer_t result;
        for (auto view : layers[layer])
        {
            if (view->get_output() == output)
            {
                result.push_back(view);
            }
        }

        return result;
    }

    layer_t filter_views(wf::output_t *output)
    {
        layer_t result;
        for (int i = 0; i < 4; i++)
        {
            auto layer_result = filter_views(output, i);
            result.insert(result.end(), layer_result.begin(), layer_result.end());
        }

        return result;
    }

    void set_exclusive_zone(wayfire_layer_shell_view *v)
    {
        int edges = v->lsurface->current.anchor;

        /* Special case we support */
        if (__builtin_popcount(edges) == 3)
        {
            if ((edges & both_horiz) == both_horiz)
            {
                edges ^= both_horiz;
            }

            if ((edges & both_vert) == both_vert)
            {
                edges ^= both_vert;
            }
        }

        if ((edges == 0) || (__builtin_popcount(edges) > 1))
        {
            LOGE(
                "Unsupported: layer-shell exclusive zone for surfaces anchored to 0, 2 or 4 edges");

            return;
        }

        if (!v->anchored_area)
        {
            v->anchored_area =
                std::make_unique<wf::output_workarea_manager_t::anchored_area>();
            v->anchored_area->reflowed =
                [v] (wf::geometry_t geometry, wf::geometry_t _)
            { v->configure(geometry); };
            /* Notice that the reflowed areas won't be changed until we call
             * reflow_reserved_areas(). However, by that time the information
             * in anchored_area will have been populated */
            v->get_output()->workarea->add_reserved_area(v->anchored_area.get());
        }

        v->anchored_area->edge = anchor_to_edge(edges);
        v->anchored_area->reserved_size = v->lsurface->current.exclusive_zone;
        v->anchored_area->real_size     = v->anchored_area->edge <=
            wf::output_workarea_manager_t::ANCHORED_EDGE_BOTTOM ?
            v->lsurface->current.desired_height : v->lsurface->current.desired_width;
    }

    void pin_view(wayfire_layer_shell_view *v, wf::geometry_t usable_workarea)
    {
        auto state  = &v->lsurface->current;
        auto bounds = v->lsurface->current.exclusive_zone < 0 ?
            v->get_output()->get_relative_geometry() : usable_workarea;

        wf::geometry_t box;
        box.x     = box.y = 0;
        box.width = state->desired_width;
        box.height = state->desired_height;

        if ((state->anchor & both_horiz) && (box.width == 0))
        {
            box.x     = bounds.x;
            box.width = bounds.width;
        } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT))
        {
            box.x = bounds.x;
        } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT))
        {
            box.x = bounds.x + (bounds.width - box.width);
        } else
        {
            box.x = bounds.x + ((bounds.width / 2) - (box.width / 2));
        }

        if ((state->anchor & both_vert) && (box.height == 0))
        {
            box.y = bounds.y;
            box.height = bounds.height;
        } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP))
        {
            box.y = bounds.y;
        } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM))
        {
            box.y = bounds.y + (bounds.height - box.height);
        } else
        {
            box.y = bounds.y + ((bounds.height / 2) - (box.height / 2));
        }

        v->configure(box);
    }

    void arrange_layer(wf::output_t *output, int layer)
    {
        auto views = filter_views(output, layer);

        /* First we need to put all views that have exclusive zone set.
         * The rest are then placed into the free area */
        for (auto v : views)
        {
            if (v->lsurface->pending.exclusive_zone > 0)
            {
                set_exclusive_zone(v);
            } else
            {
                /* Make sure the view doesn't have a reserved area anymore */
                v->remove_anchored(false);
            }
        }

        auto usable_workarea = output->workarea->get_workarea();
        for (auto v : views)
        {
            /* The protocol dictates that the values -1 and 0 for exclusive zone
             * mean that it doesn't have one */
            if (v->lsurface->pending.exclusive_zone < 1)
            {
                pin_view(v, usable_workarea);
            }
        }
    }

    void arrange_unmapped_view(wayfire_layer_shell_view *view)
    {
        if (view->lsurface->pending.exclusive_zone < 1)
        {
            return pin_view(view, view->get_output()->workarea->get_workarea());
        }

        set_exclusive_zone(view);
        view->get_output()->workarea->reflow_reserved_areas();
    }

    void arrange_layers(wf::output_t *output)
    {
        auto views = filter_views(output);

        arrange_layer(output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
        arrange_layer(output, ZWLR_LAYER_SHELL_V1_LAYER_TOP);
        arrange_layer(output, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);
        arrange_layer(output, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND);
        output->workarea->reflow_reserved_areas();
    }
};

wayfire_layer_shell_view::wayfire_layer_shell_view(wlr_layer_surface_v1 *lsurf) :
    wf::view_interface_t(), lsurface(lsurf)
{
    on_surface_commit.set_callback([&] (void*) { commit(); });
    this->main_surface = std::make_shared<wf::scene::wlr_surface_node_t>(lsurf->surface, true);

    LOGD("Create a layer surface: namespace ", lsurf->namespace_t,
        " layer ", lsurf->current.layer);

    role = wf::VIEW_ROLE_DESKTOP_ENVIRONMENT;
    std::memset(&this->prev_state, 0, sizeof(prev_state));
    sticky = true;

    /* If we already have an output set, then assign it before core assigns us
     * an output */
    if (lsurf->output)
    {
        auto wo = wf::get_core().output_layout->find_output(lsurf->output);
        set_output(wo);
    } else
    {
        set_output(wf::get_core().get_active_output());
    }
}

void wayfire_layer_shell_view::initialize()
{
    wf::view_interface_t::initialize();
    wf::dassert(get_output() != nullptr,
        "layer-shell views are always assigned an output!");

    lsurface->output = get_output()->handle;
    lsurface->data   = dynamic_cast<wf::view_interface_t*>(this);

    on_map.set_callback([&] (void*) { map(); });
    on_unmap.set_callback([&] (void*) { unmap(); });
    on_destroy.set_callback([&] (void*) { destroy(); });
    on_new_popup.set_callback([&] (void *data)
    {
        create_xdg_popup((wlr_xdg_popup*)data);
    });

    on_commit_unmapped.set_callback([&] (void*)
    {
        if (!this->get_output())
        {
            // This case can happen in the following scenario:
            // 1. Create output X
            // 2. Client opens a layer-shell surface Y on X
            // 3. X is destroyed, Y's output is now NULL
            // 4. Y commits
            return;
        }

        wf_layer_shell_manager::get_instance().arrange_unmapped_view(this);
    });

    on_map.connect(&lsurface->events.map);
    on_unmap.connect(&lsurface->events.unmap);
    on_destroy.connect(&lsurface->events.destroy);
    on_new_popup.connect(&lsurface->events.new_popup);
    on_commit_unmapped.connect(&lsurface->surface->events.commit);

    // Initial configure
    on_commit_unmapped.emit(NULL);
}

void wayfire_layer_shell_view::destroy()
{
    this->lsurface = nullptr;
    on_map.disconnect();
    on_unmap.disconnect();
    on_destroy.disconnect();
    on_new_popup.disconnect();

    remove_anchored(true);
    unref();
}

wf::layer_t wayfire_layer_shell_view::get_layer()
{
    static const std::vector<std::string> desktop_widget_ids = {
        "keyboard",
        "de-widget",
    };

    auto it = std::find(desktop_widget_ids.begin(),
        desktop_widget_ids.end(), nonull(lsurface->namespace_t));

    switch (lsurface->current.layer)
    {
      case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
        if (it != desktop_widget_ids.end())
        {
            return wf::LAYER_DESKTOP_WIDGET;
        }

        return wf::LAYER_LOCK;

      case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        return wf::LAYER_TOP;

      case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        return wf::LAYER_BOTTOM;

      case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        return wf::LAYER_BACKGROUND;

      default:
        throw std::domain_error("Invalid layer for layer surface!");
    }
}

void wayfire_layer_shell_view::map()
{
    {
        this->app_id = nonull(lsurface->namespace_t);
        wf::view_app_id_changed_signal data;
        data.view = self();
        emit(&data);
    }

    // Disconnect, from now on regular commits will work
    on_commit_unmapped.disconnect();

    priv->set_mapped_surface_contents(main_surface);
    priv->set_mapped(true);
    on_surface_commit.connect(&lsurface->surface->events.commit);

    /* Read initial data */
    priv->keyboard_focus_enabled = lsurface->current.keyboard_interactive;

    get_output()->workspace->add_view(self(), get_layer());
    wf_layer_shell_manager::get_instance().handle_map(this);
    if (lsurface->current.keyboard_interactive == 1)
    {
        get_output()->refocus();
    }
}

void wayfire_layer_shell_view::unmap()
{
    damage();

    emit_view_pre_unmap();
    priv->unset_mapped_surface_contents();
    on_surface_commit.disconnect();
    emit_view_unmap();
    priv->set_mapped(false);

    wf_layer_shell_manager::get_instance().handle_unmap(this);
}

void wayfire_layer_shell_view::commit()
{
    wf::dimensions_t new_size{lsurface->surface->current.width, lsurface->surface->current.height};
    if (new_size != wf::dimensions(geometry))
    {
        auto old_geometry = geometry;
        this->geometry.width  = new_size.width;
        this->geometry.height = new_size.height;
        wf::emit_geometry_changed_signal(self(), old_geometry);
        view_damage_raw(self(), last_bounding_box);
    }

    this->last_bounding_box = get_bounding_box();

    auto state = &lsurface->current;
    /* Update the keyboard focus enabled state. If a refocusing is needed, i.e
     * the view state changed, then this will happen when arranging layers */
    priv->keyboard_focus_enabled = state->keyboard_interactive;

    if (state->committed)
    {
        /* Update layer manually */
        if (prev_state.layer != state->layer)
        {
            get_output()->workspace->add_view(self(), get_layer());
            /* Will also trigger reflowing */
            wf_layer_shell_manager::get_instance().handle_move_layer(this);
        } else
        {
            /* Reflow reserved areas and positions */
            wf_layer_shell_manager::get_instance().arrange_layers(get_output());
        }

        if (prev_state.keyboard_interactive != state->keyboard_interactive)
        {
            if (state->keyboard_interactive == 1)
            {
                get_output()->refocus();
            }
        }

        prev_state = *state;
    }
}

void wayfire_layer_shell_view::set_output(wf::output_t *output)
{
    if (this->get_output() != output)
    {
        // Happens in two cases:
        // View's output is being destroyed, no point in reflowing
        // View is about to be mapped, no anchored area at all.
        this->remove_anchored(false);
    }

    wf::view_interface_t::set_output(output);
}

void wayfire_layer_shell_view::close()
{
    if (lsurface)
    {
        wlr_layer_surface_v1_destroy(lsurface);
    }
}

void wayfire_layer_shell_view::configure(wf::geometry_t box)
{
    auto state = &lsurface->current;
    if ((state->anchor & both_horiz) == both_horiz)
    {
        box.x     += state->margin.left;
        box.width -= state->margin.left + state->margin.right;
    } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT))
    {
        box.x += state->margin.left;
    } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT))
    {
        box.x -= state->margin.right;
    }

    if ((state->anchor & both_vert) == both_vert)
    {
        box.y += state->margin.top;
        box.height -= state->margin.top + state->margin.bottom;
    } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP))
    {
        box.y += state->margin.top;
    } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM))
    {
        box.y -= state->margin.bottom;
    }

    if ((box.width < 0) || (box.height < 0))
    {
        LOGE("layer-surface has calculated width and height < 0");
        close();
    }

    // TODO: transactions here could make sense, since we want to change x,y,w,h together, but have to wait
    // for the client to resize.
    move(box.x, box.y);
    wlr_layer_surface_v1_configure(lsurface, box.width, box.height);
}

void wayfire_layer_shell_view::remove_anchored(bool reflow)
{
    if (anchored_area)
    {
        get_output()->workarea->remove_reserved_area(anchored_area.get());
        anchored_area = nullptr;

        if (reflow)
        {
            get_output()->workarea->reflow_reserved_areas();
        }
    }
}

static wlr_layer_shell_v1 *layer_shell_handle;
void wf::init_layer_shell()
{
    static wf::wl_listener_wrapper on_created;

    layer_shell_handle = wlr_layer_shell_v1_create(wf::get_core().display);
    if (layer_shell_handle)
    {
        on_created.set_callback([] (void *data)
        {
            auto lsurf = static_cast<wlr_layer_surface_v1*>(data);
            wf::get_core().add_view(
                std::make_unique<wayfire_layer_shell_view>(lsurf));
        });

        on_created.connect(&layer_shell_handle->events.new_surface);
    }
}
