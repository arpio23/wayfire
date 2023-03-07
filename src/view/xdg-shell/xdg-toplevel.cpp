#include "xdg-toplevel.hpp"
#include "wayfire/txn/transaction-object.hpp"

static void emit_object_ready(wf::txn::transaction_object_t *obj)
{
    wf::txn::object_ready_signal data_ready;
    data_ready.self = obj;
    obj->emit(&data_ready);
    return;
}

wf::xdg_toplevel_t::xdg_toplevel_t(wlr_xdg_toplevel* toplevel)
{
    this->toplevel = toplevel;

    on_surface_commit.set_callback([&] (void*) { handle_surface_commit(); });
    on_surface_commit.connect(&toplevel->base->surface->events.commit);

    on_toplevel_destroy.set_callback([&] (void*)
    {
        this->toplevel = NULL;
        on_toplevel_destroy.disconnect();
        on_surface_commit.disconnect();
    });
    on_toplevel_destroy.connect(&toplevel->base->events.destroy);
}

void wf::xdg_toplevel_t::commit()
{
    _committed = _pending;
    if (wf::dimensions(_pending.geometry) == wf::dimensions(_current.geometry))
    {
        emit_object_ready(this);
        return;
    }

    if (!this->toplevel)
    {
        // No longer mapped => we can do whatever
        emit_object_ready(this);
        return;
    }

    auto margins = frame ? frame->get_margins() : decoration_margins_t{0, 0, 0, 0};
    const int configure_width = std::max(1, _pending.geometry.width - margins.left - margins.right);
    const int configure_height = std::max(1, _pending.geometry.height - margins.top - margins.bottom);
    this->target_configure = wlr_xdg_toplevel_set_size(this->toplevel, configure_width, configure_height);
}

void wf::xdg_toplevel_t::apply()
{
    if (toplevel)
    {
        wlr_box wm_box;
        wlr_xdg_surface_get_geometry(toplevel->base, &wm_box);
        _committed.geometry.width = wm_box.width;
        _committed.geometry.height = wm_box.height;
    } else
    {
        // If toplevel does no longer exist, we can't change the size anymore.
        _committed.geometry.width = _current.geometry.width;
        _committed.geometry.height = _current.geometry.height;
    }

    this->_current = this->_committed;
}

void wf::xdg_toplevel_t::handle_surface_commit()
{
    // TODO: handle overflow?
    if (this->toplevel->base->current.configure_serial < this->target_configure)
    {
        // Desired state not reached => Ignore the state altogether
        return;
    }

    emit_object_ready(this);
}

void wf::xdg_toplevel_t::set_decoration(decorator_frame_t_t* frame)
{
    this->frame = frame;
}
