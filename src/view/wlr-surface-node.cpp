#include "wlr-surface-node.hpp"
#include "pixman.h"
#include "view/view-impl.hpp"
#include "wayfire/geometry.hpp"
#include "wayfire/render-manager.hpp"
#include "wayfire/scene-render.hpp"
#include "wayfire/scene.hpp"
#include "wlr-surface-pointer-interaction.cpp"
#include "wlr-surface-touch-interaction.cpp"
#include <memory>
#include <sstream>
#include <string>
#include <wayfire/signal-provider.hpp>
#include <wlr/util/box.h>

wf::scene::surface_state_t::surface_state_t(surface_state_t&& other)
{
    if (&other != this)
    {
        *this = std::move(other);
    }
}

wf::scene::surface_state_t& wf::scene::surface_state_t::operator =(surface_state_t&& other)
{
    if (current_buffer)
    {
        wlr_buffer_unlock(current_buffer);
    }

    current_buffer = other.current_buffer;
    texture = other.texture;
    accumulated_damage = other.accumulated_damage;
    size = other.size;
    src_viewport = other.src_viewport;

    other.current_buffer = NULL;
    other.texture = NULL;
    other.accumulated_damage.clear();
    other.src_viewport.reset();
    return *this;
}

void wf::scene::surface_state_t::merge_state(wlr_surface *surface)
{
    if (current_buffer)
    {
        wlr_buffer_unlock(current_buffer);
    }

    if (surface->buffer)
    {
        wlr_buffer_lock(&surface->buffer->base);
    }

    this->current_buffer = &surface->buffer->base;
    this->texture = surface->buffer->texture;
    this->size    = {surface->current.width, surface->current.height};

    if (surface->current.viewport.has_src)
    {
        wlr_fbox fbox;
        wlr_surface_get_buffer_source_box(surface, &fbox);
        this->src_viewport = fbox;
    } else
    {
        this->src_viewport.reset();
    }

    wf::region_t current_damage;
    wlr_surface_get_effective_damage(surface, current_damage.to_pixman());
    this->accumulated_damage |= current_damage;
}

wf::scene::surface_state_t::~surface_state_t()
{
    if (current_buffer)
    {
        wlr_buffer_unlock(current_buffer);
    }
}

wf::scene::wlr_surface_node_t::wlr_surface_node_t(wlr_surface *surface, bool autocommit) :
    node_t(false), autocommit(autocommit)
{
    this->surface = surface;
    this->ptr_interaction = std::make_unique<wlr_surface_pointer_interaction_t>(surface, this);
    this->tch_interaction = std::make_unique<wlr_surface_touch_interaction_t>(surface);

    this->on_surface_destroyed.set_callback([=] (void*)
    {
        this->surface = NULL;
        this->ptr_interaction = std::make_unique<pointer_interaction_t>();
        this->tch_interaction = std::make_unique<touch_interaction_t>();

        on_surface_commit.disconnect();
        on_surface_destroyed.disconnect();
    });

    this->on_surface_commit.set_callback([=] (void*)
    {
        if (!wlr_surface_has_buffer(this->surface) && this->visibility.empty())
        {
            send_frame_done();
        }

        if (this->autocommit)
        {
            surface_state_t state;
            state.merge_state(surface);
            this->apply_state(std::move(state));
        }

        for (auto& [wo, _] : visibility)
        {
            wo->render->schedule_redraw();
        }
    });

    on_surface_destroyed.connect(&surface->events.destroy);
    on_surface_commit.connect(&surface->events.commit);
    send_frame_done();
}

void wf::scene::wlr_surface_node_t::apply_state(surface_state_t&& state)
{
    const bool size_changed = current_state.size != state.size;
    this->current_state = std::move(state);
    wf::scene::damage_node(this, current_state.accumulated_damage);
    if (size_changed)
    {
        scene::update(this->shared_from_this(), scene::update_flag::GEOMETRY);
    }
}

std::optional<wf::scene::input_node_t> wf::scene::wlr_surface_node_t::find_node_at(const wf::pointf_t& at)
{
    if (!surface)
    {
        return {};
    }

    if (wlr_surface_point_accepts_input(surface, at.x, at.y))
    {
        wf::scene::input_node_t result;
        result.node = this;
        result.local_coords = at;
        return result;
    }

    return {};
}

std::string wf::scene::wlr_surface_node_t::stringify() const
{
    std::ostringstream name;
    name << "wlr-surface-node ";
    if (surface)
    {
        name << "surface";
    } else
    {
        name << "inert";
    }

    name << " " << stringify_flags();
    return name.str();
}

wf::pointer_interaction_t& wf::scene::wlr_surface_node_t::pointer_interaction()
{
    return *this->ptr_interaction;
}

wf::touch_interaction_t& wf::scene::wlr_surface_node_t::touch_interaction()
{
    return *this->tch_interaction;
}

void wf::scene::wlr_surface_node_t::send_frame_done()
{
    if (surface)
    {
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        wlr_surface_send_frame_done(surface, &now);
    }
}

class wf::scene::wlr_surface_node_t::wlr_surface_render_instance_t : public render_instance_t
{
    std::shared_ptr<wlr_surface_node_t> self;
    wf::signal::connection_t<wf::frame_done_signal> on_frame_done = [=] (wf::frame_done_signal *ev)
    {
        self->send_frame_done();
    };

    wf::output_t *visible_on;
    damage_callback push_damage;

    wf::signal::connection_t<node_damage_signal> on_surface_damage =
        [=] (node_damage_signal *data)
    {
        if (self->surface)
        {
            // Make sure to expand damage, because stretching the surface may cause additional damage.
            const float scale = self->surface->current.scale;
            const float output_scale = visible_on ? visible_on->handle->scale : 1.0;
            if (scale != output_scale)
            {
                data->region.expand_edges(std::ceil(std::abs(scale - output_scale)));
            }
        }

        push_damage(data->region);
    };

  public:
    wlr_surface_render_instance_t(std::shared_ptr<wlr_surface_node_t> self,
        damage_callback push_damage, wf::output_t *visible_on)
    {
        if (visible_on)
        {
            self->visibility[visible_on]++;
            if (self->surface)
            {
                wlr_surface_send_enter(self->surface, visible_on->handle);
            }
        }

        this->self = self;
        this->push_damage = push_damage;
        this->visible_on  = visible_on;
        self->connect(&on_surface_damage);
    }

    ~wlr_surface_render_instance_t()
    {
        if (visible_on)
        {
            self->visibility[visible_on]--;
            if ((self->visibility[visible_on] == 0) && self->surface)
            {
                self->visibility.erase(visible_on);
                wlr_surface_send_leave(self->surface, visible_on->handle);
            }
        }
    }

    void schedule_instructions(std::vector<render_instruction_t>& instructions,
        const wf::render_target_t& target, wf::region_t& damage) override
    {
        wf::region_t our_damage = damage & self->get_bounding_box();
        if (!our_damage.empty())
        {
            instructions.push_back(render_instruction_t{
                .instance = this,
                .target   = target,
                .damage   = std::move(our_damage),
            });

            if (self->surface)
            {
                pixman_region32_subtract(damage.to_pixman(), damage.to_pixman(),
                    &self->surface->opaque_region);
            }
        }
    }

    void render(const wf::render_target_t& target, const wf::region_t& region) override
    {
        if (!self->current_state.current_buffer)
        {
            return;
        }

        wf::geometry_t geometry = self->get_bounding_box();
        wf::texture_t texture{self->current_state.texture, self->current_state.src_viewport};

        OpenGL::render_begin(target);
        OpenGL::render_texture(texture, target, geometry, glm::vec4(1.f), OpenGL::RENDER_FLAG_CACHED);
        // use GL_NEAREST for integer scale.
        // GL_NEAREST makes scaled text blocky instead of blurry, which looks better
        // but only for integer scale.
        if (target.scale - floor(target.scale) < 0.001)
        {
            GL_CALL(glTexParameteri(texture.target, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        }

        for (const auto& rect : region)
        {
            target.logic_scissor(wlr_box_from_pixman_box(rect));
            OpenGL::draw_cached();
        }

        OpenGL::clear_cached();
        OpenGL::render_end();
    }

    void presentation_feedback(wf::output_t *output) override
    {
        if (self->surface)
        {
            wlr_presentation_surface_sampled_on_output(wf::get_core_impl().protocols.presentation,
                self->surface, output->handle);
        }
    }

    direct_scanout try_scanout(wf::output_t *output) override
    {
        if (!self->surface)
        {
            return direct_scanout::SKIP;
        }

        if (self->get_bounding_box() != output->get_relative_geometry())
        {
            return direct_scanout::OCCLUSION;
        }

        // Must have a wlr surface with the correct scale and transform
        auto wlr_surf = self->surface;
        if ((wlr_surf->current.scale != output->handle->scale) ||
            (wlr_surf->current.transform != output->handle->transform))
        {
            return direct_scanout::OCCLUSION;
        }

        // Finally, the opaque region must be the full surface.
        wf::region_t non_opaque = output->get_relative_geometry();
        non_opaque ^= wf::region_t{&wlr_surf->opaque_region};
        if (!non_opaque.empty())
        {
            return direct_scanout::OCCLUSION;
        }

        wlr_presentation_surface_sampled_on_output(
            wf::get_core().protocols.presentation, wlr_surf, output->handle);
        wlr_output_attach_buffer(output->handle, &wlr_surf->buffer->base);

        if (wlr_output_commit(output->handle))
        {
            return direct_scanout::SUCCESS;
        } else
        {
            return direct_scanout::OCCLUSION;
        }
    }

    void compute_visibility(wf::output_t *output, wf::region_t& visible) override
    {
        auto our_box = self->get_bounding_box();
        on_frame_done.disconnect();

        if (!(visible & our_box).empty())
        {
            // We are visible on the given output => send wl_surface.frame on output frame, so that clients
            // can draw the next frame.
            output->connect(&on_frame_done);
            // TODO: compute actually visible region and disable damage reporting for that region.
        }
    }
};

void wf::scene::wlr_surface_node_t::gen_render_instances(
    std::vector<render_instance_uptr>& instances, damage_callback damage,
    wf::output_t *output)
{
    instances.push_back(std::make_unique<wlr_surface_render_instance_t>(
        std::dynamic_pointer_cast<wlr_surface_node_t>(this->shared_from_this()), damage, output));
}

wf::geometry_t wf::scene::wlr_surface_node_t::get_bounding_box()
{
    return wf::construct_box({0, 0}, current_state.size);
}

wlr_surface*wf::scene::wlr_surface_node_t::get_surface() const
{
    return this->surface;
}

std::optional<wf::texture_t> wf::scene::wlr_surface_node_t::to_texture() const
{
    if (this->current_state.current_buffer)
    {
        return wf::texture_t{current_state.texture, current_state.src_viewport};
    }

    return {};
}
