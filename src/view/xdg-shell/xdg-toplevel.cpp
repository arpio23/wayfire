#include "xdg-toplevel.hpp"
#include "wayfire/core.hpp"
#include <memory>
#include <wayfire/txn/transaction-manager.hpp>
#include <wlr/util/edges.h>
#include "wayfire/decorator.hpp"
#include "wayfire/geometry.hpp"
#include "wayfire/txn/transaction-object.hpp"

wf::xdg_toplevel_t::xdg_toplevel_t(wlr_xdg_toplevel *toplevel,
    std::shared_ptr<wf::scene::wlr_surface_node_t> main_surface)
{
    this->toplevel     = toplevel;
    this->main_surface = main_surface;

    on_surface_commit.set_callback([&] (void*) { handle_surface_commit(); });
    on_surface_commit.connect(&toplevel->base->surface->events.commit);

    on_toplevel_destroy.set_callback([&] (void*)
    {
        this->toplevel = NULL;
        on_toplevel_destroy.disconnect();
        on_surface_commit.disconnect();
        emit_ready();
    });
    on_toplevel_destroy.connect(&toplevel->base->events.destroy);
}

void wf::xdg_toplevel_t::request_native_size()
{
    // This will trigger a client-driven transaction
    wlr_xdg_toplevel_set_size(toplevel, 0, 0);
}

void wf::xdg_toplevel_t::commit()
{
    this->pending_ready = true;
    _committed = _pending;
    LOGC(TXNI, this, ": committing toplevel state geometry=", _pending.geometry);

    if (wf::dimensions(_pending.geometry) == wf::dimensions(_current.geometry))
    {
        emit_ready();
        return;
    }

    if (!this->toplevel)
    {
        // No longer mapped => we can do whatever
        emit_ready();
        return;
    }

    auto margins = frame ? frame->get_margins() : decoration_margins_t{0, 0, 0, 0};
    const int configure_width  = std::max(1, _pending.geometry.width - margins.left - margins.right);
    const int configure_height = std::max(1, _pending.geometry.height - margins.top - margins.bottom);
    this->target_configure = wlr_xdg_toplevel_set_size(this->toplevel, configure_width, configure_height);
}

void adjust_geometry_for_gravity(wf::toplevel_state_t& desired_state, wf::dimensions_t actual_size)
{
    if (desired_state.gravity & WLR_EDGE_RIGHT)
    {
        desired_state.geometry.x += desired_state.geometry.width - actual_size.width;
    }

    if (desired_state.gravity & WLR_EDGE_BOTTOM)
    {
        desired_state.geometry.y += desired_state.geometry.height - actual_size.height;
    }

    desired_state.geometry.width  = actual_size.width;
    desired_state.geometry.height = actual_size.height;
}

void wf::xdg_toplevel_t::apply()
{
    xdg_toplevel_applied_state_signal event_applied;
    event_applied.old_state = current();

    if (!toplevel)
    {
        // If toplevel does no longer exist, we can't change the size anymore.
        _committed.geometry.width  = _current.geometry.width;
        _committed.geometry.height = _current.geometry.height;
    }

    this->_current = committed();
    apply_pending_state();

    emit(&event_applied);
}

void wf::xdg_toplevel_t::handle_surface_commit()
{
    pending_state.merge_state(toplevel->base->surface);

    const bool is_committed = wf::get_core().tx_manager->is_object_committed(shared_from_this());
    if (is_committed)
    {
        // TODO: handle overflow?
        if (this->toplevel->base->current.configure_serial < this->target_configure)
        {
            // Desired state not reached => Ignore the state altogether
            return;
        }

        wlr_box wm_box;
        wlr_xdg_surface_get_geometry(toplevel->base, &wm_box);
        adjust_geometry_for_gravity(_committed, wf::dimensions(wm_box));

        emit_ready();
        return;
    }

    const bool is_pending = wf::get_core().tx_manager->is_object_pending(shared_from_this());
    if (is_pending)
    {
        return;
    }

    if (pending_state.size == wf::dimensions(main_surface->get_bounding_box()))
    {
        // Size did not change, there are no transactions going on - apply the new texture directly
        apply_pending_state();
        return;
    }

    // Size did change => Start a new transaction to change the size.
    wlr_box wm_box;
    wlr_xdg_surface_get_geometry(toplevel->base, &wm_box);
    auto margins = get_margins();

    this->pending().geometry.width  = wm_box.width + margins.left + margins.right;
    this->pending().geometry.height = wm_box.height + margins.top + margins.bottom;

    LOGC(VIEWS, "Client-initiated resize to geometry ", pending().geometry);
    auto tx = wf::txn::transaction_t::create();
    tx->add_object(shared_from_this());
    wf::get_core().tx_manager->schedule_transaction(std::move(tx));
}

void wf::xdg_toplevel_t::set_decoration(decorator_frame_t_t *frame)
{
    this->frame = frame;
}

wf::geometry_t wf::xdg_toplevel_t::calculate_base_geometry()
{
    auto geometry = current().geometry;
    auto margins  = get_margins();
    geometry.x     = geometry.x - wm_offset.x + margins.left;
    geometry.y     = geometry.y - wm_offset.y + margins.top;
    geometry.width = main_surface->get_bounding_box().width;
    geometry.height = main_surface->get_bounding_box().height;
    return geometry;
}

void wf::xdg_toplevel_t::apply_pending_state()
{
    if (toplevel)
    {
        pending_state.merge_state(toplevel->base->surface);
    }

    main_surface->apply_state(std::move(pending_state));

    if (toplevel)
    {
        wlr_box wm_box;
        wlr_xdg_surface_get_geometry(toplevel->base, &wm_box);
        this->wm_offset = wf::origin(wm_box);
    }
}

wf::decoration_margins_t wf::xdg_toplevel_t::get_margins()
{
    return frame ? frame->get_margins() : wf::decoration_margins_t{0, 0, 0, 0};
}

void wf::xdg_toplevel_t::emit_ready()
{
    if (pending_ready)
    {
        pending_ready = false;
        emit_object_ready(this);
    }
}
