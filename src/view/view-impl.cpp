#include "view/surface-impl.hpp"
#include "wayfire/core.hpp"
#include "../core/core-impl.hpp"
#include "view-impl.hpp"
#include "wayfire/decorator.hpp"
#include "wayfire/scene-input.hpp"
#include "wayfire/scene-render.hpp"
#include "wayfire/scene.hpp"
#include "wayfire/signal-definitions.hpp"
#include "wayfire/view.hpp"
#include "wayfire/workspace-set.hpp"
#include "wayfire/output-layout.hpp"
#include <memory>
#include <wayfire/util/log.hpp>
#include <wayfire/view-helpers.hpp>
#include <wayfire/scene-operations.hpp>

#include "xdg-shell.hpp"

void wf::emit_view_map_signal(wayfire_view view, bool has_position)
{
    wf::view_mapped_signal data;
    data.view = view;
    data.is_positioned = has_position;

    view->emit(&data);
    view->get_output()->emit(&data);
    wf::get_core().emit(&data);
}

void wf::emit_ping_timeout_signal(wayfire_view view)
{
    wf::view_ping_timeout_signal data;
    data.view = view;
    view->emit(&data);
}

void wf::emit_geometry_changed_signal(wayfire_toplevel_view view, wf::geometry_t old_geometry)
{
    wf::view_geometry_changed_signal data;
    data.view = view;
    data.old_geometry = old_geometry;

    view->emit(&data);
    wf::get_core().emit(&data);
    if (view->get_output())
    {
        view->get_output()->emit(&data);
    }
}

void wf::view_interface_t::emit_view_map()
{
    emit_view_map_signal(self(), false);
}

void wf::view_interface_t::emit_view_unmap()
{
    view_unmapped_signal data;
    data.view = self();

    if (get_output())
    {
        get_output()->emit(&data);

        view_disappeared_signal disappeared_data;
        disappeared_data.view = self();
        get_output()->emit(&disappeared_data);
    }

    this->emit(&data);
    wf::get_core().emit(&data);
}

void wf::view_interface_t::emit_view_pre_unmap()
{
    view_pre_unmap_signal data;
    data.view = self();

    if (get_output())
    {
        get_output()->emit(&data);
    }

    emit(&data);
    wf::get_core().emit(&data);
}

void wf::init_desktop_apis()
{
    init_xdg_shell();
    init_layer_shell();

    wf::option_wrapper_t<bool> xwayland_enabled("core/xwayland");
    if (xwayland_enabled == 1)
    {
        init_xwayland();
    }
}

wayfire_view wf::wl_surface_to_wayfire_view(wl_resource *resource)
{
    if (!resource)
    {
        return nullptr;
    }

    auto surface = (wlr_surface*)wl_resource_get_user_data(resource);
    if (!surface)
    {
        return nullptr;
    }

    void *handle = NULL;
    if (wlr_surface_is_xdg_surface(surface))
    {
        handle = wlr_xdg_surface_from_wlr_surface(surface)->data;
    }

    if (wlr_surface_is_layer_surface(surface))
    {
        handle = wlr_layer_surface_v1_from_wlr_surface(surface)->data;
    }

#if WF_HAS_XWAYLAND
    if (wlr_surface_is_xwayland_surface(surface))
    {
        handle = wlr_xwayland_surface_from_wlr_surface(surface)->data;
    }

#endif

    wf::view_interface_t *view = static_cast<wf::view_interface_t*>(handle);
    return view ? view->self() : nullptr;
}

void wf::view_interface_t::view_priv_impl::set_mapped_surface_contents(
    std::shared_ptr<scene::wlr_surface_node_t> content)
{
    wsurface = content->get_surface();
    surface_root_node->set_children_list({content});
    scene::update(surface_root_node, scene::update_flag::CHILDREN_LIST);

    if (content->get_surface())
    {
        surface_controller =
            std::make_unique<wlr_surface_controller_t>(content->get_surface(), surface_root_node);
    }
}

void wf::view_interface_t::view_priv_impl::unset_mapped_surface_contents()
{
    wsurface = nullptr;
    surface_root_node->set_children_list({});
    scene::update(surface_root_node, scene::update_flag::CHILDREN_LIST);
    surface_controller.reset();
}

void wf::view_interface_t::view_priv_impl::set_mapped(bool mapped)
{
    if (mapped)
    {
        scene::set_node_enabled(root_node, true);
    } else
    {
        scene::set_node_enabled(root_node, false);
    }
}

// ---------------------------------------------- view helpers -----------------------------------------------
std::optional<wf::scene::layer> wf::get_view_layer(wayfire_view view)
{
    wf::scene::node_t *node = view->get_root_node().get();
    auto root = wf::get_core().scene().get();

    while (node->parent())
    {
        if (node->parent() == root)
        {
            for (int i = 0; i < (int)wf::scene::layer::ALL_LAYERS; i++)
            {
                if (node == root->layers[i].get())
                {
                    return (wf::scene::layer)i;
                }
            }
        }

        node = node->parent();
    }

    return {};
}

void wf::view_bring_to_front(wayfire_view view)
{
    wf::scene::node_t *node = view->get_root_node().get();
    wf::scene::node_t *damage_from = nullptr;
    while (node->parent())
    {
        if (!node->is_structure_node() && dynamic_cast<scene::floating_inner_node_t*>(node->parent()))
        {
            damage_from = node->parent();
            wf::scene::raise_to_front(node->shared_from_this());
        }

        node = node->parent();
    }

    if (damage_from)
    {
        wf::scene::damage_node(damage_from->shared_from_this(), damage_from->get_bounding_box());
    }
}

static void gather_views(wf::scene::node_ptr root, std::vector<wayfire_view>& views)
{
    if (!root->is_enabled())
    {
        return;
    }

    if (auto view = wf::node_to_view(root))
    {
        views.push_back(view);
        return;
    }

    for (auto& ch : root->get_children())
    {
        gather_views(ch, views);
    }
}

std::vector<wayfire_view> wf::collect_views_from_scenegraph(wf::scene::node_ptr root)
{
    std::vector<wayfire_view> views;
    gather_views(root, views);
    return views;
}

std::vector<wayfire_view> wf::collect_views_from_output(wf::output_t *output,
    std::initializer_list<wf::scene::layer> layers)
{
    std::vector<wayfire_view> views;
    for (auto layer : layers)
    {
        gather_views(output->node_for_layer(layer), views);
    }

    return views;
}

void wf::adjust_geometry_for_gravity(wf::toplevel_state_t& desired_state, wf::dimensions_t actual_size)
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
