#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "analyzer.h"

#include <cstring>

#if LUNAUT_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace lunaut
{
namespace
{
namespace priv
{

void* open_library(const std::string& path, std::string& error)
{
#if LUNAUT_PLATFORM_WINDOWS
	HMODULE handle = LoadLibraryA(path.c_str());

	if (handle == nullptr)
	{
		error = "cannot load analyzer library: " + path;
	}

	return reinterpret_cast<void*>(handle);
#else
	void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);

	if (handle == nullptr)
	{
		const char* message = dlerror();
		error = std::string("cannot load analyzer library: ") + (message != nullptr ? message : path);
	}

	return handle;
#endif
}

void close_library(void* handle)
{
	if (handle == nullptr)
	{
		return;
	}

#if LUNAUT_PLATFORM_WINDOWS
	FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
	dlclose(handle);
#endif
}

void* find_symbol(void* handle, const char* name)
{
#if LUNAUT_PLATFORM_WINDOWS
	return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
	return dlsym(handle, name);
#endif
}

std::string frontend_api_name(uint32_t api)
{
	switch (api)
	{
	case LUNAUT_ANALYZER_FRONTEND_PRE_0743:
		return "pre-0.743";
	case LUNAUT_ANALYZER_FRONTEND_POST_0743:
		return "post-0.743";
	default:
		return "unknown";
	}
}

result<nothing> validate_info(const lunaut_analyzer_info& info)
{
	if (info.size < sizeof(lunaut_analyzer_info))
	{
		return result<nothing>::fail("incompatible analyzer: info structure is too small");
	}

	if (info.abi != LUNAUT_ANALYZER_ABI_VERSION)
	{
		return result<nothing>::fail(
			"incompatible analyzer ABI " + std::to_string(info.abi) + ", lunaut expects " + std::to_string(LUNAUT_ANALYZER_ABI_VERSION));
	}

	if (info.frontend_api != LUNAUT_ANALYZER_FRONTEND_PRE_0743 && info.frontend_api != LUNAUT_ANALYZER_FRONTEND_POST_0743)
	{
		return result<nothing>::fail("incompatible analyzer frontend API " + std::to_string(info.frontend_api));
	}

	if (info.name == nullptr || info.name[0] == '\0')
	{
		return result<nothing>::fail("incompatible analyzer: empty name");
	}

	if (info.luau_version_code == 0)
	{
		return result<nothing>::fail("incompatible analyzer: the library does not report its Luau version");
	}

	uint32_t major = info.luau_version_code / 100000;
	uint32_t minor = (info.luau_version_code / 100) % 1000;

	if (major != 0 || minor < 700)
	{
		std::string reported = info.luau_version != nullptr ? info.luau_version : std::to_string(major) + "." + std::to_string(minor);
		return result<nothing>::fail("incompatible analyzer version " + reported + ", lunaut supports Luau 0.700 and newer");
	}

	if (info.frontend_api == LUNAUT_ANALYZER_FRONTEND_PRE_0743 && minor >= 743)
	{
		std::string reported = info.luau_version != nullptr ? info.luau_version : std::to_string(major) + "." + std::to_string(minor);
		return result<nothing>::fail(
			"incompatible analyzer version " + reported + ": pre-0.743 frontend API cannot serve Luau 0.743 and newer");
	}

	if (info.frontend_api == LUNAUT_ANALYZER_FRONTEND_POST_0743 && minor < 743)
	{
		std::string reported = info.luau_version != nullptr ? info.luau_version : std::to_string(major) + "." + std::to_string(minor);
		return result<nothing>::fail("incompatible analyzer version " + reported + ": post-0.743 frontend API needs Luau 0.743 or newer");
	}

	return result<nothing>::ok({});
}

}
}

analyzerplugin::~analyzerplugin()
{
	unload();
}

bool analyzerplugin::load(const std::string& path)
{
	unload();
	error.clear();

	if (path.empty())
	{
		error = "analyzer path is empty";
		return false;
	}

	handle = priv::open_library(path, error);

	if (handle == nullptr)
	{
		return false;
	}

	get_info = reinterpret_cast<info_function>(priv::find_symbol(handle, "lunaut_analyzer_info_v1"));
	run_check = reinterpret_cast<check_function>(priv::find_symbol(handle, "lunaut_analyzer_check_v1"));
	release = reinterpret_cast<free_function>(priv::find_symbol(handle, "lunaut_analyzer_free_v1"));

	if (get_info == nullptr || run_check == nullptr)
	{
		error = "incompatible analyzer: missing exported symbols (lunaut_analyzer_info_v1, lunaut_analyzer_check_v1)";
		unload();
		return false;
	}

	const lunaut_analyzer_info* info = static_cast<const lunaut_analyzer_info*>(get_info());

	if (info == nullptr)
	{
		error = "incompatible analyzer: lunaut_analyzer_info_v1 returned null";
		unload();
		return false;
	}

	result<nothing> compatible = priv::validate_info(*info);

	if (!compatible)
	{
		error = compatible.message;
		unload();
		return false;
	}

	name = info->name;
	version = info->luau_version != nullptr ? info->luau_version : "unknown";
	frontend_api = priv::frontend_api_name(info->frontend_api);
	enabled = true;
	loaded = true;
	return true;
}

void analyzerplugin::unload()
{
	if (handle != nullptr)
	{
		priv::close_library(handle);
	}

	handle = nullptr;
	get_info = nullptr;
	run_check = nullptr;
	release = nullptr;
	enabled = false;
	loaded = false;
	name.clear();
	version.clear();
	frontend_api = "unknown";
}

bool analyzerplugin::is_loaded() const
{
	return loaded;
}

const std::string& analyzerplugin::get_name() const
{
	return name;
}

const std::string& analyzerplugin::get_version() const
{
	return version;
}

const std::string& analyzerplugin::get_error() const
{
	return error;
}

const std::string& analyzerplugin::get_frontend_api() const
{
	return frontend_api;
}

json analyzerplugin::to_json() const
{
	return {
		{"enabled", enabled},
		{"loaded", loaded},
		{"name", name},
		{"version", version},
		{"frontend_api", frontend_api},
		{"error", error},
	};
}

std::optional<json>
analyzerplugin::check(const std::string& module, const std::string& path, const std::string& source, const json& settings) const
{
	if (!loaded || run_check == nullptr)
	{
		return std::nullopt;
	}

	std::string dumped = settings.dump();
	lunaut_analyzer_request request;
	request.size = sizeof(lunaut_analyzer_request);
	request.module = module.c_str();
	request.path = path.c_str();
	request.source = source.c_str();
	request.settings = dumped.c_str();

	lunaut_analyzer_response response;
	response.size = sizeof(lunaut_analyzer_response);
	response.diagnostics = nullptr;
	response.error = nullptr;

	if (run_check(&request, &response) != 0)
	{
		return std::nullopt;
	}

	if (response.diagnostics == nullptr)
	{
		return std::nullopt;
	}

	std::optional<json> result;

	try
	{
		json parsed = json::parse(response.diagnostics);
		result = parsed.is_array() ? parsed : json::array();
	}
	catch (const std::exception&)
	{
		result = std::nullopt;
	}

	if (release != nullptr)
	{
		release(response.diagnostics);
	}

	return result;
}

}
