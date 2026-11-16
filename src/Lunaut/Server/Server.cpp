#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Common.h"

LUAU_FASTFLAG(LuauSolverV2)

namespace lunaut
{
namespace
{
namespace priv
{

using request_handler = json (server::*)(const json&);
using notification_handler = void (server::*)(const json&);

const std::unordered_map<std::string_view, request_handler> relevant_requests = {
	{"initialize", &server::on_initialize},
	{"shutdown", &server::on_shutdown},
	{"textDocument/hover", &server::on_hover},
	{"textDocument/completion", &server::on_completion},
	{"textDocument/definition", &server::on_definition},
	{"textDocument/documentSymbol", &server::on_document_symbol},
	{"textDocument/signatureHelp", &server::on_signature_help},
	{"textDocument/references", &server::on_references},
	{"textDocument/prepareRename", &server::on_prepare_rename},
	{"textDocument/rename", &server::on_rename},
	{"textDocument/inlayHint", &server::on_inlay_hint},
	{"textDocument/semanticTokens/full", &server::on_semantic_tokens},
	{"textDocument/codeAction", &server::on_code_action},
	{"textDocument/documentColor", &server::on_document_color},
	{"textDocument/colorPresentation", &server::on_color_presentation},
	{"textDocument/foldingRange", &server::on_folding_range},
	{"textDocument/documentHighlight", &server::on_document_highlight},
	{"textDocument/documentLink", &server::on_document_link},
	{"workspace/symbol", &server::on_workspace_symbol},
	{"textDocument/prepareCallHierarchy", &server::on_prepare_call_hierarchy},
	{"callHierarchy/incomingCalls", &server::on_incoming_calls},
	{"callHierarchy/outgoingCalls", &server::on_outgoing_calls},
	{"textDocument/onTypeFormatting", &server::on_on_type_formatting},
	{"textDocument/formatting", &server::on_formatting},
	{"textDocument/rangeFormatting", &server::on_range_formatting},
	{"lunaut/status", &server::on_lunaut_status},
	{"lunaut/typeStats", &server::on_lunaut_type_stats},
	{"lunaut/fflags", &server::on_lunaut_fflags},
	{"lunaut/definitions", &server::on_lunaut_definitions},
	{"lunaut/sourcemap", &server::on_lunaut_sourcemap},
};

const std::unordered_map<std::string_view, notification_handler> relevant_notifications = {
	{"initialized", &server::on_initialized},
	{"exit", &server::on_exit},
	{"textDocument/didOpen", &server::on_did_open},
	{"textDocument/didChange", &server::on_did_change},
	{"textDocument/didClose", &server::on_did_close},
	{"textDocument/didSave", &server::on_did_save},
	{"workspace/didChangeConfiguration", &server::on_did_change_configuration},
	{"workspace/didChangeWatchedFiles", &server::on_did_change_watched_files},
	{"workspace/didChangeWorkspaceFolders", &server::on_did_change_workspace_folders},
	{"$/plugin/full", &server::on_plugin_full},
	{"$/plugin/clear", &server::on_plugin_clear},
	{"$/setTrace", &server::on_ignore},
	{"$/cancelRequest", &server::on_ignore},
};

const std::vector<std::pair<std::string_view, std::string_view>> relevant_refreshes = {
	{"/workspace/inlayHint/refreshSupport", "workspace/inlayHint/refresh"},
	{"/workspace/semanticTokens/refreshSupport", "workspace/semanticTokens/refresh"},
};

const std::vector<std::string_view> watched_globs = {"**/*.luau", "**/*.lua", "**/.luaurc", "**/sourcemap.json", "**/*.project.json"};

json get_client_settings(const json& value);

}
}

server::server(jsonrpc& rpc, optionsinfo options)
	: rpc(rpc)
	, options(std::move(options))
{
}

int server::run()
{
	std::thread reader(&server::read_messages, this);
	reader.detach();

	while (state != enumitem_server_state::Exited)
	{
		std::optional<json> message = take_message();

		if (!message)
		{
			break;
		}

		process_message(*message);

		bool idle = false;

		{
			std::lock_guard<std::mutex> guard(queue_lock);
			idle = queue.empty();
		}

		if (!idle)
		{
			continue;
		}

		for (const std::unique_ptr<workspace>& folder : workspaces)
		{
			try
			{
				folder->flush_diagnostics();
			}
			catch (const std::exception& error)
			{
				send_log(enumitem_message_type::Error, "publishing diagnostics failed: " + std::string(error.what()));
			}
			catch (...)
			{
				send_log(enumitem_message_type::Error, "publishing diagnostics failed");
			}
		}

		if (null_workspace)
		{
			try
			{
				null_workspace->flush_diagnostics();
			}
			catch (...)
			{
				send_log(enumitem_message_type::Error, "publishing diagnostics failed");
			}
		}
	}

	return exit_code;
}

void server::read_messages()
{
	while (true)
	{
		result<json> message = rpc.read_message();
		std::lock_guard<std::mutex> guard(queue_lock);

		if (!message)
		{
			if (message.message == "input stream closed")
			{
				input_closed = true;
				queue_ready.notify_all();
				return;
			}

			stdx::oute(message.message);
			continue;
		}

		if (message.value.value("method", "") == "$/cancelRequest")
		{
			cancelled.insert(message.value.value("/params/id"_json_pointer, json()).dump());
			continue;
		}

		queue.push_back(std::move(message.value));
		queue_ready.notify_all();
	}
}

std::optional<json> server::take_message()
{
	std::unique_lock<std::mutex> guard(queue_lock);
	queue_ready.wait(guard, [this]() { return !queue.empty() || input_closed; });

	if (queue.empty())
	{
		return std::nullopt;
	}

	json message = std::move(queue.front());
	queue.pop_front();
	return message;
}

void server::send_log(enumitem_message_type type, const std::string& message)
{
	rpc.send_notification("window/logMessage", {{"type", static_cast<int>(type)}, {"message", message}});

#if LUNAUT_DEBUG
	stdx::outi(std::string(enum_message_type::get_name(type)) + ": " + message);
#endif
}

void server::send_window_message(enumitem_message_type type, const std::string& message)
{
	rpc.send_notification("window/showMessage", {{"type", static_cast<int>(type)}, {"message", message}});
}

void server::send_diagnostics(const std::string& address, std::optional<int> version, const json& diagnostics)
{
	json params = {{"uri", address}, {"diagnostics", diagnostics}};

	if (version)
	{
		params["version"] = *version;
	}

	rpc.send_notification("textDocument/publishDiagnostics", params);
}

void server::send_edit(const json& edit, bool move_cursor)
{
	if (extension_client)
	{
		rpc.send_notification("lunaut/applyEdit", {{"edit", edit}, {"moveCursor", move_cursor}});
		return;
	}

	rpc.send_request("workspace/applyEdit", {{"label", "insert end"}, {"edit", edit}});
}

json server::on_initialize(const json& params)
{
	client_settings = priv::get_client_settings(params.value("initializationOptions", json::object()));
	client_capabilities = params.value("capabilities", json::object());
	extension_client = params.value("initializationOptions", json::object()).contains("vscode");

	std::vector<std::string> roots;

	if (params.contains("workspaceFolders") && params["workspaceFolders"].is_array())
	{
		for (const json& folder : params["workspaceFolders"])
		{
			roots.push_back(folder.value("uri", ""));
		}
	}
	else if (params.contains("rootUri") && params["rootUri"].is_string())
	{
		roots.push_back(params["rootUri"].get<std::string>());
	}

	std::filesystem::path first = roots.empty() ? std::filesystem::path() : std::filesystem::path(uri::to_path(roots.front()));
	fflags = fflagregistry::apply(get_settings_for(first).fflags, options.fflags, options.no_flags_enabled, *this);

	for (const std::string& root : roots)
	{
		add_workspace(root);
	}

	null_workspace = std::make_unique<workspace>(*this, std::filesystem::path(), get_settings_for(std::filesystem::path()));
	state = enumitem_server_state::Running;

	return {
		{"capabilities", protocol::get_capabilities()},
		{"serverInfo", {{"name", LUNAUT_NAME}, {"version", LUNAUT_VERSION}}},
	};
}

json server::on_shutdown(const json& params)
{
	state = enumitem_server_state::ShuttingDown;
	return nullptr;
}

json server::on_hover(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_hover(params);
}

json server::on_completion(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_completion(params);
}

json server::on_definition(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_definition(params);
}

json server::on_document_symbol(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_document_symbols(params);
}

json server::on_signature_help(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_signature_help(params);
}

json server::on_references(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_references(params);
}

json server::on_prepare_rename(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_prepare_rename(params);
}

json server::on_rename(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_rename(params);
}

json server::on_inlay_hint(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_inlay_hints(params);
}

json server::on_semantic_tokens(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_semantic_tokens(params);
}

json server::on_code_action(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_code_actions(params);
}

json server::on_document_color(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_document_colors(params);
}

json server::on_color_presentation(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_color_presentations(params);
}

json server::on_folding_range(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_folding_ranges(params);
}

json server::on_document_highlight(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_document_highlights(params);
}

json server::on_document_link(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_document_links(params);
}

json server::on_workspace_symbol(const json& params)
{
	json symbols = json::array();

	for (const std::unique_ptr<workspace>& folder : workspaces)
	{
		folder->initialize();

		for (json& symbol : folder->get_workspace_symbols(params))
		{
			symbols.push_back(std::move(symbol));
		}
	}

	return symbols;
}

json server::on_prepare_call_hierarchy(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_call_hierarchy_items(params);
}

json server::on_incoming_calls(const json& params)
{
	return get_workspace(params["item"].value("uri", "")).get_incoming_calls(params);
}

json server::on_outgoing_calls(const json& params)
{
	return get_workspace(params["item"].value("uri", "")).get_outgoing_calls(params);
}

json server::on_on_type_formatting(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_on_type_edits(params);
}

json server::on_formatting(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_formatting(params);
}

json server::on_range_formatting(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_formatting(params);
}

void server::on_plugin_full(const json& params)
{
	for (const std::unique_ptr<workspace>& folder : workspaces)
	{
		folder->apply_plugin_tree(params);
	}
}

void server::on_plugin_clear(const json& params)
{
	for (const std::unique_ptr<workspace>& folder : workspaces)
	{
		folder->apply_plugin_tree(nullptr);
	}
}

json server::on_lunaut_status(const json& params)
{
	json statuses = json::array();

	for (const std::unique_ptr<workspace>& folder : workspaces)
	{
		statuses.push_back(folder->get_status());
	}

	return {
		{"name", LUNAUT_NAME},
		{"version", LUNAUT_VERSION},
		{"luau", LUNAUT_LUAU_VERSION},
		{"platform", LUNAUT_PLATFORM},
		{"compiler", LUNAUT_COMPILER},
		{"solver", FFlag::LuauSolverV2 ? "new" : "old"},
		{"workspaces", statuses},
	};
}

json server::on_lunaut_type_stats(const json& params)
{
	json stats = json::array();

	for (const std::unique_ptr<workspace>& folder : workspaces)
	{
		try
		{
			stats.push_back({{"root", folder->get_root().generic_string()}, {"stats", folder->get_type_stats()}});
		}
		catch (const std::exception& error)
		{
			stats.push_back({{"root", folder->get_root().generic_string()}, {"error", error.what()}});
		}
		catch (...)
		{
			stats.push_back({{"root", folder->get_root().generic_string()}, {"error", "unknown error"}});
		}
	}

	return {{"workspaces", stats}};
}

json server::on_lunaut_fflags(const json& params)
{
	std::string filter = stdx::to_lower(params.value("filter", ""));
	json flags = json::array();

	for (const fflagstateinfo& flag : fflagregistry::get_flags())
	{
		if (filter.empty() || stdx::to_lower(flag.name).find(filter) != std::string::npos)
		{
			flags.push_back(flag.to_json());
		}
	}

	return flags;
}

json server::on_lunaut_definitions(const json& params)
{
	json listed = json::array();

	for (const std::unique_ptr<workspace>& folder : workspaces)
	{
		json definitions = json::array();

		for (const loadeddefinitioninfo& definition : folder->get_definitions())
		{
			definitions.push_back(definition.to_json());
		}

		listed.push_back({{"root", folder->get_root().generic_string()}, {"definitions", definitions}});
	}

	return listed;
}

json server::on_lunaut_sourcemap(const json& params)
{
	json rebuilt = json::array();

	for (const std::unique_ptr<workspace>& folder : workspaces)
	{
		if (!folder->is_roblox())
		{
			continue;
		}

		result<nothing> outcome = folder->rebuild_sourcemap(params.value("regenerate", true));
		rebuilt.push_back({{"root", folder->get_root().generic_string()}, {"success", outcome.state}, {"message", outcome.message}});
	}

	return rebuilt;
}

void server::on_initialized(const json& params)
{
	for (const std::unique_ptr<workspace>& folder : workspaces)
	{
		folder->initialize();
		folder->publish_workspace_diagnostics();
	}

	register_watchers();
	send_log(
		enumitem_message_type::Info, std::string(LUNAUT_DISPLAY_NAME) + " " + LUNAUT_VERSION + " (Luau " + LUNAUT_LUAU_VERSION + ") ready");
}

void server::on_exit(const json& params)
{
	exit_code = state == enumitem_server_state::ShuttingDown ? 0 : 1;
	state = enumitem_server_state::Exited;
}

void server::on_did_open(const json& params)
{
	const json& document = params["textDocument"];
	std::string address = document.value("uri", "");
	get_workspace(address).open_document(address, document.value("text", ""), document.value("version", 0));
}

void server::on_did_change(const json& params)
{
	const json& document = params["textDocument"];
	std::string address = document.value("uri", "");
	get_workspace(address).update_document(address, params["contentChanges"], document.value("version", 0));
}

void server::on_did_close(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	get_workspace(address).close_document(address);
}

void server::on_did_save(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	get_workspace(address).save_document(address);
}

void server::on_did_change_configuration(const json& params)
{
	client_settings = priv::get_client_settings(params.value("settings", json::object()));

	for (const std::unique_ptr<workspace>& folder : workspaces)
	{
		folder->set_settings(get_settings_for(folder->get_root()));
	}

	for (const auto& [capability, method] : priv::relevant_refreshes)
	{
		if (client_capabilities.value(json::json_pointer(std::string(capability)), false))
		{
			rpc.send_request(std::string(method), nullptr);
		}
	}
}

void server::on_did_change_watched_files(const json& params)
{
	std::map<workspace*, json> grouped;

	for (const json& change : params.value("changes", json::array()))
	{
		grouped[&get_workspace(change.value("uri", ""))].push_back(change);
	}

	for (auto& [folder, changes] : grouped)
	{
		folder->apply_watched_changes(changes);
	}
}

void server::on_did_change_workspace_folders(const json& params)
{
	const json& event = params["event"];

	for (const json& removed : event.value("removed", json::array()))
	{
		std::string path = uri::to_path(removed.value("uri", ""));
		workspaces.erase(
			std::remove_if(
				workspaces.begin(),
				workspaces.end(),
				[&path](const std::unique_ptr<workspace>& folder)
				{ return uri::normalize_path(folder->get_root().generic_string()) == path; }),
			workspaces.end());
	}

	for (const json& added : event.value("added", json::array()))
	{
		add_workspace(added.value("uri", ""));
		workspaces.back()->initialize();
	}
}

void server::on_ignore(const json& params) {}

void server::process_message(const json& message)
{
	if (!message.contains("method"))
	{
		return;
	}

	std::string method = message.value("method", "");
	json params = message.value("params", json::object());

	if (!params.is_object())
	{
		params = json::object();
	}

	if (!params.contains("textDocument") || !params["textDocument"].is_object())
	{
		params["textDocument"] = json::object();
	}

	if (!params["textDocument"].contains("uri") || !params["textDocument"]["uri"].is_string())
	{
		params["textDocument"]["uri"] = "";
	}

	if (!params.contains("position") || !params["position"].is_object())
	{
		params["position"] = {{"line", 0}, {"character", 0}};
	}

	if (!params.contains("item") || !params["item"].is_object())
	{
		params["item"] = json::object();
	}

	if (!message.contains("id"))
	{
		const auto handler = priv::relevant_notifications.find(method);

		if (handler == priv::relevant_notifications.end())
		{
			return;
		}

		if (state == enumitem_server_state::Uninitialized && method != "exit")
		{
			return;
		}

		try
		{
			(this->*handler->second)(params);
		}
		catch (const std::exception& error)
		{
			send_log(enumitem_message_type::Error, method + " failed: " + error.what());
		}
		catch (...)
		{
			send_log(enumitem_message_type::Error, method + " failed: unknown error");
		}

		return;
	}

	const json& id = message["id"];

	{
		std::lock_guard<std::mutex> guard(queue_lock);

		if (cancelled.erase(id.dump()) > 0)
		{
			rpc.send_error(id, enumitem_error_code::RequestCancelled, "request cancelled");
			return;
		}
	}

	if (state == enumitem_server_state::Uninitialized && method != "initialize")
	{
		rpc.send_error(id, enumitem_error_code::ServerNotInitialized, "server is not initialized");
		return;
	}

	if (state == enumitem_server_state::ShuttingDown)
	{
		rpc.send_error(id, enumitem_error_code::InvalidRequest, "server is shutting down");
		return;
	}

	const auto handler = priv::relevant_requests.find(method);

	if (handler == priv::relevant_requests.end())
	{
		rpc.send_error(id, enumitem_error_code::MethodNotFound, "unknown method: " + method);
		return;
	}

	try
	{
		rpc.send_response(id, (this->*handler->second)(params));
	}
	catch (const std::exception& error)
	{
		rpc.send_error(id, enumitem_error_code::InternalError, method + " failed: " + error.what());
	}
	catch (...)
	{
		rpc.send_error(id, enumitem_error_code::InternalError, method + " failed: unknown error");
	}
}

void server::add_workspace(const std::string& address)
{
	std::filesystem::path root(uri::to_path(address));
	workspaces.push_back(std::make_unique<workspace>(*this, root, get_settings_for(root)));
}

void server::register_watchers()
{
	bool dynamic = client_capabilities.value("/workspace/didChangeWatchedFiles/dynamicRegistration"_json_pointer, false);

	if (!dynamic)
	{
		return;
	}

	json watchers = json::array();

	for (std::string_view glob : priv::watched_globs)
	{
		watchers.push_back({{"globPattern", glob}});
	}

	rpc.send_request(
		"client/registerCapability",
		{{"registrations",
		  {{{"id", "lunaut-watched-files"},
			{"method", "workspace/didChangeWatchedFiles"},
			{"registerOptions", {{"watchers", watchers}}}}}}});
}

workspace& server::get_workspace(const std::string& address)
{
	workspace* best = nullptr;
	size_t best_length = 0;

	for (const std::unique_ptr<workspace>& folder : workspaces)
	{
		size_t length = folder->get_root().generic_string().size();

		if (folder->is_inside(address) && length >= best_length)
		{
			best = folder.get();
			best_length = length;
		}
	}

	if (best == nullptr)
	{
		best = null_workspace.get();
	}

	if (best == nullptr)
	{
		throw std::runtime_error("no workspace available");
	}

	best->initialize();
	return *best;
}

settingsinfo server::get_settings_for(const std::filesystem::path& root) const
{
	settingsinfo settings = settingsinfo::from_json(client_settings).with_project(root, options.settings_file);
	settings.definitions.insert(settings.definitions.end(), options.definitions.begin(), options.definitions.end());
	return settings;
}

namespace
{
namespace priv
{

json get_client_settings(const json& value)
{
	if (!value.is_object())
	{
		return json::object();
	}

	if (value.contains("vscode") && value["vscode"].is_object())
	{
		return settingsinfo::convert_vscode_settings(value["vscode"]);
	}

	if (value.contains("lunaut") && value["lunaut"].is_object())
	{
		return value["lunaut"];
	}

	if (value.contains("settings") && value["settings"].is_object())
	{
		return get_client_settings(value["settings"]);
	}

	return value;
}

}
}

}
