#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include <algorithm>

namespace lunaut
{
namespace
{
namespace priv
{

const std::vector<std::string_view> vscode_prefixes = {"lunaut.", "luau-lsp."};
constexpr std::string_view vscode_file = ".vscode/settings.json";

const std::unordered_map<std::string_view, std::string_view> relevant_vscode_keys = {
	{"lunaut.platform", "/platform"},
	{"lunaut.fflags.enableByDefault", "/fflags/enable_by_default"},
	{"lunaut.fflags.sync", "/fflags/sync"},
	{"lunaut.fflags.enableNewSolver", "/fflags/enable_new_solver"},
	{"lunaut.fflags.override", "/fflags/override"},
	{"lunaut.definitions", "/definitions"},
	{"lunaut.discoverDefinitions", "/discover_definitions"},
	{"lunaut.restartOnDefinitionChanges", "/restart_on_definition_changes"},
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
	{"lunaut.diagnostics.delay", "/diagnostics/delay"},
	{"lunaut.ignoreGlobs", "/ignore_globs"},
	{"lunaut.analyzeLuaFiles", "/analyze_lua_files"},
	{"lunaut.baseLuaurc", "/base_luaurc"},
	{"lunaut.inlayHints.variableTypes", "/inlay_hints/variable_types"},
	{"lunaut.inlayHints.parameterNames", "/inlay_hints/parameter_names"},
	{"lunaut.inlayHints.parameterTypes", "/inlay_hints/parameter_types"},
	{"lunaut.inlayHints.functionReturnTypes", "/inlay_hints/function_return_types"},
	{"lunaut.inlayHints.hideHintsForErrorTypes", "/inlay_hints/hide_error_types"},
	{"lunaut.inlayHints.hideHintsForMatchingParameterNames", "/inlay_hints/hide_matching_names"},
	{"lunaut.inlayHints.typeHintMaxLength", "/inlay_hints/max_length"},
	{"lunaut.inlayHints.makeInsertable", "/inlay_hints/insertable"},
	{"lunaut.inlayHints.autoRequire", "/inlay_hints/auto_require"},
	{"lunaut.typeDisplay.aliases", "/features/alias_types"},
	{"lunaut.typeDisplay.refine", "/features/refine_types"},
	{"lunaut.typeDisplay.unknownError", "/features/unknown_error"},
	{"lunaut.hover.documentation", "/features/documentation"},
	{"lunaut.completion.autoImports", "/features/auto_imports"},
	{"lunaut.completion.importKeyword", "/features/import_keyword"},
	{"lunaut.completion.imports.requireStyle", "/features/require_style"},
	{"lunaut.completion.imports.aliasPath", "/features/alias_path"},
	{"lunaut.semanticTokens.enabled", "/features/semantic_tokens"},
	{"lunaut.colors.enabled", "/features/colors"},
	{"lunaut.completion.autocompleteEnd", "/features/autocomplete_end"},
	{"lunaut.format.convertQuotes", "/features/convert_quotes"},
	{"lunaut.format.closeInterpolation", "/features/close_interpolation"},
	{"lunaut.format.enabled", "/features/formatting"},
	{"lunaut.format.styluaPath", "/features/stylua"},
	{"lunaut.codeLens.references", "/features/code_lens"},
	{"lunaut.implicitSelf", "/features/implicit_self"},
	{"lunaut.diagnostics.workspace", "/diagnostics/workspace"},
	{"lunaut.analyzer.enabled", "/analyzer/enabled"},
	{"lunaut.analyzer.path", "/analyzer/path"},
	{"lunaut.analyzer.diagnostics", "/analyzer/diagnostics"},
	{"luau-lsp.completion.autocompleteEnd", "/features/autocomplete_end"},
	{"luau-lsp.autocompleteEnd", "/features/autocomplete_end"},
	{"luau-lsp.format.convertQuotes", "/features/convert_quotes"},
	{"luau-lsp.diagnostics.workspace", "/diagnostics/workspace"},
	{"lunaut.documentationFiles", "/documentation_files"},
	{"lunaut.disabledGlobals", "/disabled_globals"},
	{"luau-lsp.platform.type", "/platform"},
	{"luau-lsp.fflags.enableByDefault", "/fflags/enable_by_default"},
	{"luau-lsp.fflags.sync", "/fflags/sync"},
	{"luau-lsp.fflags.enableNewSolver", "/fflags/enable_new_solver"},
	{"luau-lsp.fflags.override", "/fflags/override"},
	{"luau-lsp.types.definitionFiles", "/definitions"},
	{"luau-lsp.types.documentationFiles", "/documentation_files"},
	{"luau-lsp.types.disabledGlobals", "/disabled_globals"},
	{"luau-lsp.types.roblox", "/roblox/download_types"},
	{"luau-lsp.types.robloxSecurityLevel", "/roblox/security_level"},
	{"luau-lsp.sourcemap.enabled", "/rojo/enabled"},
	{"luau-lsp.sourcemap.rojoProjectFile", "/rojo/project_file"},
	{"luau-lsp.sourcemap.sourcemapFile", "/rojo/sourcemap_file"},
	{"luau-lsp.sourcemap.autogenerate", "/rojo/autogenerate"},
	{"luau-lsp.sourcemap.includeNonScripts", "/rojo/include_non_scripts"},
	{"luau-lsp.sourcemap.rojoPath", "/rojo/executable"},
	{"luau-lsp.diagnostics.strictDatamodelTypes", "/rojo/strict_datamodel_types"},
	{"luau-lsp.diagnostics.includeDependents", "/diagnostics/include_dependents"},
	{"luau-lsp.ignoreGlobs", "/ignore_globs"},
	{"luau-lsp.analyzeLuaFiles", "/analyze_lua_files"},
	{"luau-lsp.server.baseLuaurc", "/base_luaurc"},
	{"luau-lsp.inlayHints.variableTypes", "/inlay_hints/variable_types"},
	{"luau-lsp.inlayHints.parameterNames", "/inlay_hints/parameter_names"},
	{"luau-lsp.inlayHints.parameterTypes", "/inlay_hints/parameter_types"},
	{"luau-lsp.inlayHints.functionReturnTypes", "/inlay_hints/function_return_types"},
	{"luau-lsp.inlayHints.hideHintsForErrorTypes", "/inlay_hints/hide_error_types"},
	{"luau-lsp.inlayHints.hideHintsForMatchingParameterNames", "/inlay_hints/hide_matching_names"},
	{"luau-lsp.inlayHints.typeHintMaxLength", "/inlay_hints/max_length"},
	{"luau-lsp.inlayHints.makeInsertable", "/inlay_hints/insertable"},
	{"luau-lsp.completion.imports.enabled", "/features/auto_imports"},
	{"luau-lsp.completion.imports.requireStyle", "/features/require_style"},
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
	priv::read(data, "restart_on_definition_changes", info.restart_on_definition_changes);

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
		priv::read(diagnostics, "workspace", info.diagnostics.workspace);
		priv::read(diagnostics, "delay", info.diagnostics.delay);
	}

	if (data.contains("analyzer") && data["analyzer"].is_object())
	{
		const json& analyzer = data["analyzer"];
		priv::read(analyzer, "path", info.analyzer.path);
		info.analyzer.enabled = !info.analyzer.path.empty();
		priv::read(analyzer, "enabled", info.analyzer.enabled);
		priv::read(analyzer, "diagnostics", info.analyzer.diagnostics);
	}

	if (data.contains("inlay_hints") && data["inlay_hints"].is_object())
	{
		const json& hints = data["inlay_hints"];
		priv::read(hints, "variable_types", info.inlay_hints.variable_types);
		priv::read(hints, "parameter_types", info.inlay_hints.parameter_types);
		priv::read(hints, "function_return_types", info.inlay_hints.function_return_types);
		priv::read(hints, "hide_error_types", info.inlay_hints.hide_error_types);
		priv::read(hints, "hide_matching_names", info.inlay_hints.hide_matching_names);
		priv::read(hints, "max_length", info.inlay_hints.max_length);
		priv::read(hints, "insertable", info.inlay_hints.insertable);
		priv::read(hints, "auto_require", info.inlay_hints.auto_require);

		if (hints.contains("parameter_names") && hints["parameter_names"].is_string())
		{
			info.inlay_hints.parameter_names =
				enum_parameter_hints::from_name(hints["parameter_names"].get<std::string>()).value_or(info.inlay_hints.parameter_names);
		}
	}

	if (data.contains("features") && data["features"].is_object())
	{
		const json& features = data["features"];
		priv::read(features, "documentation", info.features.documentation);
		priv::read(features, "auto_imports", info.features.auto_imports);
		priv::read(features, "semantic_tokens", info.features.semantic_tokens);

		if (features.contains("import_keyword") && features["import_keyword"].is_string())
		{
			info.features.import_keyword =
				enum_import_keyword::from_name(features["import_keyword"].get<std::string>()).value_or(info.features.import_keyword);
		}

		if (features.contains("require_style") && features["require_style"].is_string())
		{
			info.features.require_style =
				enum_require_style::from_name(features["require_style"].get<std::string>()).value_or(info.features.require_style);
		}

		if (features.contains("alias_path") && features["alias_path"].is_string())
		{
			info.features.alias_path =
				enum_alias_path::from_name(features["alias_path"].get<std::string>()).value_or(info.features.alias_path);
		}

		priv::read(features, "colors", info.features.colors);
		priv::read(features, "autocomplete_end", info.features.autocomplete_end);
		priv::read(features, "convert_quotes", info.features.convert_quotes);
		priv::read(features, "close_interpolation", info.features.close_interpolation);
		priv::read(features, "implicit_self", info.features.implicit_self);
		priv::read(features, "formatting", info.features.formatting);
		priv::read(features, "code_lens", info.features.code_lens);
		priv::read(features, "stylua", info.features.stylua);
		priv::read(features, "alias_types", info.features.alias_types);
		priv::read(features, "refine_types", info.features.refine_types);
		priv::read(features, "unknown_error", info.features.unknown_error);
	}

	priv::read(data, "documentation_files", info.documentation_files);
	priv::read(data, "disabled_globals", info.disabled_globals);
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

	return result<json>::ok(convert_vscode_settings(data));
}

json settingsinfo::convert_vscode_settings(const json& data)
{
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

	return converted;
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

	if (root.empty())
	{
		return settings;
	}

	for (const std::string& glob : crater::get_ignore_globs(root))
	{
		if (std::find(settings.ignore_globs.begin(), settings.ignore_globs.end(), glob) == settings.ignore_globs.end())
		{
			settings.ignore_globs.push_back(glob);
		}
	}

	return settings;
}

std::map<std::string, std::string> settingsinfo::get_compatible_keys()
{
	std::map<std::string_view, std::string_view> owners;
	std::map<std::string, std::string> keys;

	for (const auto& [key, pointer] : priv::relevant_vscode_keys)
	{
		if (stdx::starts_with(key, "lunaut."))
		{
			owners[pointer] = key;
		}
	}

	for (const auto& [key, pointer] : priv::relevant_vscode_keys)
	{
		const auto owner = owners.find(pointer);

		if (stdx::starts_with(key, "luau-lsp.") && owner != owners.end())
		{
			keys[std::string(key)] = std::string(owner->second);
		}
	}

	return keys;
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

		if (definition.show_as_module)
		{
			item["show_as_module"] = *definition.show_as_module;
		}

		if (!definition.description.empty())
		{
			item["message"] = definition.description;
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
		{"restart_on_definition_changes", restart_on_definition_changes},
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
		{"diagnostics",
		 {{"lint", diagnostics.lint},
		  {"include_dependents", diagnostics.include_dependents},
		  {"workspace", diagnostics.workspace},
		  {"delay", diagnostics.delay}}},
		{"analyzer", {{"enabled", analyzer.enabled}, {"diagnostics", analyzer.diagnostics}, {"path", analyzer.path}}},
		{"ignore_globs", ignore_globs},
		{"analyze_lua_files", analyze_lua_files},
		{"base_luaurc", base_luaurc},
		{"documentation_files", documentation_files},
		{"disabled_globals", disabled_globals},
		{"inlay_hints",
		 {
			 {"variable_types", inlay_hints.variable_types},
			 {"parameter_names", enum_parameter_hints::get_name(inlay_hints.parameter_names)},
			 {"parameter_types", inlay_hints.parameter_types},
			 {"function_return_types", inlay_hints.function_return_types},
			 {"hide_error_types", inlay_hints.hide_error_types},
			 {"hide_matching_names", inlay_hints.hide_matching_names},
			 {"max_length", inlay_hints.max_length},
			 {"insertable", inlay_hints.insertable},
			 {"auto_require", inlay_hints.auto_require},
		 }},
		{"features",
		 {
			 {"documentation", features.documentation},
			 {"auto_imports", features.auto_imports},
			 {"import_keyword", enum_import_keyword::get_name(features.import_keyword)},
			 {"require_style", enum_require_style::get_name(features.require_style)},
			 {"alias_path", enum_alias_path::get_name(features.alias_path)},
			 {"semantic_tokens", features.semantic_tokens},
			 {"colors", features.colors},
			 {"autocomplete_end", features.autocomplete_end},
			 {"convert_quotes", features.convert_quotes},
			 {"close_interpolation", features.close_interpolation},
			 {"implicit_self", features.implicit_self},
			 {"formatting", features.formatting},
			 {"code_lens", features.code_lens},
			 {"stylua", features.stylua},
			 {"alias_types", features.alias_types},
			 {"refine_types", features.refine_types},
			 {"unknown_error", features.unknown_error},
		 }},
	};
}

json loadeddefinitioninfo::to_json() const
{
	return {
		{"name", name},
		{"path", path},
		{"priority", priority},
		{"success", success},
		{"show_as_module", show_as_module},
		{"message", message},
		{"description", description},
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
	if (data.is_string())
	{
		target.clear();
		target.push_back(definitioninfo{data.get<std::string>(), "", std::nullopt, std::nullopt, enumitem_definition_source::Config});
		return;
	}

	if (data.is_object())
	{
		target.clear();
		for (const auto& [name, path] : data.items())
		{
			if (path.is_string())
			{
				target.push_back(
					definitioninfo{path.get<std::string>(), name, std::nullopt, std::nullopt, enumitem_definition_source::Config});
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
			target.push_back(definitioninfo{item.get<std::string>(), "", std::nullopt, std::nullopt, enumitem_definition_source::Config});
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

		if (item.contains("show_as_module") && item["show_as_module"].is_boolean())
		{
			info.show_as_module = item["show_as_module"].get<bool>();
		}

		if (item.contains("message") && item["message"].is_string())
		{
			info.description = item["message"].get<std::string>();
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

		for (std::string_view prefix : vscode_prefixes)
		{
			if (stdx::starts_with(next, prefix) || stdx::starts_with(std::string(prefix), next + "."))
			{
				flatten(child, next, target);
				break;
			}
		}
	}
}

}
}

}
