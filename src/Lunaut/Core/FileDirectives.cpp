#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Ast.h"
#include "Luau/ParseResult.h"

#include <algorithm>
#include <unordered_map>

namespace lunaut
{
namespace
{
namespace priv
{

constexpr std::string_view ignore_directive = "ignore";
constexpr std::string_view silence_directive = "noerror";
constexpr std::string_view lint_value = "nolint";
constexpr std::string_view any_type = "any";

const std::unordered_map<std::string_view, Luau::Mode> relevant_modes = {
	{"strict", Luau::Mode::Strict},
	{"nonstrict", Luau::Mode::Nonstrict},
	{"nocheck", Luau::Mode::NoCheck},
};

class returnwriter : public Luau::AstVisitor
{
public:
	explicit returnwriter(Luau::SourceModule& source);

	bool visit(Luau::AstExprFunction* node) override;
	bool visit(Luau::AstStatReturn* node) override;

private:
	Luau::SourceModule& source;
};

std::optional<std::string> get_ignore_value(const Luau::HotComment& comment);
bool is_lint_value(const std::string& value);

}
}

void filedirectives::apply(Luau::SourceModule& source, Luau::Mode fallback)
{
	std::vector<Luau::HotComment> lint_comments;
	std::optional<Luau::Mode> mode;

	for (const Luau::HotComment& comment : source.hotcomments)
	{
		std::optional<std::string> value = priv::get_ignore_value(comment);

		if (!value)
		{
			continue;
		}

		const auto found = priv::relevant_modes.find(*value);

		if (found != priv::relevant_modes.end())
		{
			mode = found->second;
		}
		else if (priv::is_lint_value(*value))
		{
			lint_comments.push_back(Luau::HotComment{true, comment.location, *value});
		}
	}

	source.hotcomments.insert(source.hotcomments.end(), lint_comments.begin(), lint_comments.end());

	if (mode)
	{
		source.mode = mode;
	}

	if (source.root != nullptr && source.allocator && source.names && source.mode.value_or(fallback) == Luau::Mode::NoCheck)
	{
		priv::returnwriter writer(source);
		source.root->visit(&writer);
	}
}

bool filedirectives::is_silenced(const Luau::SourceModule& source)
{
	return std::any_of(
		source.hotcomments.begin(),
		source.hotcomments.end(),
		[](const Luau::HotComment& comment) { return comment.header && stdx::trim(comment.content) == priv::silence_directive; });
}

std::vector<Luau::HotComment> filedirectives::get_unknown_ignores(const Luau::SourceModule& source)
{
	std::vector<Luau::HotComment> unknown;

	for (const Luau::HotComment& comment : source.hotcomments)
	{
		std::optional<std::string> value = priv::get_ignore_value(comment);

		if (value && priv::relevant_modes.find(*value) == priv::relevant_modes.end() && !priv::is_lint_value(*value))
		{
			unknown.push_back(comment);
		}
	}

	return unknown;
}

bool filedirectives::is_own_directive(const std::string& name)
{
	return name == priv::ignore_directive || name == priv::silence_directive;
}

namespace
{
namespace priv
{

returnwriter::returnwriter(Luau::SourceModule& source)
	: source(source)
{
}

bool returnwriter::visit(Luau::AstExprFunction* node)
{
	return false;
}

bool returnwriter::visit(Luau::AstStatReturn* node)
{
	Luau::AstName name = source.names->getOrAdd(std::string(any_type).c_str());

	for (size_t index = 0; index < node->list.size; ++index)
	{
		Luau::AstExpr* expr = node->list.data[index];
		Luau::AstType* annotation =
			source.allocator->alloc<Luau::AstTypeReference>(expr->location, std::nullopt, name, std::nullopt, expr->location);
		node->list.data[index] = source.allocator->alloc<Luau::AstExprTypeAssertion>(expr->location, expr, annotation);
	}

	return false;
}

std::optional<std::string> get_ignore_value(const Luau::HotComment& comment)
{
	if (!comment.header)
	{
		return std::nullopt;
	}

	std::string content = stdx::trim(comment.content);

	if (!stdx::starts_with(content, ignore_directive) || content.size() == ignore_directive.size() ||
		(content[ignore_directive.size()] != ' ' && content[ignore_directive.size()] != '\t'))
	{
		return std::nullopt;
	}

	std::string value = stdx::trim(content.substr(ignore_directive.size()));

	if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
	{
		value = value.substr(1, value.size() - 2);
	}

	return value;
}

bool is_lint_value(const std::string& value)
{
	return value == lint_value ||
		(stdx::starts_with(value, lint_value) && value.size() > lint_value.size() &&
		 (value[lint_value.size()] == ' ' || value[lint_value.size()] == '\t'));
}

}
}

}
