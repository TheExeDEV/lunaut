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

console::console(bool verbose)
	: verbose(verbose)
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
