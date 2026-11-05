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

constexpr std::string_view downloader = "curl -fsSL --max-time 30 --user-agent " LUNAUT_NAME "/" LUNAUT_VERSION " ";

}
}

result<std::string> http::get_text(const std::string& url)
{
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
