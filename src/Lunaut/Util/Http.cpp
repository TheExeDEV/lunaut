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
