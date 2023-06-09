#include <wayfire/core.hpp>
#include <algorithm>
#include "bindings-repository-impl.hpp"

wf::bindings_repository_t::bindings_repository_t()
{
    priv = std::make_unique<impl>();
    wf::get_core().connect(&priv->on_config_reload);
}

template<class Option, class Callback>
static void push_binding(wf::binding_container_t<Option, Callback>& bindings,
    wf::option_sptr_t<Option> opt, Callback *callback)
{
    auto bnd = std::make_unique<wf::binding_t<Option, Callback>>();
    bnd->activated_by = opt;
    bnd->callback     = callback;
    bindings.emplace_back(std::move(bnd));
}

wf::bindings_repository_t::~bindings_repository_t()
{}

void wf::bindings_repository_t::add_key(option_sptr_t<keybinding_t> key, wf::key_callback *cb)
{
    push_binding(priv->keys, key, cb);
}

void wf::bindings_repository_t::add_axis(option_sptr_t<keybinding_t> axis, wf::axis_callback *cb)
{
    push_binding(priv->axes, axis, cb);
}

void wf::bindings_repository_t::add_button(option_sptr_t<buttonbinding_t> button, wf::button_callback *cb)
{
    push_binding(priv->buttons, button, cb);
}

void wf::bindings_repository_t::add_activator(
    option_sptr_t<activatorbinding_t> activator, wf::activator_callback *cb)
{
    push_binding(priv->activators, activator, cb);
    if (activator->get_value().get_hotspots().size())
    {
        priv->recreate_hotspots();
    }
}

bool wf::bindings_repository_t::handle_key(const wf::keybinding_t& pressed,
    uint32_t mod_binding_key)
{
    std::vector<std::function<bool()>> callbacks;
    for (auto& binding : this->priv->keys)
    {
        if (binding->activated_by->get_value() == pressed)
        {
            /* We must be careful because the callback might be erased,
             * so force copy the callback into the lambda */
            auto callback = binding->callback;
            callbacks.emplace_back([pressed, callback] ()
            {
                return (*callback)(pressed);
            });
        }
    }

    for (auto& binding : this->priv->activators)
    {
        if (binding->activated_by->get_value().has_match(pressed))
        {
            /* We must be careful because the callback might be erased,
             * so force copy the callback into the lambda */
            auto callback = binding->callback;
            callbacks.emplace_back([pressed, callback, mod_binding_key] ()
            {
                wf::activator_data_t ev = {
                    .source = activator_source_t::KEYBINDING,
                    .activation_data = pressed.get_key()
                };

                if (mod_binding_key)
                {
                    ev.source = activator_source_t::MODIFIERBINDING;
                    ev.activation_data = mod_binding_key;
                }

                return (*callback)(ev);
            });
        }
    }

    bool handled = false;
    for (auto& cb : callbacks)
    {
        handled |= cb();
    }

    return handled;
}

bool wf::bindings_repository_t::handle_axis(uint32_t modifiers,
    wlr_pointer_axis_event *ev)
{
    std::vector<wf::axis_callback*> callbacks;

    for (auto& binding : this->priv->axes)
    {
        if (binding->activated_by->get_value() == wf::keybinding_t{modifiers, 0})
        {
            callbacks.push_back(binding->callback);
        }
    }

    for (auto call : callbacks)
    {
        (*call)(ev);
    }

    return !callbacks.empty();
}

bool wf::bindings_repository_t::handle_button(const wf::buttonbinding_t& pressed)
{
    std::vector<std::function<bool()>> callbacks;
    for (auto& binding : this->priv->buttons)
    {
        if (binding->activated_by->get_value() == pressed)
        {
            /* We must be careful because the callback might be erased,
             * so force copy the callback into the lambda */
            auto callback = binding->callback;
            callbacks.emplace_back([=] ()
            {
                return (*callback)(pressed);
            });
        }
    }

    for (auto& binding : this->priv->activators)
    {
        if (binding->activated_by->get_value().has_match(pressed))
        {
            /* We must be careful because the callback might be erased,
             * so force copy the callback into the lambda */
            auto callback = binding->callback;
            callbacks.emplace_back([=] ()
            {
                wf::activator_data_t data = {
                    .source = activator_source_t::BUTTONBINDING,
                    .activation_data = pressed.get_button(),
                };
                return (*callback)(data);
            });
        }
    }

    bool binding_handled = false;
    for (auto call : callbacks)
    {
        binding_handled |= call();
    }

    return binding_handled;
}

void wf::bindings_repository_t::handle_gesture(const wf::touchgesture_t& gesture)
{
    std::vector<std::function<void()>> callbacks;
    for (auto& binding : this->priv->activators)
    {
        if (binding->activated_by->get_value().has_match(gesture))
        {
            /* We must be careful because the callback might be erased,
             * so force copy the callback into the lambda */
            auto callback = binding->callback;
            callbacks.emplace_back([=] ()
            {
                wf::activator_data_t data = {
                    .source = activator_source_t::GESTURE,
                    .activation_data = 0
                };
                (*callback)(data);
            });
        }
    }

    for (auto& cb : callbacks)
    {
        cb();
    }
}

void wf::bindings_repository_t::rem_binding(void *callback)
{
    const auto& erase = [callback] (auto& container)
    {
        auto it = std::remove_if(container.begin(), container.end(),
            [callback] (const auto& ptr)
        {
            return ptr->callback == callback;
        });
        container.erase(it, container.end());
    };

    bool update_hotspots = false;
    for (auto& act : this->priv->activators)
    {
        update_hotspots |= !act->activated_by->get_value().get_hotspots().empty();
    }

    erase(priv->keys);
    erase(priv->buttons);
    erase(priv->axes);
    erase(priv->activators);

    if (update_hotspots)
    {
        priv->recreate_hotspots();
    }
}
