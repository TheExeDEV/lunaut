#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

namespace lunaut
{
namespace
{
namespace priv
{

constexpr std::string_view vscode_prefix = "lunaut.";
constexpr std::string_view vscode_file = ".vscode/settings.json";

const std::unordered_map<std::string_view, std::string_view> relevant_vscode_keys = {
	{"lunaut.platform", "/platform"},
	{"lunaut.fflags.enableByDefault", "/fflags/enable_by_default"},
	{"lunaut.fflags.sync", "/fflags/sync"},
	{"lunaut.fflags.enableNewSolver", "/fflags/enable_new_solver"},
	{"lunaut.fflags.override", "/fflags/override"},
	{"lunaut.definitions", "/definitions"},
	{"lunaut.discoverDefinitions", "/discover_definitions"},
	{"lunaut.roblox.downloadTypes", "/roblox/download_types"},
	{"lunaut.roblox.securityLevel", "/roblox/security_level"},
	{"lunaut.roblox.typesPriority", "/roblox/types_priority"},
	{"lunaut.rojo.enabled", "/rojo/enabled"},
	{"lunaut.rojo.projectFile", "/rojo/project_file"},
	{"lunaut.rojo.sourcemapFile", "/rojo/sourcemap_file"},
	{"lunaut.rojo.autogenerate", "/rojo/autogenerate"},
	{"lunaut.rojo.includeNonScripts", "/rojo/include_non_scripts"},
	{"lunaut.rojo.strictDatamodelTypes", "/rojo/strict_datamodel_types"},
	{"lunaut.rojo.executable", "/rojo/executable"},
	{"lunaut.diagnostics.lint", "/diagnostics/lint"},
	{"lunaut.diagnostics.includeDependents", "/diagnostics/include_dependents"},
	{"lunaut.ignoreGlobs", "/ignore_globs"},
	{"lunaut.analyzeLuaFiles", "/analyze_lua_files"},
	{"lunaut.baseLuaurc", "/base_luaurc"},
};

template <typename T>
void read(const json& data, const char* key, T& target);
std::string to_string_value(const json& value);
void read_definitions(const json& data, std::vector<definitioninfo>& target);
std::string strip_trailing_commas(const std::string& source);
void flatten(const json& value, const std::string& prefix, std::map<std::string, json>& target);

}
}

settingsinfo settingsinfo::from_json(const json& data)
{
	return from_json(data, settingsinfo{});
}

settingsinfo settingsinfo::from_json(const json& data, const settingsinfo& base)
{
	settingsinfo info = base;

	if (!data.is_object())
	{
		return info;
	}

	if (data.contains("platform") && data["platform"].is_string())
	{
		info.platform = enum_platform_variant::from_name(data["platform"].get<std::string>()).value_or(info.platform);
	}

	if (data.contains("fflags") && data["fflags"].is_object())
	{
		const json& fflags = data["fflags"];
		priv::read(fflags, "enable_by_default", info.fflags.enable_by_default);
		priv::read(fflags, "sync", info.fflags.sync);
		priv::read(fflags, "enable_new_solver", info.fflags.enable_new_solver);

		if (fflags.contains("override") && fflags["override"].is_object())
		{
			for (const auto& [name, value] : fflags["override"].items())
			{
				info.fflags.override[name] = priv::to_string_value(value);
			}
		}
	}

	if (data.contains("definitions"))
	{
		priv::read_definitions(data["definitions"], info.definitions);
	}

	priv::read(data, "discover_definitions", info.discover_definitions);

	if (data.contains("roblox") && data["roblox"].is_object())
	{
		const json& roblox = data["roblox"];
		priv::read(roblox, "download_types", info.roblox.download_types);
		priv::read(roblox, "security_level", info.roblox.security_level);
		priv::read(roblox, "types_priority", info.roblox.types_priority);
	}

	if (data.contains("rojo") && data["rojo"].is_object())
	{
		const json& rojo = data["rojo"];
		priv::read(rojo, "enabled", info.rojo.enabled);
		priv::read(rojo, "project_file", info.rojo.project_file);
		priv::read(rojo, "sourcemap_file", info.rojo.sourcemap_file);
		priv::read(rojo, "autogenerate", info.rojo.autogenerate);
		priv::read(rojo, "include_non_scripts", info.rojo.include_non_scripts);
		priv::read(rojo, "strict_datamodel_types", info.rojo.strict_datamodel_types);
		priv::read(rojo, "executable", info.rojo.executable);
	}

	if (data.contains("diagnostics") && data["diagnostics"].is_object())
	{
		const json& diagnostics = data["diagnostics"];
		priv::read(diagnostics, "lint", info.diagnostics.lint);
		priv::read(diagnostics, "include_dependents", info.diagnostics.include_dependents);
	}

	priv::read(data, "analyze_lua_files", info.analyze_lua_files);
	priv::read(data, "base_luaurc", info.base_luaurc);

	if (data.contains("ignore_globs") && data["ignore_globs"].is_array())
	{
		info.ignore_globs.clear();
		for (const auto& glob : data["ignore_globs"])
		{
			if (glob.is_string())
			{
				info.ignore_globs.push_back(glob.get<std::string>());
			}
		}
	}

	return info;
}

settingsinfo settingsinfo::clone(const json& change) const
{
	return from_json(change, *this);
}

result<json> settingsinfo::read_vscode_settings(const std::filesystem::path& file)
{
	std::optional<std::string> contents = stdx::read_file(file);

	if (!contents)
	{
		return result<json>::fail("cannot read " + file.generic_string());
	}

	json data;

	try
	{
		data = json::parse(priv::strip_trailing_commas(*contents), nullptr, true, true);
	}
	catch (const json::exception& error)
	{
		return result<json>::fail(file.generic_string() + ": " + error.what());
	}

	std::map<std::string, json> flat;
	priv::flatten(data, "", flat);

	json converted = json::object();

	for (const auto& [key, value] : flat)
	{
		const auto found = priv::relevant_vscode_keys.find(key);

		if (found != priv::relevant_vscode_keys.end())
		{
			converted[json::json_pointer(std::string(found->second))] = value;
		}
	}

	return result<json>::ok(converted);
}

settingsinfo settingsinfo::with_project(const std::filesystem::path& root, const std::optional<std::string>& extra_file) const
{
	settingsinfo settings = *this;
	std::vector<std::filesystem::path> files;

	if (!root.empty())
	{
		files.push_back(root / std::string(priv::vscode_file));
	}

	if (extra_file)
	{
		files.emplace_back(*extra_file);
	}

	for (const std::filesystem::path& file : files)
	{
		std::error_code error;

		if (!std::filesystem::exists(file, error))
		{
			continue;
		}

		result<json> read = read_vscode_settings(file);

		if (!read)
		{
			stdx::oute(read.message);
			continue;
		}

		settings = settings.clone(read.value);
	}

	return settings;
}

json settingsinfo::to_json() const
{
	json definition_list = json::array();
	for (const auto& definition : definitions)
	{
		json item = {{"path", definition.path}, {"name", definition.name}, {"source", enum_definition_source::get_name(definition.source)}};
		if (definition.priority)
		{
			item["priority"] = *definition.priority;
		}
		definition_list.push_back(item);
	}

	return {
		{"platform", enum_platform_variant::get_name(platform)},
		{"fflags",
		 {{"enable_by_default", fflags.enable_by_default},
		  {"sync", fflags.sync},
		  {"enable_new_solver", fflags.enable_new_solver},
		  {"override", fflags.override}}},
		{"definitions", definition_list},
		{"discover_definitions", discover_definitions},
		{"roblox",
		 {{"download_types", roblox.download_types}, {"security_level", roblox.security_level}, {"types_priority", roblox.types_priority}}},
		{"rojo",
		 {{"enabled", rojo.enabled},
		  {"project_file", rojo.project_file},
		  {"sourcemap_file", rojo.sourcemap_file},
		  {"autogenerate", rojo.autogenerate},
		  {"include_non_scripts", rojo.include_non_scripts},
		  {"strict_datamodel_types", rojo.strict_datamodel_types},
		  {"executable", rojo.executable}}},
		{"diagnostics", {{"lint", diagnostics.lint}, {"include_dependents", diagnostics.include_dependents}}},
		{"ignore_globs", ignore_globs},
		{"analyze_lua_files", analyze_lua_files},
		{"base_luaurc", base_luaurc},
	};
}

json loadeddefinitioninfo::to_json() const
{
	return {
		{"name", name},
		{"path", path},
		{"priority", priority},
		{"success", success},
		{"message", message},
		{"source", enum_definition_source::get_name(source)},
	};
}

json fflagstateinfo::to_json() const
{
	return {{"name", name}, {"value", value}, {"kind", kind}, {"source", enum_fflag_source::get_name(source)}};
}

namespace
{
namespace priv
{

template <typename T>
void read(const json& data, const char* key, T& target)
{
	if (!data.contains(key))
	{
		return;
	}

	try
	{
		target = data[key].get<T>();
	}
	catch (const json::exception& error)
	{
		stdx::oute(std::string("invalid setting '") + key + "': " + error.what());
	}
}

std::string to_string_value(const json& value)
{
	if (value.is_string())
	{
		return value.get<std::string>();
	}

	if (value.is_boolean())
	{
		return value.get<bool>() ? "true" : "false";
	}

	return value.dump();
}

void read_definitions(const json& data, std::vector<definitioninfo>& target)
{
	if (data.is_object())
	{
		target.clear();
		for (const auto& [name, path] : data.items())
		{
			if (path.is_string())
			{
				target.push_back(definitioninfo{path.get<std::string>(), name, std::nullopt, enumitem_definition_source::Config});
			}
		}
		return;
	}

	if (!data.is_array())
	{
		return;
	}

	target.clear();

	for (const auto& item : data)
	{
		if (item.is_string())
		{
			target.push_back(definitioninfo{item.get<std::string>(), "", std::nullopt, enumitem_definition_source::Config});
			continue;
		}

		if (!item.is_object() || !item.contains("path") || !item["path"].is_string())
		{
			continue;
		}

		definitioninfo info;
		info.path = item["path"].get<std::string>();
		info.name = item.value("name", "");
		if (item.contains("priority") && item["priority"].is_number_integer())
		{
			info.priority = item["priority"].get<int>();
		}

		target.push_back(info);
	}
}

std::string strip_trailing_commas(const std::string& source)
{
	std::string output;
	output.reserve(source.size());
	bool in_string = false;
	bool escaped = false;

	for (size_t i = 0; i < source.size(); ++i)
	{
		char c = source[i];

		if (in_string)
		{
			output.push_back(c);
			escaped = !escaped && c == '\\';
			in_string = escaped || c != '"';
			continue;
		}

		if (c == '"')
		{
			in_string = true;
			output.push_back(c);
			continue;
		}

		if (c == '/' && i + 1 < source.size() && (source[i + 1] == '/' || source[i + 1] == '*'))
		{
			size_t end = source[i + 1] == '/' ? source.find('\n', i) : source.find("*/", i + 2);
			end = end == std::string::npos ? source.size() : end + (source[i + 1] == '/' ? 0 : 2);
			output.append(source, i, end - i);
			i = end - 1;
			continue;
		}

		if (c == ',')
		{
			size_t next = source.find_first_not_of(" \t\r\n", i + 1);

			if (next != std::string::npos && (source[next] == '}' || source[next] == ']'))
			{
				continue;
			}
		}

		output.push_back(c);
	}

	return output;
}

void flatten(const json& value, const std::string& prefix, std::map<std::string, json>& target)
{
	if (!prefix.empty() && relevant_vscode_keys.find(prefix) != relevant_vscode_keys.end())
	{
		target[prefix] = value;
		return;
	}

	if (!value.is_object())
	{
		return;
	}

	for (const auto& [key, child] : value.items())
	{
		std::string next = prefix.empty() ? key : prefix + "." + key;

		if (stdx::starts_with(next, vscode_prefix) || stdx::starts_with(std::string(vscode_prefix), next + "."))
		{
			flatten(child, next, target);
		}
	}
}

}
}

}
