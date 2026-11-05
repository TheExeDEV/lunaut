#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include <cctype>
#include <cstdio>

namespace lunaut
{
namespace
{
namespace priv
{

constexpr std::string_view scheme = "file://";

int get_hex(char c);
std::string decode(std::string_view value);
std::string encode(std::string_view value);

}
}

std::string uri::to_path(std::string_view value)
{
	if (!stdx::starts_with(value, priv::scheme))
	{
		return normalize_path(value);
	}

	std::string path = priv::decode(value.substr(priv::scheme.size()));

#if LUNAUT_PLATFORM_WINDOWS
	if (path.size() >= 3 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':')
	{
		path.erase(0, 1);
	}
#endif

	return normalize_path(path);
}

std::string uri::from_path(std::string_view path)
{
	std::string normalized = normalize_path(path);

	if (!normalized.empty() && normalized[0] != '/')
	{
		normalized.insert(normalized.begin(), '/');
	}

	return std::string(priv::scheme) + priv::encode(normalized);
}

std::string uri::normalize_path(std::string_view path)
{
	if (path.empty())
	{
		return {};
	}

	std::string normalized = std::filesystem::path(std::string(path)).lexically_normal().generic_string();

	if (normalized.size() >= 2 && normalized[1] == ':')
	{
		normalized[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(normalized[0])));
	}

	while (normalized.size() > 1 && normalized.back() == '/' && !(normalized.size() == 3 && normalized[1] == ':'))
	{
		normalized.pop_back();
	}

	return normalized;
}

std::string uri::get_relative(const std::filesystem::path& root, const std::string& path)
{
	const std::string base = normalize_path(root.generic_string());
	const std::string target = normalize_path(path);

	if (stdx::starts_with(target, base) && target.size() > base.size() && target[base.size()] == '/')
	{
		return target.substr(base.size() + 1);
	}

	return target;
}

namespace
{
namespace priv
{

int get_hex(char c)
{
	if (c >= '0' && c <= '9')
	{
		return c - '0';
	}
	if (c >= 'a' && c <= 'f')
	{
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F')
	{
		return c - 'A' + 10;
	}
	return -1;
}

std::string decode(std::string_view value)
{
	std::string decoded;
	decoded.reserve(value.size());

	for (size_t i = 0; i < value.size(); ++i)
	{
		if (value[i] == '%' && i + 2 < value.size())
		{
			const int high = get_hex(value[i + 1]);
			const int low = get_hex(value[i + 2]);
			if (high >= 0 && low >= 0)
			{
				decoded.push_back(static_cast<char>(high * 16 + low));
				i += 2;
				continue;
			}
		}

		decoded.push_back(value[i]);
	}

	return decoded;
}

std::string encode(std::string_view value)
{
	std::string encoded;
	encoded.reserve(value.size());

	for (const char c : value)
	{
		const auto u = static_cast<unsigned char>(c);
		if (std::isalnum(u) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
		{
			encoded.push_back(c);
			continue;
		}

		char buffer[4];
		std::snprintf(buffer, sizeof(buffer), "%%%02X", u);
		encoded += buffer;
	}

	return encoded;
}

}
}

}
