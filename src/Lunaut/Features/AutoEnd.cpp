#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Ast.h"
#include "Luau/AstQuery.h"

#include <algorithm>

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

struct blockinfo
{
	std::vector<Luau::AstNode*> ancestry;
};

std::optional<blockinfo> find_open_block(const Luau::SourceModule& source, Luau::Position probe, unsigned int cursor_line);
Luau::AstNode* get_parent(const std::vector<Luau::AstNode*>& ancestry);
closing get_closing(const std::vector<Luau::AstNode*>& ancestry);
std::vector<std::pair<unsigned int, std::string>> collect_closings(const std::vector<Luau::AstNode*>& ancestry);
std::string get_indent(const std::string& line);
std::optional<json> get_missing_keyword(Luau::AstNode* parent, const textdocument& document);
std::vector<std::string> get_lines(const std::string& text);
std::optional<size_t> find_unclosed(const std::string& line, size_t until, char quote);
bool is_escaped(const std::string& line, size_t index);
json make_edit(const textdocument& document, Luau::Position begin, Luau::Position end, const std::string& text);

}
}

void workspace::invoke_end_completion(const json& params)
{
	json result = get_end_edits(params);
	json edits = result.value("edits", json::array());

	if (!edits.empty())
	{
		owner.send_edit({{"changes", {{params["textDocument"].value("uri", ""), edits}}}}, result.value("move_cursor", false));
	}
}

json workspace::get_end_edits(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);
	json result = {{"edits", json::array()}, {"move_cursor", false}};

	if (document == nullptr || !settings.features.autocomplete_end)
	{
		return result;
	}

	Luau::Position position = document->to_luau_position(params["position"]);
	Luau::ModuleName name = resolver.get_module_name(address);
	frontend.parse(name);
	const Luau::SourceModule* source = frontend.getSourceModule(name);

	if (source == nullptr)
	{
		return result;
	}

	std::vector<std::string> lines = priv::get_lines(document->get_text());
	std::optional<priv::blockinfo> block = priv::find_open_block(*source, position, position.line);

	if ((!block || priv::get_closing(block->ancestry) == priv::closing::None) && position.line > 0 && position.line - 1 < lines.size())
	{
		Luau::Position opener{position.line - 1, static_cast<unsigned int>(lines[position.line - 1].size())};
		std::optional<priv::blockinfo> fallback = priv::find_open_block(*source, opener, position.line);

		if (fallback && priv::get_closing(fallback->ancestry) != priv::closing::None)
		{
			block = fallback;
		}
	}

	if (!block)
	{
		return result;
	}

	std::vector<Luau::AstNode*>& ancestry = block->ancestry;
	Luau::AstNode* parent = priv::get_parent(ancestry);
	json& edits = result["edits"];

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
		std::string line_text = position.line < lines.size() ? lines[position.line] : "";
		size_t column = std::min<size_t>(position.column, line_text.size());
		std::string after = stdx::trim(line_text.substr(column));
		std::string indent;
		Luau::Position cursor{position.line, static_cast<unsigned int>(column)};
		Luau::Position line_end{position.line, static_cast<unsigned int>(line_text.size())};
		Luau::Position next{position.line + 1, 0};
		bool last_line = position.line + 1 >= lines.size();

		std::vector<std::pair<unsigned int, std::string>> openers = priv::collect_closings(ancestry);

		if (!openers.empty() && openers.front().first < lines.size())
		{
			indent = priv::get_indent(lines[openers.front().first]);
		}
		else if (position.line > 0 && position.line - 1 < lines.size())
		{
			indent = priv::get_indent(lines[position.line - 1]);
		}

		if (after.empty() && last_line)
		{
			edits.push_back(priv::make_edit(*document, line_end, line_end, "\n" + indent + keyword));
		}
		else if (after.empty())
		{
			const std::string& following = lines[position.line + 1];
			std::string following_indent = priv::get_indent(following);
			char first = following.size() > following_indent.size() ? following[following_indent.size()] : '\0';

			if (first == ')' || first == ',')
			{
				Luau::Position code{position.line + 1, static_cast<unsigned int>(following_indent.size())};
				edits.push_back(priv::make_edit(*document, next, code, indent + keyword));
			}
			else
			{
				edits.push_back(priv::make_edit(*document, next, next, indent + keyword + "\n"));
			}
		}
		else
		{
			std::string closings;

			for (const auto& [line, word] : priv::collect_closings(ancestry))
			{
				closings += (closings.empty() ? "" : "\n") + priv::get_indent(line < lines.size() ? lines[line] : "") + word;
			}

			if (closings.empty())
			{
				closings = indent + keyword;
			}

			if (last_line)
			{
				edits.push_back(priv::make_edit(*document, cursor, line_end, "\n" + closings + after));
			}
			else
			{
				edits.push_back(priv::make_edit(*document, cursor, line_end, ""));
				edits.push_back(priv::make_edit(*document, next, next, closings + after + "\n"));
			}
		}
	}

	return result;
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

std::optional<blockinfo> find_open_block(const Luau::SourceModule& source, Luau::Position probe, unsigned int cursor_line)
{
	std::vector<Luau::AstNode*> ancestry = Luau::findAstAncestryOfPosition(source, probe);

	if (ancestry.size() < 2)
	{
		return std::nullopt;
	}

	bool cursor_in_error = is_error(ancestry.back()) && ancestry.back()->location.begin.line == cursor_line;

	while (!ancestry.empty() && is_error(ancestry.back()))
	{
		ancestry.pop_back();
	}

	if (ancestry.empty())
	{
		return std::nullopt;
	}

	Luau::AstNode* current = ancestry.back();

	if (Luau::AstStatFunction* function = current->as<Luau::AstStatFunction>())
	{
		ancestry.push_back(function->func);
		ancestry.push_back(function->func->body);
		current = function->func->body;
	}

	Luau::AstStatBlock* body = current->as<Luau::AstStatBlock>();

	if (body == nullptr)
	{
		return std::nullopt;
	}

	if (cursor_line - body->location.begin.line > 1)
	{
		bool after_last = body->body.size == 0 || body->body.data[body->body.size - 1]->location.end.line < cursor_line;

		if (body->hasEnd || !after_last)
		{
			return std::nullopt;
		}
	}

	Luau::AstNode* parent = get_parent(ancestry);

	if (parent != nullptr && should_suppress(parent, cursor_in_error))
	{
		return std::nullopt;
	}

	return blockinfo{ancestry};
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

std::vector<std::pair<unsigned int, std::string>> collect_closings(const std::vector<Luau::AstNode*>& ancestry)
{
	std::vector<std::pair<unsigned int, std::string>> closings;

	for (size_t index = ancestry.size(); index-- > 0;)
	{
		Luau::AstNode* node = ancestry[index];
		Luau::AstNode* outer = index > 0 ? ancestry[index - 1] : nullptr;
		unsigned int line = node->location.begin.line;

		if (Luau::AstExprFunction* function = node->as<Luau::AstExprFunction>(); function != nullptr && !function->body->hasEnd)
		{
			closings.emplace_back(line, "end");
		}
		else if (
			Luau::AstStatIf* statement = node->as<Luau::AstStatIf>();
			statement != nullptr && (outer == nullptr || !outer->is<Luau::AstStatIf>()))
		{
			const Luau::AstStatIf* last = statement;

			while (last->elsebody != nullptr && last->elsebody->is<Luau::AstStatIf>())
			{
				last = last->elsebody->as<Luau::AstStatIf>();
			}

			bool has_end = last->thenbody->hasEnd;

			if (last->elsebody != nullptr)
			{
				if (const Luau::AstStatBlock* block = last->elsebody->as<Luau::AstStatBlock>())
				{
					has_end = block->hasEnd;
				}
			}

			if (!has_end)
			{
				closings.emplace_back(line, "end");
			}
		}
		else if (Luau::AstStatWhile* statement = node->as<Luau::AstStatWhile>(); statement != nullptr && !statement->body->hasEnd)
		{
			closings.emplace_back(line, "end");
		}
		else if (Luau::AstStatFor* statement = node->as<Luau::AstStatFor>(); statement != nullptr && !statement->body->hasEnd)
		{
			closings.emplace_back(line, "end");
		}
		else if (Luau::AstStatForIn* statement = node->as<Luau::AstStatForIn>(); statement != nullptr && !statement->body->hasEnd)
		{
			closings.emplace_back(line, "end");
		}
		else if (Luau::AstStatRepeat* statement = node->as<Luau::AstStatRepeat>(); statement != nullptr && !statement->body->hasEnd)
		{
			closings.emplace_back(line, "until ");
		}
		else if (
			Luau::AstStatBlock* block = node->as<Luau::AstStatBlock>();
			block != nullptr && !block->hasEnd && outer != nullptr && outer->is<Luau::AstStatBlock>() && index > 1)
		{
			closings.emplace_back(line, "end");
		}
	}

	return closings;
}

std::string get_indent(const std::string& line)
{
	size_t length = 0;

	while (length < line.size() && (line[length] == ' ' || line[length] == '\t'))
	{
		++length;
	}

	return line.substr(0, length);
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
