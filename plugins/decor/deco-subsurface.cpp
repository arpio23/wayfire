#include "wayfire/geometry.hpp"
#include "wayfire/scene-input.hpp"
#include "wayfire/scene-operations.hpp"
#include "wayfire/scene-render.hpp"
#include "wayfire/scene.hpp"
#include <memory>
#define GLM_FORCE_RADIANS
#include <glm/gtc/matrix_transform.hpp>

#include <linux/input-event-codes.h>

#include <wayfire/nonstd/wlroots.hpp>
#include <wayfire/output.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/core.hpp>
#include <wayfire/decorator.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/signal-definitions.hpp>
#include "deco-subsurface.hpp"
#include "deco-layout.hpp"
#include "deco-theme.hpp"

#include <wayfire/plugins/common/cairo-util.hpp>

#include <cairo.h>

class simple_decoration_node_t : public wf::scene::node_t, public wf::pointer_interaction_t,
    public wf::touch_interaction_t
{
    wayfire_view view;
    wf::signal::connection_t<wf::view_title_changed_signal> title_set =
        [=] (wf::view_title_changed_signal *ev)
    {
        view->damage(); // trigger re-render
    };

    void update_title(int width, int height, double scale)
    {
        int target_width  = width * scale;
        int target_height = height * scale;

        if ((title_texture.tex.width != target_width) ||
            (title_texture.tex.height != target_height) ||
            (title_texture.current_text != view->get_title()))
        {
            auto surface = theme.render_text(view->get_title(),
                target_width, target_height);
            cairo_surface_upload_to_texture(surface, title_texture.tex);
            cairo_surface_destroy(surface);
            title_texture.current_text = view->get_title();
        }
    }

    struct
    {
        wf::simple_texture_t tex;
        std::string current_text = "";
    } title_texture;

    wf::decor::decoration_theme_t theme;
    wf::decor::decoration_layout_t layout;
    wf::region_t cached_region;

    wf::dimensions_t size;

  public:
    int current_thickness;
    int current_titlebar;

    simple_decoration_node_t(wayfire_view view) :
        node_t(false),
        theme{},
        layout{theme, [=] (wlr_box box) { wf::scene::damage_node(shared_from_this(), box + get_offset()); }}
    {
        this->view = view;
        view->connect(&title_set);

        // make sure to hide frame if the view is fullscreen
        update_decoration_size();
    }

    wf::point_t get_offset()
    {
        return {-current_thickness, -current_titlebar};
    }

    void render_title(const wf::render_target_t& fb,
        wf::geometry_t geometry)
    {
        update_title(geometry.width, geometry.height, fb.scale);
        OpenGL::render_texture(title_texture.tex.tex, fb, geometry,
            glm::vec4(1.0f), OpenGL::TEXTURE_TRANSFORM_INVERT_Y);
    }

    void render_scissor_box(const wf::render_target_t& fb, wf::point_t origin,
        const wlr_box& scissor)
    {
        /* Clear background */
        wlr_box geometry{origin.x, origin.y, size.width, size.height};
        theme.render_background(fb, geometry, scissor, view->activated);

        /* Draw title & buttons */
        auto renderables = layout.get_renderable_areas();
        for (auto item : renderables)
        {
            if (item->get_type() == wf::decor::DECORATION_AREA_TITLE)
            {
                OpenGL::render_begin(fb);
                fb.logic_scissor(scissor);
                render_title(fb, item->get_geometry() + origin);
                OpenGL::render_end();
            } else // button
            {
                item->as_button().render(fb,
                    item->get_geometry() + origin, scissor);
            }
        }
    }

    std::optional<wf::scene::input_node_t> find_node_at(const wf::pointf_t& at) override
    {
        wf::pointf_t local = at - wf::pointf_t{get_offset()};
        if (cached_region.contains_pointf(local))
        {
            return wf::scene::input_node_t{
                .node = this,
                .local_coords = local,
            };
        }

        return {};
    }

    pointer_interaction_t& pointer_interaction() override
    {
        return *this;
    }

    touch_interaction_t& touch_interaction() override
    {
        return *this;
    }

    class decoration_render_instance_t : public wf::scene::render_instance_t
    {
        simple_decoration_node_t *self;
        wf::scene::damage_callback push_damage;

        wf::signal::connection_t<wf::scene::node_damage_signal> on_surface_damage =
            [=] (wf::scene::node_damage_signal *data)
        {
            push_damage(data->region);
        };

      public:
        decoration_render_instance_t(simple_decoration_node_t *self, wf::scene::damage_callback push_damage)
        {
            this->self = self;
            this->push_damage = push_damage;
            self->connect(&on_surface_damage);
        }

        void schedule_instructions(std::vector<wf::scene::render_instruction_t>& instructions,
            const wf::render_target_t& target, wf::region_t& damage) override
        {
            auto our_region = self->cached_region + self->get_offset();
            wf::region_t our_damage = damage & our_region;
            if (!our_damage.empty())
            {
                instructions.push_back(wf::scene::render_instruction_t{
                    .instance = this,
                    .target   = target,
                    .damage   = std::move(our_damage),
                });
            }
        }

        void render(const wf::render_target_t& target,
            const wf::region_t& region) override
        {
            for (const auto& box : region)
            {
                self->render_scissor_box(target, self->get_offset(), wlr_box_from_pixman_box(box));
            }
        }
    };

    void gen_render_instances(std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_damage, wf::output_t *output = nullptr) override
    {
        instances.push_back(std::make_unique<decoration_render_instance_t>(this, push_damage));
    }

    wf::geometry_t get_bounding_box() override
    {
        if (view->fullscreen)
        {
            return view->get_wm_geometry();
        } else
        {
            return wf::construct_box(get_offset(), size);
        }
    }

    /* wf::compositor_surface_t implementation */
    void handle_pointer_enter(wf::pointf_t point) override
    {
        point -= wf::pointf_t{get_offset()};
        layout.handle_motion(point.x, point.y);
    }

    void handle_pointer_leave() override
    {
        layout.handle_focus_lost();
    }

    void handle_pointer_motion(wf::pointf_t to, uint32_t) override
    {
        to -= wf::pointf_t{get_offset()};
        handle_action(layout.handle_motion(to.x, to.y));
    }

    void handle_pointer_button(const wlr_pointer_button_event& ev) override
    {
        if (ev.button != BTN_LEFT)
        {
            return;
        }

        handle_action(layout.handle_press_event(ev.state == WLR_BUTTON_PRESSED));
    }

    void handle_action(wf::decor::decoration_layout_t::action_response_t action)
    {
        switch (action.action)
        {
          case wf::decor::DECORATION_ACTION_MOVE:
            return view->move_request();

          case wf::decor::DECORATION_ACTION_RESIZE:
            return view->resize_request(action.edges);

          case wf::decor::DECORATION_ACTION_CLOSE:
            return view->close();

          case wf::decor::DECORATION_ACTION_TOGGLE_MAXIMIZE:
            if (view->tiled_edges)
            {
                view->tile_request(0);
            } else
            {
                view->tile_request(wf::TILED_EDGES_ALL);
            }

            break;

          case wf::decor::DECORATION_ACTION_MINIMIZE:
            view->minimize_request(true);
            break;

          default:
            break;
        }
    }

    void handle_touch_down(uint32_t time_ms, int finger_id, wf::pointf_t position) override
    {
        handle_touch_motion(time_ms, finger_id, position);
        handle_action(layout.handle_press_event());
    }

    void handle_touch_up(uint32_t time_ms, int finger_id, wf::pointf_t lift_off_position) override
    {
        handle_action(layout.handle_press_event(false));
        layout.handle_focus_lost();
    }

    void handle_touch_motion(uint32_t time_ms, int finger_id, wf::pointf_t position) override
    {
        position -= wf::pointf_t{get_offset()};
        layout.handle_motion(position.x, position.y);
    }

    void resize(wf::dimensions_t dims)
    {
        view->damage();
        size = dims;
        layout.resize(size.width, size.height);
        if (!view->fullscreen)
        {
            this->cached_region = layout.calculate_region();
        }

        view->damage();
    }

    void update_decoration_size()
    {
        if (view->fullscreen)
        {
            current_thickness = 0;
            current_titlebar  = 0;
            this->cached_region.clear();
        } else
        {
            current_thickness = theme.get_border_size();
            current_titlebar  =
                theme.get_title_height() + theme.get_border_size();
            this->cached_region = layout.calculate_region();
        }
    }
};

class simple_decorator_t : public wf::decorator_frame_t_t
{
    wayfire_view view;
    std::shared_ptr<simple_decoration_node_t> deco;

  public:
    simple_decorator_t(wayfire_view view)
    {
        this->view = view;
        deco = std::make_shared<simple_decoration_node_t>(view);
        wf::scene::add_back(view->get_surface_root_node(), deco);
    }

    ~simple_decorator_t()
    {
        wf::scene::remove_child(deco);
    }

    /* frame implementation */
    virtual wf::geometry_t expand_wm_geometry(
        wf::geometry_t contained_wm_geometry) override
    {
        contained_wm_geometry.x     -= deco->current_thickness;
        contained_wm_geometry.y     -= deco->current_titlebar;
        contained_wm_geometry.width += 2 * deco->current_thickness;
        contained_wm_geometry.height +=
            deco->current_thickness + deco->current_titlebar;

        return contained_wm_geometry;
    }

    virtual void calculate_resize_size(
        int& target_width, int& target_height) override
    {
        target_width  -= 2 * deco->current_thickness;
        target_height -= deco->current_thickness + deco->current_titlebar;

        target_width  = std::max(target_width, 1);
        target_height = std::max(target_height, 1);
    }

    virtual void notify_view_activated(bool active) override
    {
        view->damage();
    }

    virtual void notify_view_resized(wf::geometry_t view_geometry) override
    {
        deco->resize(wf::dimensions(view_geometry));
    }

    virtual void notify_view_tiled() override
    {}

    virtual void notify_view_fullscreen() override
    {
        deco->update_decoration_size();

        if (!view->fullscreen)
        {
            notify_view_resized(view->get_wm_geometry());
        }
    }
};

void init_view(wayfire_view view)
{
    auto decor = std::make_unique<simple_decorator_t>(view);
    view->set_decoration(std::move(decor));
}

void deinit_view(wayfire_view view)
{
    view->set_decoration(nullptr);
}
