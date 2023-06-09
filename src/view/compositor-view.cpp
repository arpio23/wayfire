#include "wayfire/scene-render.hpp"
#include "wayfire/scene.hpp"
#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/compositor-view.hpp>
#include <wayfire/signal-definitions.hpp>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>
#include <wayfire/signal-provider.hpp>

static void render_colored_rect(const wf::render_target_t& fb,
    int x, int y, int w, int h, const wf::color_t& color)
{
    wf::color_t premultiply{
        color.r * color.a,
        color.g * color.a,
        color.b * color.a,
        color.a};

    OpenGL::render_rectangle({x, y, w, h}, premultiply,
        fb.get_orthographic_projection());
}

class wf::color_rect_view_t::color_rect_node_t : public wf::scene::floating_inner_node_t
{
    class color_rect_render_instance_t : public wf::scene::simple_render_instance_t<color_rect_node_t>
    {
      public:
        using simple_render_instance_t::simple_render_instance_t;
        void render(const wf::render_target_t& target, const wf::region_t& region) override
        {
            auto geometry = self->get_bounding_box();
            auto border   = self->view->border;
            auto _border_color = self->view->_border_color;
            auto _color = self->view->_color;

            OpenGL::render_begin(target);
            for (const auto& box : region)
            {
                target.logic_scissor(wlr_box_from_pixman_box(box));

                /* Draw the border, making sure border parts don't overlap, otherwise
                 * we will get wrong corners if border has alpha != 1.0 */
                // top
                render_colored_rect(target, geometry.x, geometry.y, geometry.width, border,
                    _border_color);
                // bottom
                render_colored_rect(target, geometry.x, geometry.y + geometry.height - border,
                    geometry.width, border, _border_color);
                // left
                render_colored_rect(target, geometry.x, geometry.y + border, border,
                    geometry.height - 2 * border, _border_color);
                // right
                render_colored_rect(target, geometry.x + geometry.width - border,
                    geometry.y + border, border, geometry.height - 2 * border, _border_color);

                /* Draw the inside of the rect */
                render_colored_rect(target, geometry.x + border, geometry.y + border,
                    geometry.width - 2 * border, geometry.height - 2 * border, _color);
            }

            OpenGL::render_end();
        }
    };

    color_rect_view_t *view;

  public:
    color_rect_node_t(color_rect_view_t *view) : scene::floating_inner_node_t(false)
    {
        this->view = view;
    }

    void gen_render_instances(std::vector<scene::render_instance_uptr>& instances,
        scene::damage_callback push_damage, wf::output_t *output) override
    {
        instances.push_back(std::make_unique<color_rect_render_instance_t>(this, push_damage, output));
    }

    wf::geometry_t get_bounding_box() override
    {
        return view->get_output_geometry();
    }
};

/* Implementation of color_rect_view_t */
wf::color_rect_view_t::color_rect_view_t() : wf::view_interface_t(std::make_shared<color_rect_node_t>(this))
{
    this->geometry   = {0, 0, 1, 1};
    this->_color     = {0, 0, 0, 1};
    this->border     = 0;
    this->_is_mapped = true;
}

void wf::color_rect_view_t::close()
{
    this->_is_mapped = false;

    emit_view_unmap();
    unref();
}

void wf::color_rect_view_t::set_color(wf::color_t color)
{
    this->_color = color;
    damage();
}

void wf::color_rect_view_t::set_border_color(wf::color_t border)
{
    this->_border_color = border;
    damage();
}

void wf::color_rect_view_t::set_border(int width)
{
    this->border = width;
    damage();
}

bool wf::color_rect_view_t::is_mapped() const
{
    return _is_mapped;
}

void wf::color_rect_view_t::move(int x, int y)
{
    damage();
    view_geometry_changed_signal data;
    data.old_geometry = get_wm_geometry();

    this->geometry.x = x;
    this->geometry.y = y;

    damage();
    emit(&data);
}

void wf::color_rect_view_t::resize(int w, int h)
{
    damage();
    view_geometry_changed_signal data;
    data.old_geometry = get_wm_geometry();

    this->geometry.width  = w;
    this->geometry.height = h;

    damage();
    emit(&data);
}

wf::geometry_t wf::color_rect_view_t::get_output_geometry()
{
    return geometry;
}

wlr_surface*wf::color_rect_view_t::get_keyboard_focus_surface()
{
    return nullptr;
}

bool wf::color_rect_view_t::is_focusable() const
{
    return false;
}

bool wf::color_rect_view_t::should_be_decorated()
{
    return false;
}

void wf::color_rect_view_t::initialize()
{
    view_interface_t::initialize();
    this->get_root_node()->set_enabled(true);
}
