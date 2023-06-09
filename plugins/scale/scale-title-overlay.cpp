#include "scale.hpp"
#include "scale-title-overlay.hpp"
#include "wayfire/debug.hpp"
#include "wayfire/geometry.hpp"
#include "wayfire/output.hpp"
#include "wayfire/plugins/common/util.hpp"
#include "wayfire/render-manager.hpp"
#include "wayfire/scene-operations.hpp"
#include "wayfire/signal-definitions.hpp"
#include "wayfire/view-transform.hpp"

#include <memory>
#include <wayfire/opengl.hpp>
#include <wayfire/util/log.hpp>
#include <wayfire/plugins/common/cairo-util.hpp>
#include <wayfire/plugins/common/simple-texture.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/scene-render.hpp>

/**
 * Get the topmost parent of a view.
 */
static wayfire_view find_toplevel_parent(wayfire_view view)
{
    while (view->parent)
    {
        view = view->parent;
    }

    return view;
}

/**
 * Class storing an overlay with a view's title, only stored for parent views.
 */
struct view_title_texture_t : public wf::custom_data_t
{
    wayfire_view view;
    wf::cairo_text_t overlay;
    wf::cairo_text_t::params par;
    bool overflow = false;
    wayfire_view dialog; /* the texture should be rendered on top of this dialog */

    /**
     * Render the overlay text in our texture, cropping it to the size by
     * the given box.
     */
    void update_overlay_texture(wf::dimensions_t dim)
    {
        par.max_size = dim;
        update_overlay_texture();
    }

    void update_overlay_texture()
    {
        auto res = overlay.render_text(view->get_title(), par);
        overflow = res.width > overlay.tex.width;
    }

    wf::signal::connection_t<wf::view_title_changed_signal> view_changed_title =
        [=] (wf::view_title_changed_signal *ev)
    {
        if (overlay.tex.tex != (GLuint) - 1)
        {
            update_overlay_texture();
        }
    };

    view_title_texture_t(wayfire_view v, int font_size, const wf::color_t& bg_color,
        const wf::color_t& text_color, float output_scale) : view(v)
    {
        par.font_size    = font_size;
        par.bg_color     = bg_color;
        par.text_color   = text_color;
        par.exact_size   = true;
        par.output_scale = output_scale;

        view->connect(&view_changed_title);
    }
};

namespace wf
{
namespace scene
{
class title_overlay_node_t : public node_t
{
  public:
    enum class position
    {
        TOP,
        CENTER,
        BOTTOM,
    };

    /* save the transformed view, since we need it in the destructor */
    wayfire_view view;
    /* the position on the screen we currently render to */
    wf::geometry_t geometry{0, 0, 0, 0};
    scale_show_title_t& parent;
    unsigned int text_height; /* set in the constructor, should not change */
    position pos = position::CENTER;
    /* Whether we are currently rendering the overlay by this transformer.
     * Set in the pre-render hook and used in the render function. */
    bool overlay_shown = false;

  private:
    /**
     * Gets the overlay texture stored with the given view.
     */
    view_title_texture_t& get_overlay_texture(wayfire_view view)
    {
        auto data = view->get_data<view_title_texture_t>();
        if (!data)
        {
            auto new_data = new view_title_texture_t(view, parent.title_font_size,
                parent.bg_color, parent.text_color, parent.output->handle->scale);
            view->store_data<view_title_texture_t>(std::unique_ptr<view_title_texture_t>(
                new_data));
            return *new_data;
        }

        return *data.get();
    }

    wf::geometry_t get_scaled_bbox(wayfire_view v)
    {
        auto tr = v->get_transformed_node()->
            get_transformer<wf::scene::view_2d_transformer_t>("scale");

        if (tr)
        {
            auto wm_geometry = v->get_wm_geometry();
            return get_bbox_for_node(tr, wm_geometry);
        }

        return v->get_bounding_box();
    }

    wf::dimensions_t find_maximal_title_size()
    {
        wf::dimensions_t max_size = {0, 0};
        auto parent = find_toplevel_parent(view);

        for (auto v : parent->enumerate_views())
        {
            if (!v->get_transformed_node()->is_enabled())
            {
                continue;
            }

            auto bbox = get_scaled_bbox(v);
            max_size.width  = std::max(max_size.width, bbox.width);
            max_size.height = std::max(max_size.height, bbox.height);
        }

        return max_size;
    }

    /**
     * Check if this view should display an overlay.
     */
    bool should_have_overlay()
    {
        if (this->parent.show_view_title_overlay ==
            scale_show_title_t::title_overlay_t::NEVER)
        {
            return false;
        }

        auto parent = find_toplevel_parent(view);

        if ((this->parent.show_view_title_overlay ==
             scale_show_title_t::title_overlay_t::MOUSE) &&
            (this->parent.last_title_overlay != parent))
        {
            return false;
        }

        while (!parent->children.empty())
        {
            parent = parent->children[0];
        }

        return view == parent;
    }

    wf::effect_hook_t pre_render = [=] () -> void
    {
        if (!should_have_overlay())
        {
            overlay_shown = false;
            return;
        }

        overlay_shown = true;
        auto box = find_maximal_title_size();
        auto output_scale = parent.output->handle->scale;

        /**
         * regenerate the overlay texture in the following cases:
         * 1. Output's scale changed
         * 2. The overlay does not fit anymore
         * 3. The overlay previously did not fit, but there is more space now
         * TODO: check if this wastes too high CPU power when views are being
         * animated and maybe redraw less frequently
         */
        auto& tex = get_overlay_texture(find_toplevel_parent(view));
        if ((tex.overlay.tex.tex == (GLuint) - 1) ||
            (output_scale != tex.par.output_scale) ||
            (tex.overlay.tex.width > box.width * output_scale) ||
            (tex.overflow &&
             (tex.overlay.tex.width < std::floor(box.width * output_scale))))
        {
            this->do_push_damage(get_bounding_box());
            tex.par.output_scale = output_scale;
            tex.update_overlay_texture({box.width, box.height});
        }

        geometry.width  = tex.overlay.tex.width / output_scale;
        geometry.height = tex.overlay.tex.height / output_scale;

        auto bbox = get_scaled_bbox(view);
        geometry.x = bbox.x + bbox.width / 2 - geometry.width / 2;
        switch (pos)
        {
          case position::TOP:
            geometry.y = bbox.y;
            break;

          case position::CENTER:
            geometry.y = bbox.y + bbox.height / 2 - geometry.height / 2;
            break;

          case position::BOTTOM:
            geometry.y = bbox.y + bbox.height - geometry.height / 2;
            break;
        }

        this->do_push_damage(get_bounding_box());
    };

    wf::output_t *output;

  public:
    title_overlay_node_t(
        wayfire_view view_, position pos_, scale_show_title_t& parent_) :
        node_t(false), view(view_), parent(parent_), pos(pos_)
    {
        auto parent = find_toplevel_parent(view);
        auto& title = get_overlay_texture(parent);

        if (title.overlay.tex.tex != (GLuint) - 1)
        {
            text_height = (unsigned int)std::ceil(
                title.overlay.tex.height / title.par.output_scale);
        } else
        {
            text_height =
                wf::cairo_text_t::measure_height(title.par.font_size, true);
        }

        this->output = view->get_output();
        output->render->add_effect(&pre_render, OUTPUT_EFFECT_PRE);
    }

    ~title_overlay_node_t()
    {
        output->render->rem_effect(&pre_render);
        view->erase_data<view_title_texture_t>();
    }

    void gen_render_instances(
        std::vector<render_instance_uptr>& instances,
        damage_callback push_damage, wf::output_t *output) override;

    void do_push_damage(wf::region_t updated_region)
    {
        node_damage_signal ev;
        ev.region = updated_region;
        this->emit(&ev);
    }

    std::string stringify() const override
    {
        return "scale-title-overlay";
    }

    wf::geometry_t get_bounding_box() override
    {
        return geometry;
    }
};

class title_overlay_render_instance_t : public render_instance_t
{
    wf::signal::connection_t<node_damage_signal> on_node_damaged =
        [=] (node_damage_signal *ev)
    {
        push_to_parent(ev->region);
    };

    title_overlay_node_t *self;
    damage_callback push_to_parent;

  public:
    title_overlay_render_instance_t(title_overlay_node_t *self,
        damage_callback push_dmg)
    {
        this->self = self;
        this->push_to_parent = push_dmg;
        self->connect(&on_node_damaged);
    }

    void schedule_instructions(
        std::vector<render_instruction_t>& instructions,
        const wf::render_target_t& target, wf::region_t& damage)
    {
        if (!self->overlay_shown || !self->view->has_data<view_title_texture_t>())
        {
            return;
        }

        // We want to render ourselves only, the node does not have children
        instructions.push_back(render_instruction_t{
                    .instance = this,
                    .target   = target,
                    .damage   = damage & self->get_bounding_box(),
                });
    }

    void render(const wf::render_target_t& target,
        const wf::region_t& region)
    {
        auto& title = *self->view->get_data<view_title_texture_t>();
        auto tr     = self->view->get_transformed_node()
            ->get_transformer<wf::scene::view_2d_transformer_t>("scale");

        GLuint tex = title.overlay.tex.tex;

        if (tex == (GLuint) - 1)
        {
            /* this should not happen */
            return;
        }

        auto ortho = target.get_orthographic_projection();
        OpenGL::render_begin(target);
        for (const auto& box : region)
        {
            target.logic_scissor(wlr_box_from_pixman_box(box));
            OpenGL::render_transformed_texture(tex, self->geometry, ortho,
                {1.0f, 1.0f, 1.0f, tr->alpha}, OpenGL::TEXTURE_TRANSFORM_INVERT_Y);
        }

        OpenGL::render_end();
    }
};

void title_overlay_node_t::gen_render_instances(
    std::vector<render_instance_uptr>& instances,
    damage_callback push_damage, wf::output_t *output)
{
    instances.push_back(std::make_unique<title_overlay_render_instance_t>(
        this, push_damage));
}
}
}

scale_show_title_t::scale_show_title_t() :
    view_filter{[this] (auto)
    {
        update_title_overlay_opt();
    }},

    scale_end{[this] (wf::signal_data_t*)
    {
        show_view_title_overlay = title_overlay_t::NEVER;
        last_title_overlay = nullptr;

        post_absolute_motion.disconnect();
        post_motion.disconnect();
    }
},

add_title_overlay{[this] (wf::signal_data_t *data)
    {
        const std::string& opt = show_view_title_overlay_opt;
        if (opt == "never")
        {
            /* TODO: support changing this option while scale is running! */
            return;
        }

        using namespace wf::scene;

        const std::string& pos_opt = title_position;
        title_overlay_node_t::position pos = title_overlay_node_t::position::CENTER;
        if (pos_opt == "top")
        {
            pos = title_overlay_node_t::position::TOP;
        } else if (pos_opt == "bottom")
        {
            pos = title_overlay_node_t::position::BOTTOM;
        }

        auto signal = static_cast<scale_transformer_added_signal*>(data);
        auto tr     = signal->view->get_transformed_node()->get_transformer("scale");
        auto parent = std::dynamic_pointer_cast<wf::scene::floating_inner_node_t>(
            tr->parent()->shared_from_this());

        auto node = std::make_shared<title_overlay_node_t>(signal->view, pos, *this);
        wf::scene::add_front(parent, node);
    }
},

rem_title_overlay{[] (wf::signal_data_t *data)
    {
        using namespace wf::scene;
        auto signal = static_cast<scale_transformer_removed_signal*>(data);

        node_t *tr =
            signal->view->get_transformed_node()->get_transformer("scale").get();

        while (tr)
        {
            for (auto& ch : tr->get_children())
            {
                if (dynamic_cast<title_overlay_node_t*>(ch.get()))
                {
                    remove_child(ch);
                    break;
                }
            }

            tr = tr->parent();
        }
    }
},

post_motion{[=] (auto)
    {
        update_title_overlay_mouse();
    }
},

post_absolute_motion{[=] (auto)
    {
        update_title_overlay_mouse();
    }
}

{}

void scale_show_title_t::init(wf::output_t *output)
{
    this->output = output;
    output->connect_signal("scale-filter", &view_filter);
    output->connect_signal("scale-transformer-added", &add_title_overlay);
    output->connect_signal("scale-transformer-removed", &rem_title_overlay);
    output->connect_signal("scale-end", &scale_end);
}

void scale_show_title_t::fini()
{
    post_motion.disconnect();
    post_absolute_motion.disconnect();
}

void scale_show_title_t::update_title_overlay_opt()
{
    const std::string& tmp = show_view_title_overlay_opt;
    if (tmp == "all")
    {
        show_view_title_overlay = title_overlay_t::ALL;
    } else if (tmp == "mouse")
    {
        show_view_title_overlay = title_overlay_t::MOUSE;
    } else
    {
        show_view_title_overlay = title_overlay_t::NEVER;
    }

    if (show_view_title_overlay == title_overlay_t::MOUSE)
    {
        update_title_overlay_mouse();

        post_absolute_motion.disconnect();
        post_motion.disconnect();
        wf::get_core().connect(&post_absolute_motion);
        wf::get_core().connect(&post_motion);
    }
}

void scale_show_title_t::update_title_overlay_mouse()
{
    wayfire_view v =
        scale_find_view_at(wf::get_core().get_cursor_position(), output);

    if (v)
    {
        v = find_toplevel_parent(v);

        if (v->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            v = nullptr;
        }
    }

    if (v != last_title_overlay)
    {
        if (last_title_overlay)
        {
            last_title_overlay->damage();
        }

        last_title_overlay = v;
        if (v)
        {
            v->damage();
        }
    }
}
