#pragma once

#include "wayfire/geometry.hpp"
#include "wayfire/util.hpp"
#include <wayfire/toplevel.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/decorator.hpp>

namespace wf
{
class xdg_toplevel_t : public toplevel_t
{
  public:
    xdg_toplevel_t(wlr_xdg_toplevel *toplevel);
    void commit() override;
    void apply() override;
    void set_decoration(decorator_frame_t_t *frame);
    wf::geometry_t calculate_base_geometry() const;

  private:
    wf::wl_listener_wrapper on_surface_commit;
    wf::wl_listener_wrapper on_toplevel_destroy;
    wlr_xdg_toplevel *toplevel;
    decorator_frame_t_t *frame;
    wf::point_t wm_offset = {0, 0};


    void handle_surface_commit();
    uint32_t target_configure = 0;
};
}
