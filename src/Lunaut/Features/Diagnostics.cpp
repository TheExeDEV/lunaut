#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

namespace lunaut
{

json workspace::get_diagnostics(const std::string& address)
{
	json diagnostics = json::array();
	Luau::ModuleName name = resolver.get_module_name(address);
	std::optional<textdocument> document = get_text_for(name);

	if (!document)
	{
		return diagnostics;
	}

	Luau::CheckResult checked = check_module(name, false);

	for (const Luau::TypeError& error : checked.errors)
	{
		if (error.moduleName == name)
		{
			diagnostics.push_back(protocol::make_type_error(*document, error, &resolver));
		}
	}

	for (const Luau::LintWarning& warning : checked.lintResult.errors)
	{
		diagnostics.push_back(protocol::make_lint_warning(*document, warning, true));
	}

	for (const Luau::LintWarning& warning : checked.lintResult.warnings)
	{
		diagnostics.push_back(protocol::make_lint_warning(*document, warning, false));
	}

	return diagnostics;
}

void workspace::publish_diagnostics(const std::string& address)
{
	std::string path = uri::to_path(address);

	if (is_definition_file(path))
	{
		return;
	}

	if (is_ignored(path) || (!settings.analyze_lua_files && stdx::ends_with(path, ".lua")))
	{
		owner.send_diagnostics(address, std::nullopt, json::array());
		return;
	}

	const textdocument* document = resolver.get_document(address);
	std::optional<int> version = document != nullptr ? std::optional<int>(document->get_version()) : std::nullopt;

	owner.send_diagnostics(address, version, get_diagnostics(address));
}

}
