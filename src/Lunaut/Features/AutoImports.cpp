#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Ast.h"

#include <algorithm>

namespace lunaut
{
namespace
{
namespace priv
{

constexpr int kind_module = 9;

const std::vector<std::string_view> module_suffixes = {".luau", ".lua"};
const std::vector<std::string_view> init_files = {"init.luau", "init.lua"};

bool is_identifier(const std::string& name);
bool is_import(const Luau::AstStat* statement);
std::string get_keyword(const featuresinfo& features);
std::string get_module_symbol(const Luau::ModuleName& name);
std::string strip_module_suffix(const std::string& path);
std::string make_instance_path(const std::string& virtual_path);
std::vector<std::string> get_services(const json& metadata);
std::optional<std::string> get_alias_path(const Luau::Config& config, const std::string& target_path, enumitem_alias_path choice);

}
}

json workspace::get_auto_imports(const Luau::ModuleName& name, const textdocument& document, const std::set<std::string>& taken)
{
	json items = json::array();

	if (!settings.features.auto_imports)
	{
		return items;
	}

	json at = {{"line", get_import_line(name, document)}, {"character", 0}};
	std::set<std::string> added = taken;

	auto add = [&](const std::string& label, const std::string& statement, const std::string& detail)
	{
		if (!added.insert(label).second)
		{
			return;
		}

		items.push_back({
			{"label", label},
			{"kind", priv::kind_module},
			{"detail", detail},
			{"sortText", "8" + label},
			{"labelDetails", {{"description", "auto-import"}}},
			{"documentation", protocol::make_markdown("```luau\n" + statement + "\n```")},
			{"additionalTextEdits", {{{"range", {{"start", at}, {"end", at}}}, {"newText", statement + "\n"}}}},
		});
	};

	if (roblox)
	{
		for (const std::string& service : priv::get_services(roblox_metadata))
		{
			add(service,
				priv::get_keyword(settings.features) + service + " = game:GetService(\"" + service + "\")",
				"Auto-import service " + service);
		}
	}

	index_workspace();

	std::vector<Luau::ModuleName> modules;

	for (const auto& [module_name, node] : frontend.sourceNodes)
	{
		modules.push_back(module_name);
	}

	std::sort(modules.begin(), modules.end());

	for (const Luau::ModuleName& module_name : modules)
	{
		std::string symbol = priv::get_module_symbol(module_name);

		if (module_name == name || !priv::is_identifier(symbol) || added.count(symbol) != 0)
		{
			continue;
		}

		std::optional<std::string> real = resolver.get_real_path(module_name);

		if (real && is_definition_file(*real))
		{
			continue;
		}

		if (std::optional<std::string> statement = get_import_statement(name, module_name))
		{
			add(symbol, *statement, "Auto-import " + resolver.getHumanReadableModuleName(module_name));
		}
	}

	return items;
}

std::optional<json> workspace::make_import_edit(const Luau::ModuleName& name, const textdocument& document, const std::string& symbol)
{
	std::optional<std::string> statement;

	if (roblox)
	{
		std::vector<std::string> services = priv::get_services(roblox_metadata);

		if (std::find(services.begin(), services.end(), symbol) != services.end())
		{
			statement = priv::get_keyword(settings.features) + symbol + " = game:GetService(\"" + symbol + "\")";
		}
	}

	if (!statement)
	{
		index_workspace();

		for (const auto& [module_name, node] : frontend.sourceNodes)
		{
			if (module_name != name && priv::get_module_symbol(module_name) == symbol)
			{
				statement = get_import_statement(name, module_name);

				if (statement)
				{
					break;
				}
			}
		}
	}

	if (!statement)
	{
		return std::nullopt;
	}

	json at = {{"line", get_import_line(name, document)}, {"character", 0}};
	return json{{"range", {{"start", at}, {"end", at}}}, {"newText", *statement + "\n"}};
}

std::optional<std::string> workspace::get_import_statement(
	const Luau::ModuleName& from, const std::string& target, const std::string& requested_symbol) const
{
	std::string symbol = requested_symbol.empty() ? priv::get_module_symbol(target) : requested_symbol;

	if (sourcemap::is_virtual_path(target))
	{
		if (!sourcemap::is_virtual_path(from))
		{
			return std::nullopt;
		}

		std::optional<std::string> from_parent = sourcemap::get_parent_path(from);
		std::optional<std::string> target_parent = sourcemap::get_parent_path(target);

		if (from_parent && target_parent && *from_parent == *target_parent && priv::is_identifier(symbol))
		{
			return priv::get_keyword(settings.features) + symbol + " = require(script.Parent." + symbol + ")";
		}

		std::string path = priv::make_instance_path(target);

		if (path.empty())
		{
			return std::nullopt;
		}

		return priv::get_keyword(settings.features) + symbol + " = require(" + path + ")";
	}

	std::optional<std::string> from_real = resolver.get_real_path(from);

	if (!from_real || sourcemap::is_virtual_path(from))
	{
		return std::nullopt;
	}

	std::filesystem::path requirer(*from_real);
	std::filesystem::path directory = requirer.parent_path();
	bool init = std::find(priv::init_files.begin(), priv::init_files.end(), requirer.filename().string()) != priv::init_files.end();
	std::filesystem::path base = init ? directory.parent_path() : directory;

	std::filesystem::path module(target);
	std::string target_path =
		std::find(priv::init_files.begin(), priv::init_files.end(), module.filename().string()) != priv::init_files.end()
		? module.parent_path().generic_string()
		: priv::strip_module_suffix(module.generic_string());

	std::string relative = std::filesystem::path(target_path).lexically_relative(base).generic_string();

	if (relative.empty())
	{
		return std::nullopt;
	}

	std::string path = stdx::starts_with(relative, "../") ? relative : "./" + relative;
	bool nearby = !stdx::starts_with(relative, "../");
	bool use_alias = settings.features.require_style == enumitem_require_style::AlwaysAbsolute ||
		(settings.features.require_style == enumitem_require_style::Auto && !nearby);

	if (use_alias)
	{
		if (std::optional<std::string> aliased =
				priv::get_alias_path(resolver.get_directory_config(directory), target_path, settings.features.alias_path))
		{
			path = *aliased;
		}
	}

	return priv::get_keyword(settings.features) + symbol + " = require(\"" + path + "\")";
}

std::string workspace::get_import_symbol(const Luau::ModuleName& name) const
{
	return priv::get_module_symbol(name);
}

unsigned int workspace::get_import_line(const Luau::ModuleName& name, const textdocument& document)
{
	frontend.parse(name);
	const Luau::SourceModule* source = frontend.getSourceModule(name);
	std::optional<unsigned int> after;

	if (source != nullptr && source->root != nullptr)
	{
		for (const Luau::AstStat* statement : source->root->body)
		{
			if (!priv::is_import(statement))
			{
				break;
			}

			after = statement->location.end.line + 1;
		}
	}

	if (after)
	{
		return *after;
	}

	std::vector<std::string> lines = stdx::split(document.get_text(), '\n');
	unsigned int line = 0;

	while (line < lines.size() && stdx::starts_with(stdx::trim(lines[line]), "--!"))
	{
		++line;
	}

	return line;
}

namespace
{
namespace priv
{

std::string get_keyword(const featuresinfo& features)
{
	return std::string(enum_import_keyword::get_name(features.import_keyword)) + " ";
}

bool is_identifier(const std::string& name)
{
	if (name.empty() || (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_'))
	{
		return false;
	}

	return std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

bool is_import(const Luau::AstStat* statement)
{
	const Luau::AstStatLocal* local = statement->as<Luau::AstStatLocal>();

	if (local == nullptr || local->values.size != 1)
	{
		return false;
	}

	const Luau::AstExprCall* call = local->values.data[0]->as<Luau::AstExprCall>();

	if (call == nullptr)
	{
		return false;
	}

	if (const Luau::AstExprGlobal* global = call->func->as<Luau::AstExprGlobal>())
	{
		return global->name == "require";
	}

	if (const Luau::AstExprIndexName* index = call->func->as<Luau::AstExprIndexName>())
	{
		return index->index == "GetService";
	}

	return false;
}

std::string get_module_symbol(const Luau::ModuleName& name)
{
	if (sourcemap::is_virtual_path(name))
	{
		size_t slash = name.find_last_of('/');
		return slash == std::string::npos ? name : name.substr(slash + 1);
	}

	std::filesystem::path path(name);
	std::string file = path.filename().string();

	if (std::find(init_files.begin(), init_files.end(), file) != init_files.end())
	{
		return path.parent_path().filename().string();
	}

	file = strip_module_suffix(file);
	size_t dot = file.find('.');
	return dot == std::string::npos ? file : file.substr(0, dot);
}

std::string strip_module_suffix(const std::string& path)
{
	for (std::string_view suffix : module_suffixes)
	{
		if (stdx::ends_with(path, suffix))
		{
			return path.substr(0, path.size() - suffix.size());
		}
	}

	return path;
}

std::string make_instance_path(const std::string& virtual_path)
{
	std::vector<std::string> parts = stdx::split(virtual_path, '/');

	if (parts.size() < 2 || parts.front() != "game")
	{
		return {};
	}

	std::string path = "game:GetService(\"" + parts[1] + "\")";

	for (size_t i = 2; i < parts.size(); ++i)
	{
		path += is_identifier(parts[i]) ? "." + parts[i] : "[\"" + parts[i] + "\"]";
	}

	return path;
}

std::optional<std::string> get_alias_path(const Luau::Config& config, const std::string& target_path, enumitem_alias_path choice)
{
	std::string normalized = uri::normalize_path(target_path);
	std::optional<std::string> chosen;

	for (const auto& [alias, info] : config.aliases)
	{
		std::filesystem::path location(info.value);
		std::string alias_root = uri::normalize_path(
			(location.is_absolute() ? location : std::filesystem::path(std::string(info.configLocation)) / location).generic_string());

		if (normalized != alias_root && !stdx::starts_with(normalized, alias_root + "/"))
		{
			continue;
		}

		std::string candidate = "@" + (info.originalCase.empty() ? alias : info.originalCase) + normalized.substr(alias_root.size());
		bool better = !chosen ||
			(choice == enumitem_alias_path::Shortest ? candidate.size() < chosen->size() : candidate.size() > chosen->size()) ||
			(candidate.size() == chosen->size() && candidate < *chosen);

		if (better)
		{
			chosen = candidate;
		}
	}

	return chosen;
}

std::vector<std::string> get_services(const json& metadata)
{
	std::vector<std::string> services;

	if (!metadata.is_object() || !metadata.contains("SERVICES") || !metadata["SERVICES"].is_array())
	{
		return services;
	}

	for (const json& service : metadata["SERVICES"])
	{
		if (service.is_string() && is_identifier(service.get<std::string>()))
		{
			services.push_back(service.get<std::string>());
		}
	}

	return services;
}

}
}

}
