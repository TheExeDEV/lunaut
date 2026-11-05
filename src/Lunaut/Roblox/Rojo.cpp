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

std::string quote(const std::string& value);

}
}

bool rojo::is_project(const std::filesystem::path& root, const rojoinfo& info)
{
	std::error_code error;
	return std::filesystem::exists(root / info.project_file, error) || std::filesystem::exists(root / info.sourcemap_file, error);
}

result<nothing> rojo::generate_sourcemap(const std::filesystem::path& root, const rojoinfo& info)
{
	std::error_code error;

	if (!std::filesystem::exists(root / info.project_file, error))
	{
		return result<nothing>::fail("rojo project not found: " + (root / info.project_file).generic_string());
	}

	if (!process::has_executable(info.executable))
	{
		return result<nothing>::fail("rojo executable not found: " + info.executable);
	}

	std::string command = priv::quote(info.executable) + " sourcemap " + priv::quote((root / info.project_file).generic_string()) +
		" --output " + priv::quote((root / info.sourcemap_file).generic_string());

	if (info.include_non_scripts)
	{
		command += " --include-non-scripts";
	}

	std::filesystem::path previous = std::filesystem::current_path(error);
	std::filesystem::current_path(root, error);
	result<std::string> output = process::invoke_command(command);
	std::filesystem::current_path(previous, error);

	if (!output)
	{
		return result<nothing>::fail(output.message + ": " + stdx::trim(output.value));
	}

	return result<nothing>::ok({});
}

namespace
{
namespace priv
{

std::string quote(const std::string& value)
{
	if (value.find_first_of(" \t") == std::string::npos)
	{
		return value;
	}

	return "\"" + value + "\"";
}

}
}

}
