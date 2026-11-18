#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include <iostream>

namespace lunaut
{
namespace
{
namespace priv
{

const std::map<int, std::string_view> relevant_severities = {
	{static_cast<int>(enumitem_diagnostic_severity::Error), "error"},
	{static_cast<int>(enumitem_diagnostic_severity::Warning), "warning"},
	{static_cast<int>(enumitem_diagnostic_severity::Information), "info"},
	{static_cast<int>(enumitem_diagnostic_severity::Hint), "hint"},
};

std::vector<std::filesystem::path> expand_inputs(const std::vector<std::string>& inputs, const settingsinfo& settings);
settingsinfo load_settings(const std::filesystem::path& root, const optionsinfo& options);

}
}

console::console(bool verbose, bool json_output)
	: verbose(verbose)
	, json_output(json_output)
{
}

int console::invoke_analyze(const std::vector<std::string>& files, const optionsinfo& options)
{
	std::filesystem::path root = std::filesystem::current_path();
	settingsinfo settings = priv::load_settings(root, options);

	fflagregistry::apply(settings.fflags, options.fflags, options.no_flags_enabled, *this);

	workspace folder(*this, root, settings);
	folder.initialize();

	std::vector<std::filesystem::path> inputs = priv::expand_inputs(files, settings);

	for (const std::filesystem::path& file : inputs)
	{
		std::optional<std::string> text = stdx::read_file(file);

		if (!text)
		{
			std::cerr << file.generic_string() << ": cannot read file\n";
			++errors;
			continue;
		}

		folder.open_document(uri::from_path(std::filesystem::absolute(file).generic_string()), *text, 0);
	}

	std::cout << inputs.size() << " files, " << errors << " errors, " << warnings << " warnings\n";
	return errors > 0 ? 1 : 0;
}

int console::invoke_stats(const std::vector<std::string>& files, const optionsinfo& options)
{
	std::filesystem::path root = std::filesystem::current_path();
	settingsinfo settings = priv::load_settings(root, options);

	fflagregistry::apply(settings.fflags, options.fflags, options.no_flags_enabled, *this);

	workspace folder(*this, root, settings);
	folder.initialize();

	std::vector<std::filesystem::path> inputs = priv::expand_inputs(files, settings);
	diagnostics_output = false;

	for (const std::filesystem::path& file : inputs)
	{
		std::optional<std::string> text = stdx::read_file(file);

		if (!text)
		{
			std::cerr << file.generic_string() << ": cannot read file\n";
			diagnostics_output = true;
			return 1;
		}

		folder.open_document(uri::from_path(std::filesystem::absolute(file).generic_string()), *text, 0);
	}

	diagnostics_output = true;

	json stats;

	try
	{
		stats = folder.get_type_stats();
	}
	catch (const std::exception& error)
	{
		std::cerr << "type statistics failed: " << error.what() << '\n';
		return 1;
	}
	catch (...)
	{
		std::cerr << "type statistics failed\n";
		return 1;
	}

	if (json_output)
	{
		std::cout << stats.dump(2) << '\n';
		return 0;
	}

	size_t definition_count = stats.value("definitions", size_t(0));

	std::cout << "Types: " << stats.value("total", size_t(0)) << " total, " << stats.value("modules", size_t(0)) << " modules, "
			  << definition_count << (definition_count == 1 ? " definition file" : " definition files") << '\n';

	const json& types = stats["types"];
	std::cout << "  in modules: " << types.value("modules", 0) << ", in globals: " << types.value("globals", 0) << '\n';

	const json& kinds = types["kinds"];

	if (!kinds.empty())
	{
		std::cout << "  by kind: ";

		bool comma = false;

		for (const auto& [kind, count] : kinds.items())
		{
			std::cout << (comma ? ", " : "") << kind << " " << count;
			comma = true;
		}

		std::cout << '\n';
	}

	std::cout << "Origins:\n";

	for (const json& origin : stats["origins"])
	{
		std::cout << "  " << origin.value("name", "") << " (" << origin.value("kind", "module") << ")";

		if (origin.contains("types"))
		{
			std::cout << " " << origin.value("types", 0) << " types";
		}
		else
		{
			std::cout << " " << origin.value("internal", 0) << " internal, " << origin.value("interface", 0) << " interface";
		}

		if (origin.contains("path"))
		{
			std::cout << " " << origin.value("path", "");
		}

		std::cout << '\n';
	}

	if (!stats["hierarchy"].empty())
	{
		std::cout << "Hierarchy:\n";

		for (const json& relation : stats["hierarchy"])
		{
			std::cout << "  " << relation.value("name", "") << " extends " << relation.value("parent", "") << '\n';
		}
	}

	if (!stats["aliases"].empty())
	{
		std::cout << "Aliases:\n";

		for (const json& alias : stats["aliases"])
		{
			std::cout << "  " << alias.value("module", "") << "." << alias.value("name", "") << ": " << alias.value("kind", "") << '\n';
		}
	}

	return 0;
}

void console::send_log(enumitem_message_type type, const std::string& message)
{
	if (verbose || type == enumitem_message_type::Error || type == enumitem_message_type::Warning)
	{
		std::cerr << "[" << enum_message_type::get_name(type) << "] " << message << '\n';
	}
}

void console::send_window_message(enumitem_message_type type, const std::string& message)
{
	std::cerr << "[" << enum_message_type::get_name(type) << "] " << message << '\n';
}

void console::send_diagnostics(const std::string& address, std::optional<int> version, const json& diagnostics)
{
	if (!diagnostics_output)
	{
		return;
	}

	std::string path = uri::get_relative(std::filesystem::current_path(), uri::to_path(address));

	for (const json& diagnostic : diagnostics)
	{
		int severity = diagnostic.value("severity", 1);
		const auto name = priv::relevant_severities.find(severity);
		const json& start = diagnostic["range"]["start"];

		if (severity == static_cast<int>(enumitem_diagnostic_severity::Error))
		{
			++errors;
		}
		else
		{
			++warnings;
		}

		std::cout << path << ":" << start.value("line", 0) + 1 << ":" << start.value("character", 0) + 1 << ": "
				  << (name == priv::relevant_severities.end() ? "error" : name->second) << " [" << diagnostic.value("code", "") << "] "
				  << diagnostic.value("message", "") << '\n';
	}
}

namespace
{
namespace priv
{

std::vector<std::filesystem::path> expand_inputs(const std::vector<std::string>& inputs, const settingsinfo& settings)
{
	std::vector<std::filesystem::path> files;

	for (const std::string& input : inputs)
	{
		std::error_code error;

		if (std::filesystem::is_directory(input, error))
		{
			for (std::string_view pattern : {"**/*.luau", "**/*.lua"})
			{
				for (const std::filesystem::path& file : stdx::find_files(input, pattern, settings.ignore_globs))
				{
					if (!stdx::ends_with(file.generic_string(), ".d.luau"))
					{
						files.push_back(file);
					}
				}
			}

			continue;
		}

		files.emplace_back(input);
	}

	return files;
}

settingsinfo load_settings(const std::filesystem::path& root, const optionsinfo& options)
{
	settingsinfo settings = settingsinfo{}.with_project(root, options.settings_file);
	settings.definitions.insert(settings.definitions.end(), options.definitions.begin(), options.definitions.end());
	return settings;
}

}
}

}
