#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Ast.h"

namespace lunaut
{
namespace
{
namespace priv
{

constexpr std::string_view region_open = "--#region";
constexpr std::string_view region_close = "--#endregion";

struct fold_collector final : Luau::AstVisitor
{
	bool visit(Luau::AstExprFunction* node) override;
	bool visit(Luau::AstExprTable* node) override;
	bool visit(Luau::AstStatIf* node) override;
	bool visit(Luau::AstStatWhile* node) override;
	bool visit(Luau::AstStatFor* node) override;
	bool visit(Luau::AstStatForIn* node) override;
	bool visit(Luau::AstStatRepeat* node) override;
	bool visit(Luau::AstStatBlock* node) override;
	bool visit(Luau::AstStatTypeAlias* node) override;

	void add(unsigned int start, unsigned int end, std::string_view kind = {});

	const Luau::AstStatBlock* root = nullptr;
	json ranges = json::array();
	std::set<unsigned int> starts;
};

}
}

json workspace::get_folding_ranges(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);

	if (document == nullptr)
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

	priv::fold_collector collected;
	collected.root = source->root;
	source->root->visit(&collected);

	std::optional<unsigned int> comment_start;
	unsigned int comment_end = 0;

	for (const Luau::Comment& comment : source->commentLocations)
	{
		if (comment.location.begin.line != comment.location.end.line)
		{
			collected.add(comment.location.begin.line, comment.location.end.line, "comment");
			continue;
		}

		if (comment_start && comment.location.begin.line == comment_end + 1)
		{
			comment_end = comment.location.begin.line;
			continue;
		}

		if (comment_start && comment_end > *comment_start)
		{
			collected.add(*comment_start, comment_end, "comment");
		}

		comment_start = comment.location.begin.line;
		comment_end = comment.location.begin.line;
	}

	if (comment_start && comment_end > *comment_start)
	{
		collected.add(*comment_start, comment_end, "comment");
	}

	std::vector<std::string> lines = stdx::split(document->get_text(), '\n');
	std::vector<unsigned int> regions;

	for (unsigned int line = 0; line < lines.size(); ++line)
	{
		std::string text = stdx::trim(lines[line]);

		if (stdx::starts_with(text, priv::region_close))
		{
			if (!regions.empty())
			{
				collected.add(regions.back(), line, "region");
				regions.pop_back();
			}
		}
		else if (stdx::starts_with(text, priv::region_open))
		{
			regions.push_back(line);
		}
	}

	return collected.ranges;
}

namespace
{
namespace priv
{

bool fold_collector::visit(Luau::AstExprFunction* node)
{
	add(node->location.begin.line, node->location.end.line);
	return true;
}

bool fold_collector::visit(Luau::AstExprTable* node)
{
	add(node->location.begin.line, node->location.end.line);
	return true;
}

bool fold_collector::visit(Luau::AstStatIf* node)
{
	unsigned int end = node->elsebody != nullptr ? node->elsebody->location.begin.line : node->location.end.line;
	add(node->location.begin.line, end);

	if (node->elsebody != nullptr && node->elsebody->is<Luau::AstStatBlock>() && node->elseLocation)
	{
		add(node->elseLocation->begin.line, node->location.end.line);
	}

	return true;
}

bool fold_collector::visit(Luau::AstStatWhile* node)
{
	add(node->location.begin.line, node->location.end.line);
	return true;
}

bool fold_collector::visit(Luau::AstStatFor* node)
{
	add(node->location.begin.line, node->location.end.line);
	return true;
}

bool fold_collector::visit(Luau::AstStatForIn* node)
{
	add(node->location.begin.line, node->location.end.line);
	return true;
}

bool fold_collector::visit(Luau::AstStatRepeat* node)
{
	add(node->location.begin.line, node->location.end.line);
	return true;
}

bool fold_collector::visit(Luau::AstStatBlock* node)
{
	if (node->hasEnd && node != root)
	{
		add(node->location.begin.line, node->location.end.line);
	}

	return true;
}

bool fold_collector::visit(Luau::AstStatTypeAlias* node)
{
	add(node->location.begin.line, node->location.end.line);
	return true;
}

void fold_collector::add(unsigned int start, unsigned int end, std::string_view kind)
{
	unsigned int last = kind.empty() && end > start ? end - 1 : end;

	if (last <= start || !starts.insert(start * 4 + (kind.empty() ? 0 : kind == "comment" ? 1 : 2)).second)
	{
		return;
	}

	json range = {{"startLine", start}, {"endLine", last}};

	if (!kind.empty())
	{
		range["kind"] = kind;
	}

	ranges.push_back(range);
}

}
}

}
