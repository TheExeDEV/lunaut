#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include <chrono>
#include <cstdlib>

namespace lunaut
{
namespace
{
namespace priv
{

std::filesystem::path get_base_directory();
bool is_fresh(const std::filesystem::path& file);

}
}

std::filesystem::path cache::get_directory()
{
	std::filesystem::path directory = priv::get_base_directory() / LUNAUT_NAME / "cache";
	std::error_code error;
	std::filesystem::create_directories(directory, error);
	return directory;
}

result<std::string> cache::get_or_fetch(const std::string& name, const std::string& url, bool refresh)
{
	std::filesystem::path file = get_directory() / name;

	if (!refresh && priv::is_fresh(file))
	{
		if (std::optional<std::string> cached = stdx::read_file(file))
		{
			return result<std::string>::ok(*cached);
		}
	}

	result<std::string> downloaded = http::get_text(url);

	if (downloaded)
	{
		stdx::write_file(file, downloaded.value);
		return downloaded;
	}

	if (std::optional<std::string> stale = stdx::read_file(file))
	{
		stdx::outi("using stale cache for " + name + ": " + downloaded.message);
		return result<std::string>::ok(*stale);
	}

	return downloaded;
}

std::optional<std::filesystem::path> cache::get_file(const std::string& name)
{
	std::filesystem::path file = get_directory() / name;
	std::error_code error;

	if (std::filesystem::exists(file, error))
	{
		return file;
	}

	return std::nullopt;
}

namespace
{
namespace priv
{

std::filesystem::path get_base_directory()
{
#if LUNAUT_PLATFORM_WINDOWS
	if (const char* local = std::getenv("LOCALAPPDATA"))
	{
		return local;
	}
#else
	if (const char* xdg = std::getenv("XDG_CACHE_HOME"))
	{
		return xdg;
	}

	if (const char* home = std::getenv("HOME"))
	{
		return std::filesystem::path(home) / ".cache";
	}
#endif

	return std::filesystem::temp_directory_path();
}

bool is_fresh(const std::filesystem::path& file)
{
	std::error_code error;
	std::filesystem::file_time_type written = std::filesystem::last_write_time(file, error);

	if (error)
	{
		return false;
	}

	std::chrono::hours age = std::chrono::duration_cast<std::chrono::hours>(std::filesystem::file_time_type::clock::now() - written);
	return age.count() < CORE_CACHE_MAX_AGE_HOURS;
}

}
}

}
