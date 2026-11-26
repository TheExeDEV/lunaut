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

constexpr std::string_view unknown_directive = "Unknown comment directive '";
constexpr std::string_view unknown_ignore = "Unknown --!ignore value; use strict, nonstrict, nocheck or nolint";

bool is_own_directive(const Luau::LintWarning& warning);

}
}

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
	const Luau::SourceModule* source = frontend.getSourceModule(name);

	if (source != nullptr && filedirectives::is_silenced(*source))
	{
		return diagnostics;
	}

	std::set<std::pair<unsigned int, unsigned int>> reported;

	for (const Luau::TypeError& error : checked.errors)
	{
		if (error.moduleName == name)
		{
			diagnostics.push_back(protocol::make_type_error(*document, error, &resolver));

			if (Luau::get_if<Luau::UnknownRequire>(&error.data) != nullptr)
			{
				reported.insert({error.location.begin.line, error.location.begin.column});
			}
		}
	}

	const auto node = frontend.sourceNodes.find(name);

	if (node != frontend.sourceNodes.end() && node->second)
	{
		for (const auto& [required, location] : node->second->requireLocations)
		{
			if (sourcemap::is_virtual_path(required) || reported.count({location.begin.line, location.begin.column}) != 0 ||
				resolver.readSource(required))
			{
				continue;
			}

			Luau::TypeError missing{location, name, Luau::UnknownRequire{resolver.getHumanReadableModuleName(required)}};
			diagnostics.push_back(protocol::make_type_error(*document, missing, &resolver));
		}
	}

	for (const Luau::LintWarning& warning : checked.lintResult.errors)
	{
		diagnostics.push_back(protocol::make_lint_warning(*document, warning, true));
	}

	for (const Luau::LintWarning& warning : checked.lintResult.warnings)
	{
		if (priv::is_own_directive(warning))
		{
			continue;
		}

		diagnostics.push_back(protocol::make_lint_warning(*document, warning, false));
	}

	if (source != nullptr)
	{
		for (const Luau::HotComment& comment : filedirectives::get_unknown_ignores(*source))
		{
			Luau::LintWarning warning{Luau::LintWarning::Code_CommentDirective, comment.location, std::string(priv::unknown_ignore)};
			diagnostics.push_back(protocol::make_lint_warning(*document, warning, false));
		}
	}

	if (settings.analyzer.enabled && settings.analyzer.diagnostics && analyzer.is_loaded())
	{
		std::optional<json> external = analyzer.check(name, uri::to_path(address), document->get_text(), settings.to_json());

		if (external)
		{
			for (const json& diagnostic : *external)
			{
				if (diagnostic.is_object())
				{
					diagnostics.push_back(diagnostic);
				}
			}
		}
		else if (!analyzer_reported)
		{
			analyzer_reported = true;
			owner.send_log(enumitem_message_type::Warning, "analyzer " + analyzer.get_name() + " returned an invalid diagnostics payload");
		}
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

void workspace::publish_workspace_diagnostics()
{
	if (!settings.diagnostics.workspace)
	{
		return;
	}

	index_workspace();

	std::vector<Luau::ModuleName> modules;

	for (const auto& [module_name, node] : frontend.sourceNodes)
	{
		modules.push_back(module_name);
	}

	for (const Luau::ModuleName& module_name : modules)
	{
		std::optional<std::string> path = resolver.get_real_path(module_name);

		if (!path || is_definition_file(*path) || stdx::ends_with(*path, ".d.luau"))
		{
			continue;
		}

		std::error_code error;

		if (std::filesystem::is_regular_file(*path, error))
		{
			publish_diagnostics(uri::from_path(*path));
		}
	}
}

namespace
{
namespace priv
{

bool is_own_directive(const Luau::LintWarning& warning)
{
	if (warning.code != Luau::LintWarning::Code_CommentDirective || !stdx::starts_with(warning.text, unknown_directive))
	{
		return false;
	}

	std::string rest = warning.text.substr(unknown_directive.size());
	size_t end = rest.find('\'');

	return end != std::string::npos && filedirectives::is_own_directive(rest.substr(0, end));
}

}
}

}
