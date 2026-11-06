#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Ast.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lunaut
{
namespace
{
namespace priv
{

struct color
{
	double red = 0;
	double green = 0;
	double blue = 0;
};

using color_parser = std::optional<color> (*)(const Luau::AstExprCall& call);

std::optional<double> get_number(const Luau::AstExprCall& call, size_t index);
std::optional<color> parse_new(const Luau::AstExprCall& call);
std::optional<color> parse_rgb(const Luau::AstExprCall& call);
std::optional<color> parse_hsv(const Luau::AstExprCall& call);
std::optional<color> parse_hex(const Luau::AstExprCall& call);

const std::map<std::string_view, color_parser> relevant_constructors = {
	{"new", &parse_new},
	{"fromRGB", &parse_rgb},
	{"fromHSV", &parse_hsv},
	{"fromHex", &parse_hex},
};

struct color_collector final : Luau::AstVisitor
{
	explicit color_collector(const textdocument& document);

	bool visit(Luau::AstExprCall* node) override;

	const textdocument& document;
	json colors = json::array();
};

double clamp(double value);
std::string format_number(double value);
std::string to_hex(const color& value);

}
}

json workspace::get_document_colors(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);

	if (document == nullptr || !settings.features.colors)
	{
		return json::array();
	}

	Luau::ModuleName name = resolver.get_module_name(address);
	frontend.parse(name);
	const Luau::SourceModule* source = frontend.getSourceModule(name);

	if (source == nullptr || source->root == nullptr)
	{
		return json::array();
	}

	priv::color_collector collected(*document);
	source->root->visit(&collected);
	return collected.colors;
}

json workspace::get_color_presentations(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);
	const json& value = params["color"];
	priv::color picked{value.value("red", 0.0), value.value("green", 0.0), value.value("blue", 0.0)};
	const json& range = params["range"];

	std::string rgb = "Color3.fromRGB(" + std::to_string(std::lround(picked.red * 255)) + ", " +
		std::to_string(std::lround(picked.green * 255)) + ", " + std::to_string(std::lround(picked.blue * 255)) + ")";
	std::string fresh = "Color3.new(" + priv::format_number(picked.red) + ", " + priv::format_number(picked.green) + ", " +
		priv::format_number(picked.blue) + ")";
	std::string hex = "Color3.fromHex(\"" + priv::to_hex(picked) + "\")";

	std::vector<std::string> labels = {rgb, fresh, hex};

	if (document != nullptr)
	{
		Luau::Position begin = document->to_luau_position(range["start"]);
		std::vector<std::string> lines = stdx::split(document->get_text(), '\n');
		std::string current =
			begin.line < lines.size() ? lines[begin.line].substr(std::min<size_t>(begin.column, lines[begin.line].size())) : "";

		if (stdx::starts_with(current, "Color3.new"))
		{
			labels = {fresh, rgb, hex};
		}
		else if (stdx::starts_with(current, "Color3.fromHex"))
		{
			labels = {hex, rgb, fresh};
		}
	}

	json presentations = json::array();

	for (const std::string& label : labels)
	{
		presentations.push_back({{"label", label}, {"textEdit", {{"range", range}, {"newText", label}}}});
	}

	return presentations;
}

namespace
{
namespace priv
{

color_collector::color_collector(const textdocument& document)
	: document(document)
{
}

bool color_collector::visit(Luau::AstExprCall* node)
{
	Luau::AstExprIndexName* index = node->func->as<Luau::AstExprIndexName>();
	Luau::AstExprGlobal* global = index != nullptr ? index->expr->as<Luau::AstExprGlobal>() : nullptr;

	if (global == nullptr || global->name != "Color3")
	{
		return true;
	}

	const auto parser = relevant_constructors.find(index->index.value);

	if (parser == relevant_constructors.end())
	{
		return true;
	}

	if (std::optional<color> found = parser->second(*node))
	{
		colors.push_back({
			{"range", document.to_lsp_range(node->location)},
			{"color", {{"red", clamp(found->red)}, {"green", clamp(found->green)}, {"blue", clamp(found->blue)}, {"alpha", 1.0}}},
		});
	}

	return true;
}

std::optional<double> get_number(const Luau::AstExprCall& call, size_t index)
{
	if (index >= call.args.size)
	{
		return 0.0;
	}

	const Luau::AstExpr* argument = call.args.data[index];

	if (const Luau::AstExprConstantNumber* number = argument->as<Luau::AstExprConstantNumber>())
	{
		return number->value;
	}

	return std::nullopt;
}

std::optional<color> parse_new(const Luau::AstExprCall& call)
{
	std::optional<double> red = get_number(call, 0);
	std::optional<double> green = get_number(call, 1);
	std::optional<double> blue = get_number(call, 2);

	if (!red || !green || !blue)
	{
		return std::nullopt;
	}

	return color{*red, *green, *blue};
}

std::optional<color> parse_rgb(const Luau::AstExprCall& call)
{
	std::optional<color> value = parse_new(call);

	if (!value)
	{
		return std::nullopt;
	}

	return color{value->red / 255.0, value->green / 255.0, value->blue / 255.0};
}

std::optional<color> parse_hsv(const Luau::AstExprCall& call)
{
	std::optional<color> value = parse_new(call);

	if (!value)
	{
		return std::nullopt;
	}

	double hue = std::fmod(clamp(value->red), 1.0) * 6.0;
	double saturation = clamp(value->green);
	double brightness = clamp(value->blue);
	int sector = static_cast<int>(std::floor(hue));
	double fraction = hue - sector;
	double p = brightness * (1 - saturation);
	double q = brightness * (1 - saturation * fraction);
	double t = brightness * (1 - saturation * (1 - fraction));

	const color table[6] = {
		{brightness, t, p},
		{q, brightness, p},
		{p, brightness, t},
		{p, q, brightness},
		{t, p, brightness},
		{brightness, p, q},
	};

	return table[std::clamp(sector, 0, 5)];
}

std::optional<color> parse_hex(const Luau::AstExprCall& call)
{
	if (call.args.size < 1)
	{
		return std::nullopt;
	}

	const Luau::AstExprConstantString* value = call.args.data[0]->as<Luau::AstExprConstantString>();

	if (value == nullptr)
	{
		return std::nullopt;
	}

	std::string text(value->value.data, value->value.size);

	if (stdx::starts_with(text, "#"))
	{
		text.erase(0, 1);
	}

	if (text.size() == 3)
	{
		text = std::string{text[0], text[0], text[1], text[1], text[2], text[2]};
	}

	if (text.size() != 6 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
	{
		return std::nullopt;
	}

	return color{
		std::stoi(text.substr(0, 2), nullptr, 16) / 255.0,
		std::stoi(text.substr(2, 2), nullptr, 16) / 255.0,
		std::stoi(text.substr(4, 2), nullptr, 16) / 255.0,
	};
}

double clamp(double value)
{
	return std::clamp(value, 0.0, 1.0);
}

std::string format_number(double value)
{
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.3f", value);
	std::string text = buffer;

	while (!text.empty() && text.back() == '0')
	{
		text.pop_back();
	}

	if (!text.empty() && text.back() == '.')
	{
		text.pop_back();
	}

	return text.empty() ? "0" : text;
}

std::string to_hex(const color& value)
{
	char buffer[8];
	std::snprintf(
		buffer,
		sizeof(buffer),
		"#%02X%02X%02X",
		static_cast<int>(std::lround(clamp(value.red) * 255)),
		static_cast<int>(std::lround(clamp(value.green) * 255)),
		static_cast<int>(std::lround(clamp(value.blue) * 255)));
	return buffer;
}

}
}

}
