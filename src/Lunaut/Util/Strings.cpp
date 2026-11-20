#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>

#if LUNAUT_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace lunaut::stdx
{

void oute(const std::string& message)
{
	std::cerr << "[" LUNAUT_NAME "] error: " << message << '\n';
}

void outi(const std::string& message)
{
#if CORE_LOG_TO_STDERR
	std::cerr << "[" LUNAUT_NAME "] " << message << '\n';
#endif
}

bool check(bool condition, const std::string& message)
{
	if (!condition)
	{
		oute(message);
	}

	return condition;
}

std::optional<std::string> read_file(const std::filesystem::path& path)
{
	std::ifstream stream(path, std::ios::in | std::ios::binary);
	if (!stream)
	{
		return std::nullopt;
	}

	std::ostringstream buffer;
	buffer << stream.rdbuf();
	std::string contents = buffer.str();

	if (starts_with(contents, "\xEF\xBB\xBF"))
	{
		contents.erase(0, 3);
	}

	return contents;
}

bool write_file(const std::filesystem::path& path, const std::string& contents)
{
	std::error_code error;
	if (path.has_parent_path())
	{
		std::filesystem::create_directories(path.parent_path(), error);
	}

	std::ofstream stream(path, std::ios::out | std::ios::binary | std::ios::trunc);
	if (!stream)
	{
		return false;
	}

	stream << contents;
	return static_cast<bool>(stream);
}

bool set_hidden(const std::filesystem::path& path, bool hidden)
{
#if LUNAUT_PLATFORM_WINDOWS
	DWORD attributes = GetFileAttributesW(path.wstring().c_str());

	if (attributes == INVALID_FILE_ATTRIBUTES)
	{
		return false;
	}

	attributes = hidden ? (attributes | FILE_ATTRIBUTE_HIDDEN) : (attributes & ~DWORD(FILE_ATTRIBUTE_HIDDEN));
	return SetFileAttributesW(path.wstring().c_str(), attributes) != 0;
#else
	return true;
#endif
}

bool starts_with(std::string_view value, std::string_view prefix)
{
	return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view value, std::string_view suffix)
{
	return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string to_lower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

std::string trim(std::string_view value)
{
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string_view::npos)
	{
		return {};
	}

	const auto last = value.find_last_not_of(" \t\r\n");
	return std::string(value.substr(first, last - first + 1));
}

std::vector<std::string> split(std::string_view value, char separator)
{
	std::vector<std::string> parts;
	size_t start = 0;

	while (start <= value.size())
	{
		const auto end = value.find(separator, start);
		if (end == std::string_view::npos)
		{
			parts.emplace_back(value.substr(start));
			break;
		}

		parts.emplace_back(value.substr(start, end - start));
		start = end + 1;
	}

	return parts;
}

}
