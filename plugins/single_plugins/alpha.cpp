/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Scott Moreau
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <memory>
#include <wayfire/core.hpp>
#include <wayfire/bindings-repository.hpp>
#include <wayfire/view.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/output.hpp>
#include "ipc-helpers.hpp"
#include "wayfire/plugin.hpp"
#include "wayfire/plugins/common/shared-core-data.hpp"
#include "wayfire/view-transform.hpp"
#include "wayfire/workspace-manager.hpp"
#include "ipc-method-repository.hpp"

class wayfire_alpha : public wf::plugin_interface_t
{
    wf::option_wrapper_t<wf::keybinding_t> modifier{"alpha/modifier"};
    wf::option_wrapper_t<double> min_value{"alpha/min_value"};
    wf::shared_data::ref_ptr_t<wf::ipc::method_repository_t> ipc_repo;

  public:
    void init() override
    {
        min_value.set_callback(min_value_changed);
        wf::get_core().bindings->add_axis(modifier, &axis_cb);
        ipc_repo->register_method("wf/alpha/set_view_alpha", ipc_set_view_alpha);
    }

    void fini() override
    {
        for (auto& view : wf::get_core().get_all_views())
        {
            view->get_transformed_node()->rem_transformer("alpha");
        }

        wf::get_core().bindings->rem_binding(&axis_cb);
        ipc_repo->unregister_method("wf/alpha/set_view_alpha");
    }

    wf::ipc::method_callback ipc_set_view_alpha = [=] (nlohmann::json data) -> nlohmann::json
    {
        WFJSON_EXPECT_FIELD(data, "view-id", number_unsigned);
        WFJSON_EXPECT_FIELD(data, "alpha", number);

        auto view = wf::ipc::find_view_by_id(data["view-id"]);
        if (view && view->is_mapped())
        {
            auto tr = ensure_transformer(view);
            tr->alpha = data["alpha"];
            view->damage();
        } else
        {
            return wf::ipc::json_error("Failed to find view with given id. Maybe it was closed?");
        }

        return wf::ipc::json_ok();
    };

    std::shared_ptr<wf::scene::view_2d_transformer_t> ensure_transformer(wayfire_view view)
    {
        auto tmgr = view->get_transformed_node();
        if (!tmgr->get_transformer<wf::scene::node_t>("alpha"))
        {
            auto node = std::make_shared<wf::scene::view_2d_transformer_t>(view);
            tmgr->add_transformer(node, wf::TRANSFORMER_2D, "alpha");
        }

        return tmgr->get_transformer<wf::scene::view_2d_transformer_t>("alpha");
    }

    void update_alpha(wayfire_view view, float delta)
    {
        auto tmgr = view->get_transformed_node();
        if (!tmgr->get_transformer<wf::scene::node_t>("alpha"))
        {
            auto node = std::make_shared<wf::scene::view_2d_transformer_t>(view);
            tmgr->add_transformer(node, wf::TRANSFORMER_2D, "alpha");
        }

        auto transformer =
            tmgr->get_transformer<wf::scene::view_2d_transformer_t>("alpha");

        auto old_value = transformer->alpha;
        transformer->alpha = std::clamp(
            transformer->alpha - delta * 0.003, (double)min_value, 1.0);

        if (transformer->alpha == 1.0)
        {
            return view->get_transformed_node()->rem_transformer("alpha");
        } else if (old_value != transformer->alpha)
        {
            view->damage();
        }
    }

    wf::axis_callback axis_cb = [=] (wlr_pointer_axis_event *ev)
    {
        auto output = wf::get_core().get_active_output();
        if (!output)
        {
            return false;
        }

        if (!output->can_activate_plugin(wf::CAPABILITY_MANAGE_DESKTOP))
        {
            return false;
        }

        auto view = wf::get_core().get_cursor_focus_view();
        if (!view)
        {
            return false;
        }

        auto layer = output->workspace->get_view_layer(view);

        if (layer == wf::LAYER_BACKGROUND)
        {
            return false;
        }

        if (ev->orientation == WLR_AXIS_ORIENTATION_VERTICAL)
        {
            update_alpha(view, ev->delta);

            return true;
        }

        return false;
    };

    wf::config::option_base_t::updated_callback_t min_value_changed = [=] ()
    {
        for (auto& view : wf::get_core().get_all_views())
        {
            auto tmgr = view->get_transformed_node();
            auto transformer = tmgr->get_transformer<wf::scene::view_2d_transformer_t>("alpha");
            if (transformer && (transformer->alpha < min_value))
            {
                transformer->alpha = min_value;
                view->damage();
            }
        }
    };
};

DECLARE_WAYFIRE_PLUGIN(wayfire_alpha);
