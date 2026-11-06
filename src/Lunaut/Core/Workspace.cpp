#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/BuiltinDefinitions.h"
#include "Luau/Common.h"
#include "Luau/TypeArena.h"

LUAU_FASTFLAG(LuauSolverV2)

namespace lunaut
{
namespace
{
namespace priv
{

Luau::FrontendOptions make_options(bool for_autocomplete, bool lint);
std::filesystem::path get_base_luaurc(const std::filesystem::path& root, const settingsinfo& settings);
json make_definition_diagnostics(const textdocument& document, const Luau::LoadDefinitionFileResult& loaded, Luau::FileResolver* resolver);

}
}

workspace::workspace(client& owner, std::filesystem::path root, settingsinfo settings)
	: owner(owner)
	, root(std::move(root))
	, settings(std::move(settings))
	, resolver(this->root)
	, frontend(&resolver, &resolver, priv::make_options(false, true))
{
	frontend.setLuauSolverMode(FFlag::LuauSolverV2 ? Luau::SolverMode::New : Luau::SolverMode::Old);
	resolver.set_base_config(priv::get_base_luaurc(this->root, this->settings));
}

void workspace::initialize()
{
	if (ready)
	{
		return;
	}

	roblox = settings.platform == enumitem_platform_variant::Roblox ||
		(settings.platform == enumitem_platform_variant::Auto && rojo::is_project(root, settings.rojo));

	owner.send_log(
		enumitem_message_type::Info,
		"initializing workspace " + root.generic_string() + " (platform: " + (roblox ? "roblox" : "standard") +
			", solver: " + (FFlag::LuauSolverV2 ? "new" : "old") + ")");

	register_types();

	if (settings.features.documentation)
	{
		std::vector<std::filesystem::path> files;

		for (const std::string& file : settings.documentation_files)
		{
			std::filesystem::path path(file);
			files.push_back(path.is_absolute() ? path : root / path);
		}

		docs.load(roblox, files, owner);
	}

#if FFLAG_ROJO
	if (roblox && settings.rojo.enabled)
	{
		result<nothing> rebuilt = rebuild_sourcemap(settings.rojo.autogenerate);

		if (!rebuilt)
		{
			owner.send_log(enumitem_message_type::Warning, rebuilt.message);
		}
	}
#endif

	ready = true;
}

bool workspace::is_ready() const
{
	return ready;
}

bool workspace::is_roblox() const
{
	return roblox;
}

bool workspace::is_inside(const std::string& address) const
{
	std::string path = uri::to_path(address);
	std::string base = uri::normalize_path(root.generic_string());
	return !base.empty() && stdx::starts_with(path, base) && path.size() > base.size() && path[base.size()] == '/';
}

const std::filesystem::path& workspace::get_root() const
{
	return root;
}

const settingsinfo& workspace::get_settings() const
{
	return settings;
}

const std::vector<loadeddefinitioninfo>& workspace::get_definitions() const
{
	return definitions;
}

json workspace::get_status() const
{
	json loaded = json::array();

	for (const loadeddefinitioninfo& definition : definitions)
	{
		loaded.push_back(definition.to_json());
	}

	return {
		{"root", root.generic_string()},
		{"ready", ready},
		{"platform", roblox ? "roblox" : "standard"},
		{"solver", FFlag::LuauSolverV2 ? "new" : "old"},
		{"sourcemap", {{"loaded", map.is_loaded()}, {"nodes", map.count()}}},
		{"definitions", loaded},
		{"settings", settings.to_json()},
	};
}

void workspace::open_document(const std::string& address, const std::string& text, int version)
{
	resolver.set_document(textdocument(address, text, version));
	frontend.markDirty(resolver.get_module_name(address));
	publish_diagnostics(address);
}

void workspace::update_document(const std::string& address, const json& changes, int version)
{
	textdocument* document = resolver.get_document(address);

	if (document == nullptr)
	{
		owner.send_log(enumitem_message_type::Warning, "change for unknown document: " + address);
		return;
	}

	document->apply_changes(changes, version);

	Luau::ModuleName name = resolver.get_module_name(address);
	std::vector<Luau::ModuleName> dirty;
	frontend.markDirty(name, &dirty);

	pending.insert(address);

	if (!settings.diagnostics.include_dependents && !settings.diagnostics.workspace)
	{
		return;
	}

	for (const Luau::ModuleName& dependent : dirty)
	{
		std::string dependent_uri = resolver.get_uri(dependent);
		bool open = resolver.get_document(dependent_uri) != nullptr;

		if (dependent != name && !dependent_uri.empty() &&
			(settings.diagnostics.workspace || (open && settings.diagnostics.include_dependents)))
		{
			pending.insert(dependent_uri);
		}
	}
}

void workspace::flush_diagnostics()
{
	std::set<std::string> current;
	current.swap(pending);

	for (const std::string& address : current)
	{
		publish_diagnostics(address);
	}
}

void workspace::apply_plugin_tree(const json& tree)
{
	if (!roblox || !settings.rojo.enabled)
	{
		return;
	}

	plugin_tree = tree;
	instances.clear(frontend, map);

	if (tree.is_null())
	{
		map.clear_plugin();
	}
	else
	{
		map.merge_plugin(tree);
	}

	resolver.set_sourcemap(map.is_loaded() ? &map : nullptr);
	instances.apply(frontend, map, settings.rojo.strict_datamodel_types);
	owner.send_log(enumitem_message_type::Info, "applied Studio plugin tree: " + std::to_string(map.count()) + " instances");

	for (const auto& [module_name, node] : frontend.sourceNodes)
	{
		std::string address = resolver.get_uri(module_name);

		if (!address.empty() && resolver.get_document(address) != nullptr)
		{
			pending.insert(address);
		}
	}
}

void workspace::close_document(const std::string& address)
{
	resolver.rem_document(address);
	frontend.markDirty(resolver.get_module_name(address));
	owner.send_diagnostics(address, std::nullopt, json::array());
}

void workspace::save_document(const std::string& address)
{
	if (resolver.get_document(address) != nullptr)
	{
		publish_diagnostics(address);
	}
}

void workspace::apply_watched_changes(const json& changes)
{
	bool configs_changed = false;
	bool sourcemap_changed = false;
	bool regenerate = false;

	for (const json& change : changes)
	{
		std::string address = change.value("uri", "");
		std::string path = uri::to_path(address);
		std::string name = std::filesystem::path(path).filename().string();
		enumitem_file_change type = static_cast<enumitem_file_change>(change.value("type", 2));

		if (name == Luau::kConfigName)
		{
			configs_changed = true;
			continue;
		}

		if (name == settings.rojo.sourcemap_file)
		{
			sourcemap_changed = true;
			continue;
		}

		if (stdx::ends_with(name, ".project.json"))
		{
			regenerate = true;
			continue;
		}

		if (!stdx::ends_with(name, ".luau") && !stdx::ends_with(name, ".lua"))
		{
			continue;
		}

		if (is_definition_file(path))
		{
			owner.send_window_message(enumitem_message_type::Info, "Definitions file changed, restart lunaut to apply: " + name);
			continue;
		}

		if (type != enumitem_file_change::Changed)
		{
			regenerate = true;
		}

		frontend.markDirty(resolver.get_module_name(address));
	}

	if (configs_changed)
	{
		resolver.clear_config_cache();

		for (const auto& [module_name, node] : frontend.sourceNodes)
		{
			frontend.markDirty(module_name);
		}
	}

#if FFLAG_ROJO
	if (roblox && settings.rojo.enabled && (sourcemap_changed || (regenerate && settings.rojo.autogenerate)))
	{
		result<nothing> rebuilt = rebuild_sourcemap(regenerate && settings.rojo.autogenerate);

		if (!rebuilt)
		{
			owner.send_log(enumitem_message_type::Warning, rebuilt.message);
		}
	}
#endif

	for (const auto& [module_name, node] : frontend.sourceNodes)
	{
		std::string address = resolver.get_uri(module_name);

		if (!address.empty() && resolver.get_document(address) != nullptr)
		{
			pending.insert(address);
		}
	}

	publish_workspace_diagnostics();
}

void workspace::set_settings(settingsinfo next)
{
	bool definitions_changed = json(next.to_json()["definitions"]) != json(settings.to_json()["definitions"]) ||
		next.roblox.security_level != settings.roblox.security_level;
	bool rojo_changed = json(next.to_json()["rojo"]) != json(settings.to_json()["rojo"]);

	bool base_changed = next.base_luaurc != settings.base_luaurc;
	settings = std::move(next);

	if (base_changed)
	{
		resolver.set_base_config(priv::get_base_luaurc(root, settings));

		for (const auto& [module_name, node] : frontend.sourceNodes)
		{
			frontend.markDirty(module_name);
		}
	}

	if (definitions_changed && ready)
	{
		owner.send_window_message(enumitem_message_type::Info, "Definitions settings changed, restart lunaut to apply them");
	}

#if FFLAG_ROJO
	if (rojo_changed && roblox && ready)
	{
		rebuild_sourcemap(settings.rojo.autogenerate);
	}
#endif
}

result<nothing> workspace::rebuild_sourcemap(bool regenerate)
{
	if (!settings.rojo.enabled)
	{
		return result<nothing>::fail("rojo support is disabled");
	}

	std::filesystem::path file = root / settings.rojo.sourcemap_file;
	std::error_code error;

	if (regenerate || !std::filesystem::exists(file, error))
	{
		result<nothing> generated = rojo::generate_sourcemap(root, settings.rojo);

		if (generated)
		{
			owner.send_log(enumitem_message_type::Info, "generated sourcemap via rojo");
		}
		else
		{
			owner.send_log(enumitem_message_type::Warning, "sourcemap generation skipped: " + generated.message);
		}
	}

	instances.clear(frontend, map);
	resolver.set_sourcemap(nullptr);

	if (!std::filesystem::exists(file, error))
	{
		map.clear();
		return result<nothing>::fail("sourcemap not found: " + file.generic_string());
	}

	result<nothing> loaded = map.load_from_file(file, root);

	if (!loaded)
	{
		return loaded;
	}

	if (!plugin_tree.is_null())
	{
		map.merge_plugin(plugin_tree);
	}

	resolver.set_sourcemap(&map);
	instances.apply(frontend, map, settings.rojo.strict_datamodel_types);
	owner.send_log(enumitem_message_type::Info, "sourcemap loaded: " + std::to_string(map.count()) + " instances");

	return result<nothing>::ok({});
}

void workspace::register_types()
{
	Luau::registerBuiltinGlobals(frontend, frontend.globals);

	if (!FFlag::LuauSolverV2)
	{
		Luau::registerBuiltinGlobals(frontend, frontend.globalsForAutocomplete);
	}

	load_definitions();
	apply_disabled_globals(frontend.globals);

	if (!FFlag::LuauSolverV2)
	{
		apply_disabled_globals(frontend.globalsForAutocomplete);
	}

	Luau::freeze(frontend.globals.globalTypes);

	if (!FFlag::LuauSolverV2)
	{
		Luau::freeze(frontend.globalsForAutocomplete.globalTypes);
	}
}

void workspace::apply_disabled_globals(Luau::GlobalTypes& globals)
{
	for (const std::string& disabled : settings.disabled_globals)
	{
		size_t dot = disabled.find('.');
		std::string library = disabled.substr(0, dot);
		std::optional<std::string> member = dot == std::string::npos ? std::nullopt : std::optional<std::string>(disabled.substr(dot + 1));
		Luau::AstName name = globals.globalNames.names->get(library.c_str());
		auto binding = name.value == nullptr ? globals.globalScope->bindings.end() : globals.globalScope->bindings.find(name);

		if (binding == globals.globalScope->bindings.end())
		{
			owner.send_log(enumitem_message_type::Warning, "disabling globals: unknown global " + disabled);
			continue;
		}

		if (!member)
		{
			globals.globalScope->bindings.erase(binding);
			owner.send_log(enumitem_message_type::Info, "disabled global " + disabled);
			continue;
		}

		Luau::TypeId type = Luau::follow(binding->second.typeId);
		bool erased = false;

		if (Luau::TableType* table = Luau::getMutable<Luau::TableType>(type))
		{
			erased = table->props.erase(*member) > 0;
		}
		else if (Luau::ExternType* extern_type = Luau::getMutable<Luau::ExternType>(type))
		{
			erased = extern_type->props.erase(*member) > 0;
		}

		owner.send_log(
			erased ? enumitem_message_type::Info : enumitem_message_type::Warning,
			std::string(erased ? "disabled global " : "disabling globals: cannot remove ") + disabled);
	}
}

void workspace::load_definitions()
{
	definitions.clear();
	definition_paths.clear();

	std::set<std::string> packages;
	json metadata = json::object();

	for (const definitioninfo& info : definitionloader::collect(root, settings, roblox, owner))
	{
		loadeddefinitioninfo loaded;
		loaded.path = info.path;
		loaded.priority = info.priority.value_or(0);
		loaded.source = info.source;
		loaded.name = definitionloader::get_package_name(info, info.path);

		for (size_t index = 2; !packages.insert(loaded.name).second; ++index)
		{
			loaded.name = definitionloader::get_package_name(info, info.path) + "_" + std::to_string(index);
		}

		definition_paths.insert(info.path);
		std::optional<std::string> source = stdx::read_file(info.path);

		if (!source)
		{
			loaded.message = "cannot read file";
			definitions.push_back(loaded);
			owner.send_window_message(enumitem_message_type::Error, "Failed to read definitions file " + info.path);
			continue;
		}

		if (std::optional<json> found = robloxmagic::get_metadata(*source))
		{
			metadata = *found;
		}

		Luau::LoadDefinitionFileResult result =
			frontend.loadDefinitionFile(frontend.globals, frontend.globals.globalScope, *source, loaded.name, true, false);

		if (!FFlag::LuauSolverV2)
		{
			frontend.loadDefinitionFile(
				frontend.globalsForAutocomplete, frontend.globalsForAutocomplete.globalScope, *source, loaded.name, true, true);
		}

		std::string address = uri::from_path(info.path);
		textdocument document(address, *source, 0);
		json diagnostics = result.success ? json::array() : priv::make_definition_diagnostics(document, result, &resolver);

		loaded.success = result.success;

		if (!result.success && !diagnostics.empty())
		{
			loaded.message = diagnostics.front().value("message", "");
		}

		owner.send_diagnostics(address, std::nullopt, diagnostics);
		owner.send_log(
			result.success ? enumitem_message_type::Info : enumitem_message_type::Error,
			std::string(result.success ? "loaded" : "failed to load") + " definitions " + loaded.name + " (priority " +
				std::to_string(loaded.priority) + ", " + std::string(enum_definition_source::get_name(loaded.source)) + "): " + info.path);

		if (!result.success)
		{
			owner.send_window_message(enumitem_message_type::Error, "Failed to load definitions file " + info.path + ": " + loaded.message);
		}

		definitions.push_back(loaded);
	}

	roblox_metadata = metadata;

	if (!roblox)
	{
		return;
	}

	robloxmagic::apply(frontend.globals, metadata);

	if (!FFlag::LuauSolverV2)
	{
		robloxmagic::apply(frontend.globalsForAutocomplete, metadata);
	}
}

Luau::CheckResult workspace::check_module(const Luau::ModuleName& name, bool for_autocomplete)
{
	if (FFlag::LuauSolverV2)
	{
		for_autocomplete = false;
	}

	Luau::ModulePtr module = get_module(name, for_autocomplete);

	if (module && module->internalTypes && module->internalTypes->types.empty())
	{
		frontend.markDirty(name);
	}

	try
	{
		return frontend.check(name, priv::make_options(for_autocomplete, settings.diagnostics.lint));
	}
	catch (const std::exception& error)
	{
		owner.send_log(enumitem_message_type::Error, "typechecking " + name + " failed: " + error.what());
		return Luau::CheckResult{};
	}
}

Luau::ModulePtr workspace::get_module(const Luau::ModuleName& name, bool for_autocomplete) const
{
	if (FFlag::LuauSolverV2 || !for_autocomplete)
	{
		return frontend.moduleResolver.getModule(name);
	}

	return frontend.moduleResolverForAutocomplete.getModule(name);
}

bool workspace::is_definition_file(const std::string& path) const
{
	return definition_paths.find(uri::normalize_path(path)) != definition_paths.end();
}

bool workspace::is_ignored(const std::string& path) const
{
	std::string relative = uri::get_relative(root, path);

	for (const std::string& glob : settings.ignore_globs)
	{
		if (stdx::glob_match(glob, relative))
		{
			return true;
		}
	}

	return false;
}

std::optional<textdocument> workspace::get_text_for(const Luau::ModuleName& name) const
{
	std::optional<std::string> path = resolver.get_real_path(name);

	if (!path)
	{
		return std::nullopt;
	}

	std::string address = uri::from_path(*path);

	if (const textdocument* document = resolver.get_document(address))
	{
		return *document;
	}

	std::optional<std::string> text = resolver.read_text(*path);

	if (!text)
	{
		return std::nullopt;
	}

	return textdocument(address, *text, 0);
}

json workspace::get_location(const Luau::ModuleName& name, const Luau::Location& location) const
{
	std::optional<textdocument> document = get_text_for(name);

	if (!document)
	{
		return nullptr;
	}

	return {{"uri", document->get_uri()}, {"range", document->to_lsp_range(location)}};
}

namespace
{
namespace priv
{

Luau::FrontendOptions make_options(bool for_autocomplete, bool lint)
{
	Luau::FrontendOptions options;
	options.retainFullTypeGraphs = true;
	options.forAutocomplete = for_autocomplete;
	options.runLintChecks = lint;
	return options;
}

std::filesystem::path get_base_luaurc(const std::filesystem::path& root, const settingsinfo& settings)
{
	if (settings.base_luaurc.empty())
	{
		return {};
	}

	std::filesystem::path path(settings.base_luaurc);
	return path.is_absolute() || root.empty() ? path : root / path;
}

json make_definition_diagnostics(const textdocument& document, const Luau::LoadDefinitionFileResult& loaded, Luau::FileResolver* resolver)
{
	json diagnostics = json::array();

	for (const Luau::ParseError& error : loaded.parseResult.errors)
	{
		diagnostics.push_back(protocol::make_parse_error(document, error));
	}

	if (loaded.module)
	{
		for (const Luau::TypeError& error : loaded.module->errors)
		{
			diagnostics.push_back(protocol::make_type_error(document, error, resolver));
		}
	}

	return diagnostics;
}

}
}

}
