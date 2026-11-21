#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include <cstdlib>

namespace lunaut
{
namespace
{
namespace priv
{

constexpr std::string_view downloader = "curl -fsSL --max-time 30 --user-agent " LUNAUT_NAME "/" LUNAUT_VERSION " ";

}
}

result<nothing> http::download_file(const std::string& url, const std::filesystem::path& target)
{
	if (std::getenv("LUNAUT_OFFLINE") != nullptr)
	{
		return result<nothing>::fail("offline mode (LUNAUT_OFFLINE)");
	}

	std::string path = target.generic_string();

	if (url.find_first_of("\"'`$ ") != std::string::npos || path.find_first_of("\"'`$") != std::string::npos)
	{
		return result<nothing>::fail("refusing unsafe url or path: " + url);
	}

	result<std::string> response =
		process::invoke_command(std::string(priv::downloader) + "--location --output \"" + path + ".download\" \"" + url + "\"");

	if (!response)
	{
		std::error_code error;
		std::filesystem::remove(path + ".download", error);
		return result<nothing>::fail("download failed (" + url + "): " + stdx::trim(response.value));
	}

	std::error_code error;
	std::filesystem::rename(path + ".download", target, error);

	if (error)
	{
		return result<nothing>::fail("cannot write " + path + ": " + error.message());
	}

	return result<nothing>::ok({});
}

result<std::string> http::get_text(const std::string& url)
{
	if (std::getenv("LUNAUT_OFFLINE") != nullptr)
	{
		return result<std::string>::fail("offline mode (LUNAUT_OFFLINE)");
	}

	if (url.find_first_of("\"'`$ ") != std::string::npos)
	{
		return result<std::string>::fail("refusing unsafe url: " + url);
	}

	result<std::string> response = process::invoke_command(std::string(priv::downloader) + "\"" + url + "\"");

	if (!response)
	{
		return result<std::string>::fail("download failed (" + url + "): " + stdx::trim(response.value));
	}

	return response;
}

}
