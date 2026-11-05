#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Common.h"
#include "Luau/ExperimentalFlags.h"

#include <algorithm>
#include <cstring>

LUAU_FASTINT(LuauTarjanChildLimit)
LUAU_FASTINT(LuauTableTypeMaximumStringifierLength)

namespace lunaut
{
namespace
{
namespace priv
{

constexpr std::string_view studio_settings = "https://clientsettingscdn.roblox.com/v1/settings/application?applicationName=PCStudioApp";
constexpr std::string_view studio_cache = "fflags.PCStudioApp.json";

const std::vector<std::string_view> flag_prefixes = {"DFFlag", "DFInt", "FFlag", "FInt"};

std::map<std::string, enumitem_fflag_source> sources;

Luau::FValue<bool>* find_bool(const std::string& name);
Luau::FValue<int>* find_int(const std::string& name);
std::optional<bool> parse_bool(const std::string& value);
void apply_required();
void apply_map(const std::map<std::string, std::string>& flags, enumitem_fflag_source source, client& owner, bool report_unknown);

}
}

std::vector<fflagstateinfo>
fflagregistry::apply(const fflaginfo& info, const std::map<std::string, std::string>& cli, bool no_flags_enabled, client& owner)
{
	if (info.enable_by_default && !no_flags_enabled)
	{
		for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag != nullptr; flag = flag->next)
		{
			if (std::strncmp(flag->name, "Luau", 4) == 0 && !Luau::isAnalysisFlagExperimental(flag->name))
			{
				flag->value = true;
				priv::sources[flag->name] = enumitem_fflag_source::EnableByDefault;
			}
		}
	}

#if FFLAG_FFLAGS_SYNC
	if (info.sync)
	{
		result<std::map<std::string, std::string>> synced = sync_from_roblox(false);

		if (synced)
		{
			priv::apply_map(synced.value, enumitem_fflag_source::Sync, owner, false);
			owner.send_log(enumitem_message_type::Info, "synced " + std::to_string(synced.value.size()) + " fflags from Roblox Studio");
		}
		else
		{
			owner.send_log(enumitem_message_type::Warning, "fflags sync failed: " + synced.message);
		}
	}
#endif

	if (info.enable_new_solver)
	{
		priv::apply_map({{"LuauSolverV2", "true"}}, enumitem_fflag_source::NewSolver, owner, true);
	}

	priv::apply_map(info.override, enumitem_fflag_source::Override, owner, true);
	priv::apply_map(cli, enumitem_fflag_source::Cli, owner, true);
	priv::apply_required();

	return get_flags();
}

std::vector<fflagstateinfo> fflagregistry::get_flags()
{
	std::vector<fflagstateinfo> flags;

	for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag != nullptr; flag = flag->next)
	{
		const auto source = priv::sources.find(flag->name);
		flags.push_back(
			fflagstateinfo{
				flag->name,
				flag->value ? "true" : "false",
				"bool",
				source == priv::sources.end() ? enumitem_fflag_source::Default : source->second,
			});
	}

	for (Luau::FValue<int>* flag = Luau::FValue<int>::list; flag != nullptr; flag = flag->next)
	{
		const auto source = priv::sources.find(flag->name);
		flags.push_back(
			fflagstateinfo{
				flag->name,
				std::to_string(flag->value),
				"int",
				source == priv::sources.end() ? enumitem_fflag_source::Default : source->second,
			});
	}

	std::sort(flags.begin(), flags.end(), [](const fflagstateinfo& a, const fflagstateinfo& b) { return a.name < b.name; });
	return flags;
}

result<nothing> fflagregistry::set_flag(const std::string& name, const std::string& value, enumitem_fflag_source source)
{
	if (Luau::FValue<bool>* flag = priv::find_bool(name))
	{
		std::optional<bool> parsed = priv::parse_bool(value);

		if (!parsed)
		{
			return result<nothing>::fail("expected boolean for fflag " + name + ", got '" + value + "'");
		}

		flag->value = *parsed;
		priv::sources[name] = source;
		return result<nothing>::ok({});
	}

	if (Luau::FValue<int>* flag = priv::find_int(name))
	{
		try
		{
			flag->value = std::stoi(value);
		}
		catch (const std::exception&)
		{
			return result<nothing>::fail("expected integer for fflag " + name + ", got '" + value + "'");
		}

		priv::sources[name] = source;
		return result<nothing>::ok({});
	}

	return result<nothing>::fail("unknown fflag: " + name);
}

result<std::map<std::string, std::string>> fflagregistry::sync_from_roblox(bool refresh)
{
	result<std::string> response = cache::get_or_fetch(std::string(priv::studio_cache), std::string(priv::studio_settings), refresh);

	if (!response)
	{
		return result<std::map<std::string, std::string>>::fail(response.message);
	}

	std::map<std::string, std::string> flags;

	try
	{
		json data = json::parse(response.value);

		for (const auto& [key, value] : data.at("applicationSettings").items())
		{
			for (std::string_view prefix : priv::flag_prefixes)
			{
				if (stdx::starts_with(key, prefix))
				{
					flags[key.substr(prefix.size())] = value.is_string() ? value.get<std::string>() : value.dump();
					break;
				}
			}
		}
	}
	catch (const std::exception& error)
	{
		return result<std::map<std::string, std::string>>::fail(std::string("malformed studio settings: ") + error.what());
	}

	return result<std::map<std::string, std::string>>::ok(flags);
}

namespace
{
namespace priv
{

Luau::FValue<bool>* find_bool(const std::string& name)
{
	for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag != nullptr; flag = flag->next)
	{
		if (name == flag->name)
		{
			return flag;
		}
	}

	return nullptr;
}

Luau::FValue<int>* find_int(const std::string& name)
{
	for (Luau::FValue<int>* flag = Luau::FValue<int>::list; flag != nullptr; flag = flag->next)
	{
		if (name == flag->name)
		{
			return flag;
		}
	}

	return nullptr;
}

std::optional<bool> parse_bool(const std::string& value)
{
	std::string lowered = stdx::to_lower(value);

	if (lowered == "true")
	{
		return true;
	}

	if (lowered == "false")
	{
		return false;
	}

	return std::nullopt;
}

void apply_required()
{
	if (FInt::LuauTarjanChildLimit > 0 && FInt::LuauTarjanChildLimit < 15000)
	{
		FInt::LuauTarjanChildLimit.value = 15000;
	}

	if (FInt::LuauTableTypeMaximumStringifierLength == 40)
	{
		FInt::LuauTableTypeMaximumStringifierLength.value = 0;
	}
}

void apply_map(const std::map<std::string, std::string>& flags, enumitem_fflag_source source, client& owner, bool report_unknown)
{
	for (const auto& [name, value] : flags)
	{
		result<nothing> applied = fflagregistry::set_flag(name, value, source);

		if (!applied && (report_unknown || find_bool(name) != nullptr || find_int(name) != nullptr))
		{
			owner.send_log(enumitem_message_type::Warning, applied.message);
		}
	}
}

}
}

}
