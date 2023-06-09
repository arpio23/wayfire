#include "wayfire/signal-provider.hpp"
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/view.hpp>
#include <wayfire/core.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/signal-definitions.hpp>

class wayfire_place_window : public wf::per_output_plugin_instance_t
{
    wf::signal::connection_t<wf::view_mapped_signal> on_view_mapped = [=] (wf::view_mapped_signal *ev)
    {
        if ((ev->view->role != wf::VIEW_ROLE_TOPLEVEL) || ev->view->parent ||
            ev->view->fullscreen || ev->view->tiled_edges || ev->is_positioned)
        {
            return;
        }

        ev->is_positioned = true;
        auto workarea = output->workspace->get_workarea();

        std::string mode = placement_mode;
        if (mode == "cascade")
        {
            cascade(ev->view, workarea);
        } else if (mode == "maximize")
        {
            maximize(ev->view, workarea);
        } else if (mode == "random")
        {
            random(ev->view, workarea);
        } else
        {
            center(ev->view, workarea);
        }
    };

    wf::signal_connection_t workarea_changed_cb = [=] (wf::signal_data_t *data)
    {
        auto workarea = output->workspace->get_workarea();
        if ((cascade_x < workarea.x) ||
            (cascade_x > workarea.x + workarea.width))
        {
            cascade_x = workarea.x;
        }

        if ((cascade_y < workarea.y) ||
            (cascade_y > workarea.y + workarea.height))
        {
            cascade_y = workarea.y;
        }
    };

    wf::option_wrapper_t<std::string> placement_mode{"place/mode"};

    int cascade_x, cascade_y;

  public:
    void init() override
    {
        auto workarea = output->workspace->get_workarea();
        cascade_x = workarea.x;
        cascade_y = workarea.y;

        output->connect_signal("workarea-changed", &workarea_changed_cb);
        output->connect(&on_view_mapped);
    }

    void cascade(wayfire_view & view, wf::geometry_t workarea)
    {
        wf::geometry_t window = view->get_wm_geometry();

        if ((cascade_x + window.width > workarea.x + workarea.width) ||
            (cascade_y + window.height > workarea.y + workarea.height))
        {
            cascade_x = workarea.x;
            cascade_y = workarea.y;
        }

        view->move(cascade_x, cascade_y);

        cascade_x += workarea.width * .03;
        cascade_y += workarea.height * .03;
    }

    void random(wayfire_view & view, wf::geometry_t workarea)
    {
        wf::geometry_t window = view->get_wm_geometry();
        wf::geometry_t area;
        int pos_x, pos_y;

        area.x     = workarea.x;
        area.y     = workarea.y;
        area.width = workarea.width - window.width;
        area.height = workarea.height - window.height;

        if ((area.width <= 0) || (area.height <= 0))
        {
            center(view, workarea);

            return;
        }

        pos_x = rand() % area.width + area.x;
        pos_y = rand() % area.height + area.y;

        view->move(pos_x, pos_y);
    }

    void center(wayfire_view & view, wf::geometry_t workarea)
    {
        wf::geometry_t window = view->get_wm_geometry();
        window.x = workarea.x + (workarea.width / 2) - (window.width / 2);
        window.y = workarea.y + (workarea.height / 2) - (window.height / 2);
        view->move(window.x, window.y);
    }

    void maximize(wayfire_view & view, wf::geometry_t workarea)
    {
        view->tile_request(wf::TILED_EDGES_ALL);
    }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_place_window>);
