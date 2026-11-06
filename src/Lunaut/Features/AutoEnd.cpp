#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Ast.h"
#include "Luau/AstQuery.h"

namespace lunaut
{
namespace
{
namespace priv
{

enum class closing
{
	None,
	End,
	Until,
};

bool is_broken(Luau::AstExpr* expr);
bool is_error(const Luau::AstNode* node);
bool should_suppress(Luau::AstNode* parent, bool cursor_in_error);
Luau::AstNode* get_parent(const std::vector<Luau::AstNode*>& ancestry);
closing get_closing(const std::vector<Luau::AstNode*>& ancestry);
std::optional<json> get_missing_keyword(Luau::AstNode* parent, const textdocument& document);
std::vector<std::string> get_lines(const std::string& text);
std::optional<size_t> find_unclosed(const std::string& line, size_t until, char quote);
bool is_escaped(const std::string& line, size_t index);
json make_edit(const textdocument& document, Luau::Position begin, Luau::Position end, const std::string& text);

}
}

void workspace::invoke_end_completion(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);

	if (document == nullptr || !settings.features.autocomplete_end)
	{
		return;
	}

	Luau::Position position = document->to_luau_position(params["position"]);
	Luau::ModuleName name = resolver.get_module_name(address);
	frontend.parse(name);
	const Luau::SourceModule* source = frontend.getSourceModule(name);

	if (source == nullptr)
	{
		return;
	}

	std::vector<Luau::AstNode*> ancestry = Luau::findAstAncestryOfPosition(*source, position);

	if (ancestry.size() < 2)
	{
		return;
	}

	bool cursor_in_error = priv::is_error(ancestry.back()) && ancestry.back()->location.begin.line == position.line;

	while (!ancestry.empty() && priv::is_error(ancestry.back()))
	{
		ancestry.pop_back();
	}

	if (ancestry.empty())
	{
		return;
	}

	Luau::AstNode* current = ancestry.back();

	if (Luau::AstStatFunction* function = current->as<Luau::AstStatFunction>())
	{
		ancestry.push_back(function->func);
		ancestry.push_back(function->func->body);
		current = function->func->body;
	}

	if (!current->is<Luau::AstStatBlock>() || position.line - current->location.begin.line > 1)
	{
		return;
	}

	Luau::AstNode* parent = priv::get_parent(ancestry);

	if (parent != nullptr && priv::should_suppress(parent, cursor_in_error))
	{
		return;
	}

	json edits = json::array();
	bool move_cursor = false;

	if (parent != nullptr)
	{
		if (std::optional<json> keyword = priv::get_missing_keyword(parent, *document))
		{
			edits.push_back(*keyword);
		}
	}

	priv::closing closing = priv::get_closing(ancestry);

	if (closing != priv::closing::None)
	{
		std::string keyword = closing == priv::closing::Until ? "until " : "end";
		std::vector<std::string> lines = priv::get_lines(document->get_text());
		std::string current_line = position.line < lines.size() ? stdx::trim(lines[position.line]) : "";
		std::string indent;

		if (position.line > 0 && position.line - 1 < lines.size())
		{
			const std::string& previous = lines[position.line - 1];

			if (!previous.empty() && (previous[0] == ' ' || previous[0] == '\t'))
			{
				for (char c : previous)
				{
					if (c != previous[0])
					{
						break;
					}

					indent += c;
				}
			}
		}

		if (position.line + 1 >= lines.size() || !current_line.empty())
		{
			edits.push_back(
				priv::make_edit(
					*document,
					Luau::Position{position.line, 0},
					Luau::Position{position.line + 1, 0},
					"\n" + indent + keyword + current_line + "\n"));
			move_cursor = true;
		}
		else
		{
			Luau::Position next{position.line + 1, 0};
			edits.push_back(priv::make_edit(*document, next, next, indent + keyword + "\n"));
		}
	}

	if (!edits.empty())
	{
		owner.send_edit({{"changes", {{address, edits}}}}, move_cursor);
	}
}

json workspace::get_on_type_edits(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);

	if (document == nullptr || !settings.features.convert_quotes || params.value("ch", "") != "{")
	{
		return nullptr;
	}

	Luau::Position position = document->to_luau_position(params["position"]);
	std::vector<std::string> lines = priv::get_lines(document->get_text());

	if (position.line >= lines.size())
	{
		return nullptr;
	}

	const std::string& line = lines[position.line];
	size_t column = std::min<size_t>(position.column, line.size());
	std::optional<size_t> double_quote = priv::find_unclosed(line, column, '"');
	std::optional<size_t> single_quote = priv::find_unclosed(line, column, '\'');

	if (!double_quote && !single_quote)
	{
		return nullptr;
	}

	size_t quote = std::min(double_quote.value_or(SIZE_MAX), single_quote.value_or(SIZE_MAX));

	if (priv::find_unclosed(line, quote, '`'))
	{
		return nullptr;
	}

	char opening = line[quote];
	size_t closing = line.find(opening, column);

	while (closing != std::string::npos && priv::is_escaped(line, closing))
	{
		closing = line.find(opening, closing + 1);
	}

	std::string content = line.substr(quote + 1, closing == std::string::npos ? std::string::npos : closing - quote - 1);

	if (content.find('`') != std::string::npos)
	{
		return nullptr;
	}

	json edits = json::array();
	unsigned int at = static_cast<unsigned int>(quote);
	edits.push_back(priv::make_edit(*document, Luau::Position{position.line, at}, Luau::Position{position.line, at + 1}, "`"));

	if (closing != std::string::npos)
	{
		unsigned int end = static_cast<unsigned int>(closing);
		edits.push_back(priv::make_edit(*document, Luau::Position{position.line, end}, Luau::Position{position.line, end + 1}, "`"));
	}

	return edits;
}

namespace
{
namespace priv
{

bool is_broken(Luau::AstExpr* expr)
{
	Luau::AstExprError* error = expr != nullptr ? expr->as<Luau::AstExprError>() : nullptr;
	return error != nullptr && error->location.begin != error->location.end;
}

bool is_error(const Luau::AstNode* node)
{
	return node->is<Luau::AstStatError>() || node->is<Luau::AstExprError>();
}

bool should_suppress(Luau::AstNode* parent, bool cursor_in_error)
{
	if (Luau::AstStatIf* statement = parent->as<Luau::AstStatIf>(); statement != nullptr && !statement->thenLocation)
	{
		return cursor_in_error || is_broken(statement->condition);
	}

	if (Luau::AstStatWhile* statement = parent->as<Luau::AstStatWhile>(); statement != nullptr && !statement->hasDo)
	{
		return cursor_in_error || is_broken(statement->condition);
	}

	if (Luau::AstStatForIn* statement = parent->as<Luau::AstStatForIn>(); statement != nullptr && !statement->hasDo)
	{
		if (cursor_in_error)
		{
			return true;
		}

		for (Luau::AstExpr* value : statement->values)
		{
			if (is_broken(value))
			{
				return true;
			}
		}

		return false;
	}

	if (Luau::AstStatFor* statement = parent->as<Luau::AstStatFor>(); statement != nullptr && !statement->hasDo)
	{
		return cursor_in_error || is_broken(statement->from) || is_broken(statement->to) || (statement->step && is_broken(statement->step));
	}

	return false;
}

Luau::AstNode* get_parent(const std::vector<Luau::AstNode*>& ancestry)
{
	if (ancestry.size() < 2)
	{
		return nullptr;
	}

	for (auto it = ++ancestry.rbegin(); it != ancestry.rend(); ++it)
	{
		if (!is_error(*it))
		{
			return *it;
		}
	}

	return nullptr;
}

closing get_closing(const std::vector<Luau::AstNode*>& ancestry)
{
	closing result = closing::None;

	for (auto it = ancestry.rbegin(); it != ancestry.rend(); ++it)
	{
		if (Luau::AstStatForIn* statement = (*it)->as<Luau::AstStatForIn>(); statement != nullptr && !statement->body->hasEnd)
		{
			result = closing::End;
		}
		else if (Luau::AstStatFor* statement = (*it)->as<Luau::AstStatFor>(); statement != nullptr && !statement->body->hasEnd)
		{
			result = closing::End;
		}
		else if (Luau::AstStatIf* statement = (*it)->as<Luau::AstStatIf>())
		{
			bool has_end = statement->thenbody->hasEnd;

			if (statement->elsebody != nullptr)
			{
				if (Luau::AstStatBlock* block = statement->elsebody->as<Luau::AstStatBlock>())
				{
					has_end = block->hasEnd;
				}
			}

			if (!has_end)
			{
				result = closing::End;
			}
		}
		else if (Luau::AstStatWhile* statement = (*it)->as<Luau::AstStatWhile>(); statement != nullptr && !statement->body->hasEnd)
		{
			result = closing::End;
		}
		else if (Luau::AstExprFunction* function = (*it)->as<Luau::AstExprFunction>(); function != nullptr && !function->body->hasEnd)
		{
			result = closing::End;
		}

		if (Luau::AstStatBlock* block = (*it)->as<Luau::AstStatBlock>(); block != nullptr && !block->hasEnd)
		{
			result = closing::End;
		}

		if (Luau::AstStatRepeat* statement = (*it)->as<Luau::AstStatRepeat>(); statement != nullptr && !statement->body->hasEnd)
		{
			result = closing::Until;
		}
	}

	return result;
}

std::optional<json> get_missing_keyword(Luau::AstNode* parent, const textdocument& document)
{
	std::optional<std::pair<Luau::Position, std::string>> insert;

	if (Luau::AstStatIf* statement = parent->as<Luau::AstStatIf>();
		statement != nullptr && statement->condition && !statement->thenLocation)
	{
		insert = std::pair{statement->thenbody->location.begin, " then"};
	}
	else if (
		Luau::AstStatWhile* statement = parent->as<Luau::AstStatWhile>(); statement != nullptr && statement->condition && !statement->hasDo)
	{
		insert = std::pair{statement->body->location.begin, " do"};
	}
	else if (
		Luau::AstStatForIn* statement = parent->as<Luau::AstStatForIn>();
		statement != nullptr && statement->values.size > 0 && !statement->hasDo)
	{
		insert = std::pair{statement->body->location.begin, " do"};
	}
	else if (Luau::AstStatFor* statement = parent->as<Luau::AstStatFor>(); statement != nullptr && !statement->hasDo)
	{
		insert = std::pair{statement->body->location.begin, " do"};
	}

	if (!insert)
	{
		return std::nullopt;
	}

	return make_edit(document, insert->first, insert->first, insert->second);
}

std::vector<std::string> get_lines(const std::string& text)
{
	std::vector<std::string> lines = stdx::split(text, '\n');

	for (std::string& line : lines)
	{
		if (!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}
	}

	return lines;
}

bool is_escaped(const std::string& line, size_t index)
{
	size_t backslashes = 0;

	while (index > backslashes && line[index - backslashes - 1] == '\\')
	{
		++backslashes;
	}

	return backslashes % 2 == 1;
}

std::optional<size_t> find_unclosed(const std::string& line, size_t until, char quote)
{
	size_t count = 0;
	std::optional<size_t> last;

	for (size_t i = 0; i < until && i < line.size(); ++i)
	{
		if (line[i] == quote && !is_escaped(line, i))
		{
			++count;
			last = i;
		}
	}

	return count % 2 == 1 ? last : std::nullopt;
}

json make_edit(const textdocument& document, Luau::Position begin, Luau::Position end, const std::string& text)
{
	return {{"range", {{"start", document.to_lsp_position(begin)}, {"end", document.to_lsp_position(end)}}}, {"newText", text}};
}

}
}

}
