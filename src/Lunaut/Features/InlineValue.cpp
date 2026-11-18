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

struct variable_collector final : Luau::AstVisitor
{
	variable_collector(Luau::Location range, unsigned int stopped_line);

	bool visit(Luau::AstExprLocal* node) override;
	bool visit(Luau::AstStatLocal* node) override;
	bool visit(Luau::AstStatFor* node) override;
	bool visit(Luau::AstStatForIn* node) override;
	bool visit(Luau::AstExprFunction* node) override;

	void add(const Luau::Location& location, const char* name);

	Luau::Location range;
	unsigned int stopped_line;
	std::vector<std::pair<Luau::Location, std::string>> found;
};

}
}

json workspace::get_inline_values(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);
	json values = json::array();

	if (document == nullptr)
	{
		return values;
	}

	Luau::ModuleName name = resolver.get_module_name(address);
	frontend.parse(name);

	const Luau::SourceModule* source = frontend.getSourceModule(name);

	if (source == nullptr || source->root == nullptr)
	{
		return values;
	}

	Luau::Position start = document->to_luau_position(params["range"]["start"]);
	Luau::Position end = document->to_luau_position(params["range"]["end"]);
	unsigned int stopped_line = params.value("/context/stoppedLocation/end/line"_json_pointer, end.line);

	priv::variable_collector collected(Luau::Location{start, end}, stopped_line);
	source->root->visit(&collected);

	for (const auto& [location, variable] : collected.found)
	{
		values.push_back({{"range", document->to_lsp_range(location)}, {"variableName", variable}, {"caseSensitiveLookup", true}});
	}

	return values;
}

namespace
{
namespace priv
{

variable_collector::variable_collector(Luau::Location range, unsigned int stopped_line)
	: range(range)
	, stopped_line(stopped_line)
{
}

bool variable_collector::visit(Luau::AstExprLocal* node)
{
	add(node->location, node->local->name.value);
	return true;
}

bool variable_collector::visit(Luau::AstStatLocal* node)
{
	for (Luau::AstLocal* local : node->vars)
	{
		add(local->location, local->name.value);
	}

	return true;
}

bool variable_collector::visit(Luau::AstStatFor* node)
{
	add(node->var->location, node->var->name.value);
	return true;
}

bool variable_collector::visit(Luau::AstStatForIn* node)
{
	for (Luau::AstLocal* local : node->vars)
	{
		add(local->location, local->name.value);
	}

	return true;
}

bool variable_collector::visit(Luau::AstExprFunction* node)
{
	for (Luau::AstLocal* argument : node->args)
	{
		add(argument->location, argument->name.value);
	}

	return true;
}

void variable_collector::add(const Luau::Location& location, const char* name)
{
	if (location.begin.line > stopped_line || !range.encloses(location))
	{
		return;
	}

	found.emplace_back(location, name);
}

}
}

}
