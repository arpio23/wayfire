#pragma once

#include "wayfire/core.hpp"
#include "wayfire/output.hpp"
#include "wayfire/scene-input.hpp"
#include "wayfire/scene-operations.hpp"
#include "wayfire/scene.hpp"
#include <wayfire/debug.hpp>
#include <memory>
#include <string>
namespace wf
{
namespace scene
{
/**
 * A scene node which can be used to implement input grab on a particular output.
 */
class grab_node_t : public node_t
{
    std::string name;
    wf::output_t *output;
    keyboard_interaction_t *keyboard = nullptr;
    pointer_interaction_t *pointer   = nullptr;
    touch_interaction_t *touch = nullptr;

  public:
    grab_node_t(std::string name, wf::output_t *output,
        keyboard_interaction_t *keyboard = NULL,
        pointer_interaction_t *pointer   = NULL,
        touch_interaction_t *touch = NULL) :
        node_t(false), name(name), output(output),
        keyboard(keyboard), pointer(pointer), touch(touch)
    {}

    std::optional<input_node_t> find_node_at(const wf::pointf_t& at) override
    {
        if (output->get_layout_geometry() & at)
        {
            input_node_t result;
            result.node = this;
            result.local_coords = to_local(at);
            return result;
        }

        return {};
    }

    wf::keyboard_focus_node_t keyboard_refocus(wf::output_t *output) override
    {
        if (output != this->output)
        {
            return wf::keyboard_focus_node_t{};
        }

        return wf::keyboard_focus_node_t{
            .node = this,
            .importance = focus_importance::REGULAR,
            .allow_focus_below = false,
        };
    }

    /**
     * Get a textual representation of the node, used for debugging purposes.
     * For example, see wf::dump_scene().
     * The representation should therefore not contain any newline characters.
     */
    std::string stringify() const override
    {
        return name + "-input-grab";
    }

    keyboard_interaction_t& keyboard_interaction() override
    {
        return keyboard ? *keyboard : node_t::keyboard_interaction();
    }

    pointer_interaction_t& pointer_interaction() override
    {
        return pointer ? *pointer : node_t::pointer_interaction();
    }

    touch_interaction_t& touch_interaction() override
    {
        return touch ? *touch : node_t::touch_interaction();
    }
};
}

/**
 * A helper class for managing input grabs on an output.
 */
class input_grab_t
{
    wf::output_t *output;
    std::shared_ptr<scene::grab_node_t> grab_node;

  public:
    input_grab_t(std::string name, wf::output_t *output,
        keyboard_interaction_t *keyboard = NULL,
        pointer_interaction_t *pointer   = NULL,
        touch_interaction_t *touch = NULL)
    {
        this->output = output;
        grab_node    = std::make_shared<scene::grab_node_t>(name, output, keyboard, pointer, touch);
    }

    bool is_grabbed() const
    {
        return grab_node->parent() != nullptr;
    }

    /**
     * Grab input from all layers from background to @layer_below.
     */
    void grab_input(wf::scene::layer layer_below, bool retain_pressed_state = false)
    {
        wf::dassert(grab_node->parent() == nullptr, "Trying to grab twice!");

        auto& root    = wf::get_core().scene();
        auto children = root->get_children();

        auto idx = std::find(children.begin(), children.end(),
            root->layers[(int)layer_below]);

        wf::dassert(idx != children.end(), "Could not find node for a layer: " +
            std::to_string((int)layer_below));
        children.insert(idx, grab_node);
        root->set_children_list(children);
        wf::get_core().transfer_grab(grab_node, retain_pressed_state);
        scene::update(root, scene::update_flag::CHILDREN_LIST);
        output->refocus();
    }

    /**
     * Ungrab the input.
     */
    void ungrab_input()
    {
        if (grab_node->parent())
        {
            wf::scene::remove_child(grab_node);
        }

        output->refocus();
    }
};
}
