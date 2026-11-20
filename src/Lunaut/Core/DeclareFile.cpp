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

constexpr std::string_view file_name = ".lunaut-declare";
constexpr int api_version = 1;
constexpr std::string_view settings_schema =
	"https://raw.githubusercontent.com/TheExeDEV/lunaut/v" LUNAUT_VERSION "/editors/vscode/package.json";

const std::vector<std::pair<std::string_view, int>> relevant_features = {
	{"analyzer", 1},
	{"base_luaurc", 1},
	{"crater_packages", 1},
	{"definition_priority", 1},
	{"definitions", 1},
	{"fflags", 1},
	{"rojo", 1},
	{"settings", 1},
	{"show_as_module", 1},
};

const std::vector<std::pair<std::string_view, int>> relevant_json_output = {
	{"declare", 1},
	{"flags", 1},
	{"stats", 1},
};

const std::vector<std::string_view> relevant_versioned_sections = {"/features", "/cli/json_output"};

json make_definitions();
json make_versions(const std::vector<std::pair<std::string_view, int>>& versions);
int add_missing(json& target, const json& expected);
int raise_versions(json& target, const json& expected);
result<nothing> write(const std::filesystem::path& path, const json& content);

}
}

json declarefile::make_content()
{
	json compatible = json::object();

	for (const auto& [from, to] : settingsinfo::get_compatible_keys())
	{
		compatible[from] = to;
	}

	return {
		{"api_version", priv::api_version},
		{"lunaut_version", LUNAUT_VERSION},
		{"features", priv::make_versions(priv::relevant_features)},
		{"settings",
		 {
			 {"file", ".vscode/settings.json"},
			 {"prefix", "lunaut."},
			 {"compatible_prefix", "luau-lsp."},
			 {"compatible", compatible},
			 {"precedence", "lunaut"},
		 }},
		{"definitions", priv::make_definitions()},
		{"schemas", {{"settings", priv::settings_schema}}},
		{"cli", {{"json_output", priv::make_versions(priv::relevant_json_output)}}},
	};
}

std::filesystem::path declarefile::get_path(const std::filesystem::path& root)
{
	return root / std::string(priv::file_name);
}

result<std::string> declarefile::make_file(const std::filesystem::path& root)
{
	std::filesystem::path path = get_path(root);
	std::error_code error;

	if (std::filesystem::exists(path, error))
	{
		result<bool> applied = apply_missing(root);

		if (applied)
		{
			return result<std::string>::ok(path.generic_string() + (applied.value ? ": added the missing keys" : " is up to date"));
		}

		stdx::oute(applied.message);
	}

	result<nothing> written = priv::write(path, make_content());

	if (!written)
	{
		return result<std::string>::fail(written.message);
	}

	return result<std::string>::ok("wrote " + path.generic_string());
}

result<bool> declarefile::apply_missing(const std::filesystem::path& root)
{
	std::filesystem::path path = get_path(root);
	std::error_code error;

	if (!std::filesystem::exists(path, error))
	{
		return result<bool>::ok(false);
	}

	std::optional<std::string> text = stdx::read_file(path);

	if (!text)
	{
		return result<bool>::fail("cannot read " + path.generic_string());
	}

	json declared = json::parse(*text, nullptr, false);

	if (declared.is_discarded() || !declared.is_object())
	{
		return result<bool>::fail(path.generic_string() + " is not a JSON object");
	}

	json expected = make_content();
	int changed = priv::add_missing(declared, expected) + priv::raise_versions(declared, expected);

	if (changed == 0)
	{
		return result<bool>::ok(false);
	}

	declared["lunaut_version"] = LUNAUT_VERSION;
	result<nothing> written = priv::write(path, declared);

	if (!written)
	{
		return result<bool>::fail(written.message);
	}

	return result<bool>::ok(true);
}

namespace
{
namespace priv
{

json make_definitions()
{
	return {
		{"setting", "lunaut.definitions"},
		{"compatible_setting", "luau-lsp.types.definitionFiles"},
		{"merge_with_compatible", false},
		{"forms",
		 {
			 {"array", "entries, each a path string or an object with the entry fields"},
			 {"string", "one path"},
			 {"object", "a map of name to path"},
		 }},
		{"entry",
		 {
			 {"path", {{"type", "string"}, {"required", true}, {"globs", true}, {"relative_to", "workspace root"}}},
			 {"name", {{"type", "string"}, {"default", ""}, {"globs", "ignored for wildcard paths"}}},
			 {"priority", {{"type", "integer"}, {"default", 0}, {"order", "higher loads first"}}},
			 {"show_as_module", {{"type", "boolean"}, {"default", false}}},
		 }},
		{"default_name", "@ + file name without .d.luau, .d.lua, .luau or .lua"},
		{"name_prefix", "@"},
		{"priority_sources", json::array({"settings", "directive", "default"})},
		{"priority_directive", {{"text", "--!priority"}, {"lines", 32}}},
		{"duplicates", "first"},
		{"show_as_module_name", {{"strip", "first @"}, {"pattern", "^[A-Za-z_][A-Za-z0-9_]*$"}}},
		{"reserved_names", json::array({"@roblox"})},
		{"roblox_priority_setting", "lunaut.roblox.typesPriority"},
	};
}

json make_versions(const std::vector<std::pair<std::string_view, int>>& versions)
{
	json table = json::object();

	for (const auto& [name, version] : versions)
	{
		table[std::string(name)] = version;
	}

	return table;
}

int add_missing(json& target, const json& expected)
{
	int added = 0;

	for (const auto& [key, value] : expected.items())
	{
		if (!target.contains(key))
		{
			target[key] = value;
			++added;
			continue;
		}

		if (value.is_object() && target[key].is_object())
		{
			added += add_missing(target[key], value);
		}
	}

	return added;
}

int raise_versions(json& target, const json& expected)
{
	int raised = 0;

	if (target["api_version"].is_number_integer() && target["api_version"].get<int>() < expected["api_version"].get<int>())
	{
		target["api_version"] = expected["api_version"];
		++raised;
	}

	for (std::string_view section : relevant_versioned_sections)
	{
		json::json_pointer pointer{std::string(section)};

		if (!target.contains(pointer) || !target[pointer].is_object())
		{
			continue;
		}

		for (const auto& [name, version] : expected[pointer].items())
		{
			json& current = target[pointer][name];

			if (current.is_number_integer() && current.get<int>() < version.get<int>())
			{
				current = version;
				++raised;
			}
		}
	}

	return raised;
}

result<nothing> write(const std::filesystem::path& path, const json& content)
{
	std::error_code error;

	if (std::filesystem::exists(path, error))
	{
		stdx::set_hidden(path, false);
	}

	if (!stdx::write_file(path, content.dump(2) + "\n"))
	{
		return result<nothing>::fail("cannot write " + path.generic_string());
	}

	stdx::set_hidden(path, true);
	return result<nothing>::ok({});
}

}
}

}
