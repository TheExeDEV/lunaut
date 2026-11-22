#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Common.h"

LUAU_FASTFLAG(LuauSolverV2)

#include <algorithm>
#include <cstdlib>

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
	{"textDocument/declaration", &server::on_declaration},
	{"textDocument/typeDefinition", &server::on_type_definition},
	{"textDocument/implementation", &server::on_implementation},
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
	{"textDocument/codeLens", &server::on_code_lens},
	{"codeLens/resolve", &server::on_code_lens_resolve},
	{"textDocument/selectionRange", &server::on_selection_range},
	{"textDocument/inlineValue", &server::on_inline_value},
	{"textDocument/moniker", &server::on_moniker},
	{"textDocument/prepareTypeHierarchy", &server::on_prepare_type_hierarchy},
	{"typeHierarchy/supertypes", &server::on_supertypes},
	{"typeHierarchy/subtypes", &server::on_subtypes},
	{"lunaut/status", &server::on_lunaut_status},
	{"lunaut/typeStats", &server::on_lunaut_type_stats},
	{"lunaut/fflags", &server::on_lunaut_fflags},
	{"lunaut/definitions", &server::on_lunaut_definitions},
	{"lunaut/sourcemap", &server::on_lunaut_sourcemap},
	{"lunaut/rojoProjects", &server::on_lunaut_rojo_projects},
	{"workspace/executeCommand", &server::on_execute_command},
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

const std::vector<std::pair<std::string_view, std::string_view>> relevant_actions = {
	{"Lunaut: Regenerate Rojo Sourcemap", "lunaut.server.regenerateSourcemap"},
	{"Lunaut: Show Status", "lunaut.server.showStatus"},
	{"Lunaut: Reload Workspace", "lunaut.server.reloadWorkspace"},
	{"Lunaut: Install Roblox Studio Plugin", "lunaut.server.installStudioPlugin"},
};

const std::unordered_map<std::string_view, request_handler> relevant_commands = {
	{"lunaut.server.regenerateSourcemap", &server::invoke_regenerate_sourcemap},
	{"lunaut.server.showStatus", &server::invoke_show_status},
	{"lunaut.server.reloadWorkspace", &server::invoke_reload_workspace},
	{"lunaut.server.installStudioPlugin", &server::invoke_install_studio_plugin},
};

constexpr std::string_view studio_plugin_url =
	"https://github.com/TheExeDEV/lunaut/releases/download/v" LUNAUT_VERSION "/LunautPlugin.rbxm";
constexpr std::string_view studio_plugin_name = "LunautPlugin.rbxm";

constexpr std::string_view extension_client_name = "lunaut-vscode";

const std::vector<std::pair<std::string_view, std::string_view>> relevant_refreshes = {
	{"/workspace/inlayHint/refreshSupport", "workspace/inlayHint/refresh"},
	{"/workspace/semanticTokens/refreshSupport", "workspace/semanticTokens/refresh"},
};

const std::vector<std::string_view> watched_globs = {"**/*.luau", "**/*.lua", "**/.luaurc", "**/sourcemap.json", "**/*.project.json"};

json get_client_settings(const json& value);
std::optional<std::filesystem::path> get_studio_plugins_folder();

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

	auto flush_diagnostics = [&]()
	{
		std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

		for (const std::unique_ptr<workspace>& folder : workspaces)
		{
			std::optional<std::chrono::steady_clock::time_point> deadline = folder->diagnostics_deadline();

			if (deadline && *deadline > now)
			{
				continue;
			}

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
			std::optional<std::chrono::steady_clock::time_point> deadline = null_workspace->diagnostics_deadline();

			if (deadline && *deadline > now)
			{
				return;
			}

			try
			{
				null_workspace->flush_diagnostics();
			}
			catch (...)
			{
				send_log(enumitem_message_type::Error, "publishing diagnostics failed");
			}
		}
	};

	auto next_deadline = [&]() -> std::optional<std::chrono::steady_clock::time_point>
	{
		std::optional<std::chrono::steady_clock::time_point> deadline;

		for (const std::unique_ptr<workspace>& folder : workspaces)
		{
			std::optional<std::chrono::steady_clock::time_point> candidate = folder->diagnostics_deadline();

			if (candidate && (!deadline || *candidate < *deadline))
			{
				deadline = candidate;
			}
		}

		if (null_workspace)
		{
			std::optional<std::chrono::steady_clock::time_point> candidate = null_workspace->diagnostics_deadline();

			if (candidate && (!deadline || *candidate < *deadline))
			{
				deadline = candidate;
			}
		}

		return deadline;
	};

	while (state != enumitem_server_state::Exited)
	{
		std::optional<std::chrono::steady_clock::time_point> deadline = next_deadline();

		if (!deadline)
		{
			flush_diagnostics();

			std::optional<json> message = take_message();

			if (!message)
			{
				break;
			}

			process_message(*message);
			continue;
		}

		std::optional<json> message = take_message(deadline);

		if (!message)
		{
			if (input_closed)
			{
				break;
			}

			flush_diagnostics();
			continue;
		}

		process_message(*message);
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

std::optional<json> server::take_message(std::optional<std::chrono::steady_clock::time_point> deadline)
{
	std::unique_lock<std::mutex> guard(queue_lock);

	if (deadline)
	{
		queue_ready.wait_until(guard, *deadline, [this]() { return !queue.empty() || input_closed.load(); });
	}
	else
	{
		queue_ready.wait(guard, [this]() { return !queue.empty() || input_closed.load(); });
	}

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
	extension_client = params.value("initializationOptions", json::object()).value("client", "") == priv::extension_client_name;

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

json server::on_declaration(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_declaration(params);
}

json server::on_type_definition(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_type_definition(params);
}

json server::on_implementation(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_implementations(params);
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
	std::string address = params["textDocument"].value("uri", "");
	json actions = get_workspace(address).get_code_actions(params);

	if (!actions.is_array())
	{
		actions = json::array();
	}

	if (extension_client)
	{
		return actions;
	}

	for (const auto& [title, command] : priv::relevant_actions)
	{
		actions.push_back({{"title", title}, {"command", {{"title", title}, {"command", command}, {"arguments", json::array({address})}}}});
	}

	return actions;
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
	workspace& folder = get_workspace(params["textDocument"].value("uri", ""));

	if (params.value("ch", "") != "\n")
	{
		return folder.get_on_type_edits(params);
	}

	json edits = folder.get_end_edits(params).value("edits", json::array());
	return edits.empty() ? json(nullptr) : edits;
}

json server::on_formatting(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_formatting(params);
}

json server::on_range_formatting(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_formatting(params);
}

json server::on_code_lens(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_code_lenses(params);
}

json server::on_code_lens_resolve(const json& params)
{
	return get_workspace(params.value("/data/uri"_json_pointer, "")).resolve_code_lens(params);
}

json server::on_selection_range(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_selection_ranges(params);
}

json server::on_inline_value(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_inline_values(params);
}

json server::on_moniker(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_monikers(params);
}

json server::on_prepare_type_hierarchy(const json& params)
{
	return get_workspace(params["textDocument"].value("uri", "")).get_type_hierarchy_items(params);
}

json server::on_supertypes(const json& params)
{
	return get_workspace(params.value("/item/data/origin"_json_pointer, "")).get_supertypes(params);
}

json server::on_subtypes(const json& params)
{
	return get_workspace(params.value("/item/data/origin"_json_pointer, "")).get_subtypes(params);
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

json server::on_execute_command(const json& params)
{
	const auto found = priv::relevant_commands.find(params.value("command", ""));

	if (found == priv::relevant_commands.end())
	{
		return nullptr;
	}

	return (this->*found->second)(params.value("arguments", json::array()));
}

json server::invoke_regenerate_sourcemap(const json& arguments)
{
	json results = on_lunaut_sourcemap({{"regenerate", true}});

	if (results.empty())
	{
		send_window_message(enumitem_message_type::Warning, "Lunaut: open a folder to regenerate its sourcemap");
		return nullptr;
	}

	for (const json& result : results)
	{
		std::string folder = std::filesystem::path(result.value("root", "")).filename().string();

		if (result.value("success", false))
		{
			send_window_message(
				enumitem_message_type::Info,
				"Lunaut: sourcemap of " + folder + " regenerated, " + std::to_string(result.value("instances", 0)) + " instances");
		}
		else
		{
			send_window_message(
				enumitem_message_type::Error, "Lunaut: sourcemap of " + folder + " was not regenerated: " + result.value("message", ""));
		}
	}

	return nullptr;
}

json server::invoke_show_status(const json& arguments)
{
	std::string address = arguments.is_array() && !arguments.empty() && arguments[0].is_string() ? arguments[0].get<std::string>() : "";
	json status = get_workspace(address).get_status();
	std::string definitions;

	for (const json& definition : status.value("definitions", json::array()))
	{
		definitions +=
			(definitions.empty() ? "" : ", ") + definition.value("name", "") + (definition.value("success", false) ? "" : " (failed)");
	}

	std::string sourcemap = status.value("/sourcemap/loaded"_json_pointer, false)
		? std::to_string(status.value("/sourcemap/nodes"_json_pointer, 0)) + " instances"
		: "not loaded";

	send_window_message(
		enumitem_message_type::Info,
		std::string("Lunaut ") + LUNAUT_VERSION + " (Luau " + LUNAUT_LUAU_VERSION + "), " + status.value("platform", "standard") + ", " +
			status.value("solver", "old") + " solver; definitions: " + (definitions.empty() ? "none" : definitions) +
			"; sourcemap: " + sourcemap);
	return nullptr;
}

json server::invoke_install_studio_plugin(const json& arguments)
{
	std::optional<std::filesystem::path> folder = priv::get_studio_plugins_folder();

	if (!folder)
	{
		send_window_message(enumitem_message_type::Error, "Lunaut: Roblox Studio was not found on this computer");
		return nullptr;
	}

	std::error_code error;
	std::filesystem::create_directories(*folder, error);
	std::filesystem::path target = *folder / std::string(priv::studio_plugin_name);
	result<nothing> downloaded = http::download_file(std::string(priv::studio_plugin_url), target);

	if (!downloaded)
	{
		send_window_message(enumitem_message_type::Error, "Lunaut: could not install the Roblox Studio plugin: " + downloaded.message);
		return nullptr;
	}

	send_window_message(
		enumitem_message_type::Info,
		"Lunaut: installed the Roblox Studio plugin into " + target.generic_string() +
			". Restart Studio, then press Plugins > Lunaut > Sync, and turn on lunaut.studioPlugin.enabled in VS Code");
	return {{"path", target.generic_string()}};
}

json server::invoke_reload_workspace(const json& arguments)
{
	std::vector<workspace*> folders;

	for (const std::unique_ptr<workspace>& folder : workspaces)
	{
		folders.push_back(folder.get());
	}

	fflags = fflagregistry::apply(
		get_settings_for(folders.empty() ? std::filesystem::path() : folders.front()->get_root()).fflags,
		options.fflags,
		options.no_flags_enabled,
		*this);

	for (workspace* folder : folders)
	{
		reload_workspace(folder, "reload requested");
	}

	send_window_message(enumitem_message_type::Info, "Lunaut: reloaded " + std::to_string(folders.size()) + " workspace(s)");
	return nullptr;
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
		std::string root = folder->get_root().generic_string();

		if (!folder->is_roblox())
		{
			rebuilt.push_back(
				{{"root", root}, {"success", false}, {"message", "not a Roblox project: no *.project.json or sourcemap found"}});
			continue;
		}

		result<nothing> outcome = folder->rebuild_sourcemap(params.value("regenerate", true));
		size_t instances = folder->get_status().value("/sourcemap/nodes"_json_pointer, 0);
		rebuilt.push_back({{"root", root}, {"success", outcome.state}, {"message", outcome.message}, {"instances", instances}});
	}

	return rebuilt;
}

json server::on_lunaut_rojo_projects(const json& params)
{
	json folders = json::array();

	for (const std::unique_ptr<workspace>& folder : workspaces)
	{
		const std::filesystem::path& root = folder->get_root();
		const rojoinfo& info = folder->get_settings().rojo;
		json projects = json::array();

		for (const std::string& project : rojo::find_projects(root))
		{
			projects.push_back({{"path", project}, {"usable", rojo::has_existing_paths(root / project)}});
		}

		std::optional<std::string> selected = rojo::find_project(root, info);
		folders.push_back({
			{"root", root.generic_string()},
			{"projects", projects},
			{"selected", selected ? json(*selected) : json(nullptr)},
			{"setting", info.project_file},
		});
	}

	return folders;
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

	std::vector<workspace*> reloads;

	for (const std::unique_ptr<workspace>& folder : workspaces)
	{
		settingsinfo next = get_settings_for(folder->get_root());

		if (folder->needs_reload(next))
		{
			reloads.push_back(folder.get());
			continue;
		}

		folder->set_settings(std::move(next));
	}

	if (!reloads.empty() && !workspaces.empty())
	{
		fflags =
			fflagregistry::apply(get_settings_for(workspaces.front()->get_root()).fflags, options.fflags, options.no_flags_enabled, *this);
	}

	for (workspace* folder : reloads)
	{
		reload_workspace(folder, "settings for definitions or FFlags changed");
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
		bool definitions = std::any_of(
			changes.begin(),
			changes.end(),
			[folder](const json& change) { return folder->is_definition_file(uri::to_path(change.value("uri", ""))); });

		if (definitions && folder->get_settings().restart_on_definition_changes)
		{
			reload_workspace(folder, "a definition file changed");
			continue;
		}

		folder->apply_watched_changes(changes);
	}
}

void server::reload_workspace(workspace* folder, const std::string& reason)
{
	const auto found = std::find_if(
		workspaces.begin(), workspaces.end(), [folder](const std::unique_ptr<workspace>& item) { return item.get() == folder; });

	if (found == workspaces.end())
	{
		return;
	}

	std::filesystem::path root = folder->get_root();
	std::vector<textdocument> open = folder->get_open_documents();

	*found = std::make_unique<workspace>(*this, root, get_settings_for(root));
	(*found)->initialize();

	for (const textdocument& document : open)
	{
		(*found)->open_document(document.get_uri(), document.get_text(), document.get_version());
	}

	send_log(enumitem_message_type::Info, "reloaded " + root.generic_string() + ": " + reason);
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
	result<bool> declared = declarefile::apply_missing(root);

	if (!declared)
	{
		send_log(enumitem_message_type::Warning, declared.message);
	}
	else if (declared.value)
	{
		send_log(enumitem_message_type::Info, "added the missing keys to " + declarefile::get_path(root).generic_string());
	}

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

std::optional<std::filesystem::path> get_studio_plugins_folder()
{
	std::error_code error;

#if LUNAUT_PLATFORM_WINDOWS
	const char* local = std::getenv("LOCALAPPDATA");

	if (local != nullptr && std::filesystem::is_directory(std::filesystem::path(local) / "Roblox", error))
	{
		return std::filesystem::path(local) / "Roblox" / "Plugins";
	}
#else
	const char* home = std::getenv("HOME");

	if (home != nullptr && std::filesystem::is_directory(std::filesystem::path(home) / "Documents" / "Roblox", error))
	{
		return std::filesystem::path(home) / "Documents" / "Roblox" / "Plugins";
	}
#endif

	return std::nullopt;
}

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
