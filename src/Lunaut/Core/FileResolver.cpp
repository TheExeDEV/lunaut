#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Ast.h"
#include "Luau/LuauConfig.h"

#include "lualib.h"

#include <algorithm>
#include <chrono>

namespace lunaut
{
namespace
{
namespace priv
{

const std::vector<std::string_view> script_suffixes = {".server.luau", ".client.luau", ".server.lua", ".client.lua"};
const std::vector<std::string_view> module_suffixes = {".luau", ".lua"};
const std::vector<std::string_view> init_files = {"init.luau", "init.lua"};
constexpr std::chrono::milliseconds config_timeout(2000);

thread_local std::chrono::steady_clock::time_point config_deadline;

bool exists(const std::filesystem::path& path);
bool is_init_file(const std::filesystem::path& path);
std::string get_module_stem(const std::filesystem::path& path);
void interrupt_config(lua_State* state, int gc);

}
}

fileresolver::fileresolver(std::filesystem::path root)
	: root(std::move(root))
{
	default_config.mode = Luau::Mode::Nonstrict;
	requireSuggester = std::make_shared<requiresuggester>(*this);
}

std::optional<Luau::SourceCode> fileresolver::readSource(const Luau::ModuleName& name)
{
	std::optional<std::string> path = get_real_path(name);

	if (!path)
	{
		return std::nullopt;
	}

	std::optional<std::string> text = read_text(*path);

	if (!text)
	{
		return std::nullopt;
	}

	Luau::SourceCode::Type type = Luau::SourceCode::Type::Module;
	const sourcenode* node = nullptr;

	if (map != nullptr)
	{
		node = sourcemap::is_virtual_path(name) ? map->get_by_virtual_path(name) : map->get_by_real_path(*path);
	}

	if (node != nullptr && node->get_source_type() != Luau::SourceCode::Type::None)
	{
		type = node->get_source_type();
	}
	else
	{
		for (std::string_view suffix : priv::script_suffixes)
		{
			if (stdx::ends_with(*path, suffix))
			{
				type = Luau::SourceCode::Type::Script;
			}
		}
	}

	return Luau::SourceCode{*text, type};
}

std::optional<Luau::ModuleInfo>
fileresolver::resolveModule(const Luau::ModuleInfo* context, Luau::AstExpr* expr, const Luau::TypeCheckLimits& limits)
{
	bool rojo = map != nullptr && map->is_loaded();

	if (Luau::AstExprConstantString* value = expr->as<Luau::AstExprConstantString>())
	{
		std::string required(value->value.data, value->value.size);

		if (rojo && context != nullptr)
		{
			Luau::ModuleInfo virtual_context = *context;

			if (!sourcemap::is_virtual_path(context->name))
			{
				if (std::optional<std::string> virtual_path = map->get_virtual_path(context->name))
				{
					virtual_context.name = *virtual_path;
				}
			}

			if (std::optional<Luau::ModuleInfo> resolved = robloxresolver::resolve_string(*map, &virtual_context, required))
			{
				return resolved;
			}
		}

		return resolve_string_require(context, required);
	}

	if (!rojo)
	{
		return std::nullopt;
	}

	return robloxresolver::resolve_expression(
		*map, context, expr, [this](const Luau::ModuleName& name) { return map->get_virtual_path(name); });
}

std::string fileresolver::getHumanReadableModuleName(const Luau::ModuleName& name) const
{
	if (sourcemap::is_virtual_path(name))
	{
		return name;
	}

	return uri::get_relative(root, name);
}

const Luau::Config& fileresolver::getConfig(const Luau::ModuleName& name, const Luau::TypeCheckLimits& limits) const
{
	std::optional<std::string> path = get_real_path(name);

	if (!path)
	{
		return default_config;
	}

	return get_directory_config(std::filesystem::path(*path).parent_path());
}

Luau::ModuleName fileresolver::get_module_name(const std::string& address) const
{
	std::string path = uri::to_path(address);

	if (map != nullptr)
	{
		if (std::optional<std::string> virtual_path = map->get_virtual_path(path))
		{
			return *virtual_path;
		}
	}

	return path;
}

std::string fileresolver::get_uri(const Luau::ModuleName& name) const
{
	std::optional<std::string> path = get_real_path(name);

	if (!path)
	{
		return {};
	}

	return uri::from_path(*path);
}

std::optional<std::string> fileresolver::get_real_path(const Luau::ModuleName& name) const
{
	if (!sourcemap::is_virtual_path(name))
	{
		return name;
	}

	if (map == nullptr)
	{
		return std::nullopt;
	}

	return map->get_real_path(name);
}

std::optional<std::string> fileresolver::read_text(const std::string& real_path) const
{
	const auto found = documents.find(uri::normalize_path(real_path));

	if (found != documents.end())
	{
		return found->second.get_text();
	}

	return stdx::read_file(real_path);
}

const textdocument* fileresolver::get_document(const std::string& address) const
{
	const auto found = documents.find(uri::to_path(address));
	return found == documents.end() ? nullptr : &found->second;
}

textdocument* fileresolver::get_document(const std::string& address)
{
	const auto found = documents.find(uri::to_path(address));
	return found == documents.end() ? nullptr : &found->second;
}

void fileresolver::set_document(textdocument document)
{
	std::string path = uri::to_path(document.get_uri());
	documents.insert_or_assign(path, std::move(document));
}

void fileresolver::rem_document(const std::string& address)
{
	documents.erase(uri::to_path(address));
}

void fileresolver::set_sourcemap(const sourcemap* next)
{
	map = next;
}

void fileresolver::clear_config_cache()
{
	configs.clear();
}

std::optional<Luau::ModuleInfo> fileresolver::resolve_string_require(const Luau::ModuleInfo* context, const std::string& required)
{
	if (context == nullptr)
	{
		return std::nullopt;
	}

	std::optional<std::string> real = get_real_path(context->name);

	if (!real)
	{
		return std::nullopt;
	}

	std::optional<std::filesystem::path> target = get_require_target(*real, required);

	if (!target)
	{
		return std::nullopt;
	}

	std::string resolved = resolve_file_candidate(*target);

	if (map != nullptr)
	{
		if (std::optional<std::string> virtual_path = map->get_virtual_path(resolved))
		{
			return Luau::ModuleInfo{*virtual_path, context->optional};
		}
	}

	return Luau::ModuleInfo{resolved, context->optional};
}

std::string fileresolver::resolve_file_candidate(const std::filesystem::path& base) const
{
	std::string text = base.generic_string();

	for (std::string_view suffix : priv::module_suffixes)
	{
		if (stdx::ends_with(text, suffix) && priv::exists(base))
		{
			return uri::normalize_path(text);
		}
	}

	for (std::string_view suffix : priv::module_suffixes)
	{
		std::filesystem::path candidate(text + std::string(suffix));

		if (priv::exists(candidate))
		{
			return uri::normalize_path(candidate.generic_string());
		}
	}

	for (std::string_view init : priv::init_files)
	{
		std::filesystem::path candidate = base / std::string(init);

		if (priv::exists(candidate))
		{
			return uri::normalize_path(candidate.generic_string());
		}
	}

	return uri::normalize_path(text + ".luau");
}

const Luau::Config& fileresolver::get_directory_config(const std::filesystem::path& directory) const
{
	std::string key = uri::normalize_path(directory.generic_string());
	const auto found = configs.find(key);

	if (found != configs.end())
	{
		return *found->second;
	}

	std::filesystem::path parent = directory.parent_path();
	bool top = key.empty() || parent == directory || parent.empty();
	std::unique_ptr<Luau::Config> config = std::make_unique<Luau::Config>(top ? default_config : get_directory_config(parent));

	read_config_file(directory, *config);

	const Luau::Config& stored = *config;
	configs.emplace(key, std::move(config));
	return stored;
}

void fileresolver::set_base_config(const std::filesystem::path& path)
{
	default_config = Luau::Config{};
	default_config.mode = Luau::Mode::Nonstrict;
	configs.clear();

	if (path.empty())
	{
		return;
	}

	std::optional<std::string> contents = stdx::read_file(path);

	if (!contents)
	{
		stdx::oute("base luaurc not found: " + path.generic_string());
		return;
	}

	Luau::ConfigOptions options;
	options.aliasOptions = Luau::ConfigOptions::AliasOptions{uri::normalize_path(path.parent_path().generic_string()), true};

	if (std::optional<std::string> error = Luau::parseConfig(*contents, default_config, options))
	{
		stdx::oute(path.generic_string() + ": " + *error);
	}
}

std::optional<std::filesystem::path> fileresolver::get_require_target(const std::string& requirer, const std::string& required) const
{
	std::filesystem::path file(requirer);
	std::filesystem::path directory = file.parent_path();
	bool init = priv::is_init_file(file);
	std::filesystem::path base;
	std::string rest = required;

	if (stdx::starts_with(required, "@"))
	{
		size_t slash = required.find('/');
		std::string alias = stdx::to_lower(required.substr(1, slash == std::string::npos ? std::string::npos : slash - 1));
		rest = slash == std::string::npos ? "" : required.substr(slash + 1);

		if (alias == "self")
		{
			base = init ? directory : directory / priv::get_module_stem(file);
		}
		else
		{
			const Luau::Config& config = get_directory_config(directory);
			const Luau::Config::AliasInfo* found = config.aliases.find(alias);

			if (found == nullptr)
			{
				return std::nullopt;
			}

			std::filesystem::path value(found->value);
			base = value.is_absolute() ? value : std::filesystem::path(std::string(found->configLocation)) / value;
		}
	}
	else
	{
		base = init ? directory.parent_path() : directory;
	}

	while (!rest.empty() && rest.back() == '/')
	{
		rest.pop_back();
	}

	std::filesystem::path target = rest.empty() ? base : base / rest;
	return target.lexically_normal();
}

std::vector<Luau::RequireAlias> fileresolver::get_aliases(const std::filesystem::path& directory) const
{
	std::vector<Luau::RequireAlias> aliases;
	const Luau::Config& config = get_directory_config(directory);

	for (const auto& [name, info] : config.aliases)
	{
		aliases.emplace_back(info.originalCase.empty() ? name : info.originalCase);
	}

	aliases.emplace_back("self");
	return aliases;
}

void fileresolver::read_config_file(const std::filesystem::path& directory, Luau::Config& config) const
{
	std::string key = uri::normalize_path(directory.generic_string());

	if (std::optional<std::string> contents = stdx::read_file(directory / Luau::kLuauConfigName))
	{
		Luau::InterruptCallbacks callbacks;
		callbacks.initCallback = [](lua_State*) {};
		callbacks.interruptCallback = &priv::interrupt_config;
		priv::config_deadline = std::chrono::steady_clock::now() + priv::config_timeout;

		if (std::optional<std::string> error =
				Luau::extractLuauConfig(*contents, config, Luau::ConfigOptions::AliasOptions{key, true}, std::move(callbacks)))
		{
			stdx::oute(key + "/" + Luau::kLuauConfigName + ": " + *error);
		}
	}

	if (std::optional<std::string> contents = stdx::read_file(directory / Luau::kConfigName))
	{
		Luau::ConfigOptions options;
		options.aliasOptions = Luau::ConfigOptions::AliasOptions{key, true};

		if (std::optional<std::string> error = Luau::parseConfig(*contents, config, options))
		{
			stdx::oute(key + "/" + Luau::kConfigName + ": " + *error);
		}
	}
}

namespace
{
namespace priv
{

bool exists(const std::filesystem::path& path)
{
	std::error_code error;
	return std::filesystem::is_regular_file(path, error);
}

void interrupt_config(lua_State* state, int gc)
{
	if (gc < 0 && std::chrono::steady_clock::now() > config_deadline)
	{
		luaL_error(state, "configuration took too long to run");
	}
}

bool is_init_file(const std::filesystem::path& path)
{
	std::string name = path.filename().string();
	return std::find(init_files.begin(), init_files.end(), name) != init_files.end();
}

std::string get_module_stem(const std::filesystem::path& path)
{
	std::string name = path.filename().string();

	for (std::string_view suffix : module_suffixes)
	{
		if (stdx::ends_with(name, suffix))
		{
			return name.substr(0, name.size() - suffix.size());
		}
	}

	return name;
}

}
}

}
