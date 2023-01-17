#include <wayfire/plugin.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/view.hpp>
#include <wayfire/output.hpp>
#include <set>

#include "ipc-helpers.hpp"
#include "ipc.hpp"
#include "ipc-method-repository.hpp"
#include "wayfire/core.hpp"
#include "wayfire/object.hpp"
#include "wayfire/plugins/common/shared-core-data.hpp"
#include "wayfire/signal-definitions.hpp"
#include "wayfire/signal-provider.hpp"

class wayfire_ipc_notifications : public wf::plugin_interface_t
{
  public:
    void init() override
    {
        method_repository->register_method("wf/notifications/watch", on_client_watch);
        ipc_server->connect(&on_client_disconnected);
        wf::get_core().connect(&on_kbfocus_changed);
    }

    void fini() override
    {
        method_repository->unregister_method("wf/notifications/watch");
    }

    wf::ipc::method_callback on_client_watch = [=] (nlohmann::json data)
    {
        clients.insert(ipc_server->get_current_request_client());
        return wf::ipc::json_ok();
    };

  private:
    wf::shared_data::ref_ptr_t<wf::ipc::method_repository_t> method_repository;
    wf::shared_data::ref_ptr_t<wf::ipc::server_t> ipc_server;
    std::set<wf::ipc::client_t*> clients;

    wf::signal::connection_t<wf::ipc::client_disconnected_signal> on_client_disconnected =
        [=] (wf::ipc::client_disconnected_signal *ev)
    {
        clients.erase(ev->client);
    };

    wf::signal::connection_t<wf::keyboard_focus_changed_signal> on_kbfocus_changed =
        [=] (wf::keyboard_focus_changed_signal *ev)
    {
        if (auto view = wf::node_to_view(ev->new_focus))
        {
            nlohmann::json event;
            event["event"]   = "view-focused";
            event["view-id"] = view->get_id();
            for (auto& client : clients)
            {
                client->send_json(event);
            }
        }
    };
};

DECLARE_WAYFIRE_PLUGIN(wayfire_ipc_notifications);
