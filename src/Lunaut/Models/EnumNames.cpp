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

const std::map<enumitem_message_type, std::string_view> message_type_names = {
	{enumitem_message_type::Error, "Error"},
	{enumitem_message_type::Warning, "Warning"},
	{enumitem_message_type::Info, "Info"},
	{enumitem_message_type::Log, "Log"},
};

const std::map<enumitem_platform_variant, std::string_view> platform_variant_names = {
	{enumitem_platform_variant::Auto, "auto"},
	{enumitem_platform_variant::Standard, "standard"},
	{enumitem_platform_variant::Roblox, "roblox"},
};

const std::map<enumitem_definition_source, std::string_view> definition_source_names = {
	{enumitem_definition_source::Config, "Config"},
	{enumitem_definition_source::Cli, "Cli"},
	{enumitem_definition_source::Discovered, "Discovered"},
	{enumitem_definition_source::Roblox, "Roblox"},
};

const std::map<enumitem_fflag_source, std::string_view> fflag_source_names = {
	{enumitem_fflag_source::Default, "Default"},
	{enumitem_fflag_source::EnableByDefault, "EnableByDefault"},
	{enumitem_fflag_source::Sync, "Sync"},
	{enumitem_fflag_source::NewSolver, "NewSolver"},
	{enumitem_fflag_source::Override, "Override"},
	{enumitem_fflag_source::Cli, "Cli"},
};

template <typename T>
std::string_view get_name_from(const std::map<T, std::string_view>& names, T item)
{
	const auto found = names.find(item);
	return found == names.end() ? std::string_view("Unknown") : found->second;
}

}
}

namespace enum_message_type
{
std::string_view get_name(enumitem_message_type item)
{
	return priv::get_name_from(priv::message_type_names, item);
}
}

namespace enum_platform_variant
{
std::string_view get_name(enumitem_platform_variant item)
{
	return priv::get_name_from(priv::platform_variant_names, item);
}

std::optional<enumitem_platform_variant> from_name(std::string_view name)
{
	for (const auto& [item, item_name] : priv::platform_variant_names)
	{
		if (item_name == name)
		{
			return item;
		}
	}

	return std::nullopt;
}
}

namespace enum_definition_source
{
std::string_view get_name(enumitem_definition_source item)
{
	return priv::get_name_from(priv::definition_source_names, item);
}
}

namespace enum_fflag_source
{
std::string_view get_name(enumitem_fflag_source item)
{
	return priv::get_name_from(priv::fflag_source_names, item);
}
}

}
