#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Autocomplete.h"
#include "Luau/ToString.h"

namespace lunaut
{
namespace
{
namespace priv
{

const std::map<Luau::AutocompleteEntryKind, enumitem_completion_kind> relevant_kinds = {
	{Luau::AutocompleteEntryKind::Property, enumitem_completion_kind::Field},
	{Luau::AutocompleteEntryKind::Binding, enumitem_completion_kind::Variable},
	{Luau::AutocompleteEntryKind::Keyword, enumitem_completion_kind::Keyword},
	{Luau::AutocompleteEntryKind::String, enumitem_completion_kind::Constant},
	{Luau::AutocompleteEntryKind::Type, enumitem_completion_kind::Interface},
	{Luau::AutocompleteEntryKind::Module, enumitem_completion_kind::Module},
	{Luau::AutocompleteEntryKind::GeneratedFunction, enumitem_completion_kind::Function},
	{Luau::AutocompleteEntryKind::RequirePath, enumitem_completion_kind::File},
	{Luau::AutocompleteEntryKind::HotComment, enumitem_completion_kind::Keyword},
};

const std::map<Luau::TypeCorrectKind, std::string_view> relevant_sort = {
	{Luau::TypeCorrectKind::Correct, "0"},
	{Luau::TypeCorrectKind::CorrectFunctionResult, "1"},
	{Luau::TypeCorrectKind::None, "2"},
};

enumitem_completion_kind get_kind(const Luau::AutocompleteEntry& entry);
std::string get_sort_text(const std::string& label, const Luau::AutocompleteEntry& entry);
std::optional<json> get_string_range(const textdocument& document, const std::vector<Luau::AstNode*>& ancestry);

}
}

json workspace::get_completion(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);
	json items = json::array();

	if (document == nullptr)
	{
		return {{"isIncomplete", false}, {"items", items}};
	}

	Luau::Position position = document->to_luau_position(params["position"]);
	Luau::ModuleName name = resolver.get_module_name(address);
	check_module(name, true);

	Luau::AutocompleteResult completion;

	try
	{
		completion = Luau::autocomplete(
			frontend,
			name,
			position,
			[](std::string, std::optional<const Luau::ExternType*>, std::optional<std::string>) -> std::optional<Luau::AutocompleteEntryMap>
			{ return std::nullopt; });
	}
	catch (const std::exception& error)
	{
		owner.send_log(enumitem_message_type::Error, std::string("autocomplete failed: ") + error.what());
		return {{"isIncomplete", false}, {"items", items}};
	}

	Luau::ToStringOptions options;
	options.functionTypeArguments = true;

	std::optional<json> string_range = priv::get_string_range(*document, completion.ancestry);

	for (const auto& [label, entry] : completion.entryMap)
	{
		json item = {
			{"label", label},
			{"kind", static_cast<int>(priv::get_kind(entry))},
			{"sortText", priv::get_sort_text(label, entry)},
		};

		if (entry.type)
		{
			item["detail"] = Luau::toString(*entry.type, options);
		}

		if (entry.deprecated)
		{
			item["tags"] = {1};
		}

		if (entry.kind == Luau::AutocompleteEntryKind::RequirePath && string_range)
		{
			item["textEdit"] = {{"range", *string_range}, {"newText", entry.insertText.value_or(label)}};
			item["filterText"] = entry.insertText.value_or(label);
		}
		else if (entry.insertText)
		{
			item["insertText"] = *entry.insertText;
		}
		else if (entry.parens == Luau::ParenthesesRecommendation::CursorAfter)
		{
			item["insertText"] = label + "()";
		}
		else if (entry.parens == Luau::ParenthesesRecommendation::CursorInside)
		{
			item["insertText"] = label + "($0)";
			item["insertTextFormat"] = 2;
		}

		items.push_back(item);
	}

	return {{"isIncomplete", false}, {"items", items}};
}

namespace
{
namespace priv
{

enumitem_completion_kind get_kind(const Luau::AutocompleteEntry& entry)
{
	bool is_function = entry.type && Luau::get<Luau::FunctionType>(Luau::follow(*entry.type)) != nullptr;

	if (entry.kind == Luau::AutocompleteEntryKind::Property && is_function)
	{
		return entry.indexedWithSelf ? enumitem_completion_kind::Method : enumitem_completion_kind::Function;
	}

	if (entry.kind == Luau::AutocompleteEntryKind::Binding && is_function)
	{
		return enumitem_completion_kind::Function;
	}

	const auto found = relevant_kinds.find(entry.kind);
	return found == relevant_kinds.end() ? enumitem_completion_kind::Text : found->second;
}

std::string get_sort_text(const std::string& label, const Luau::AutocompleteEntry& entry)
{
	if (entry.kind == Luau::AutocompleteEntryKind::Keyword)
	{
		return "5" + label;
	}

	if (entry.wrongIndexType)
	{
		return "4" + label;
	}

	const auto found = relevant_sort.find(entry.typeCorrect);
	return std::string(found == relevant_sort.end() ? "3" : found->second) + label;
}

std::optional<json> get_string_range(const textdocument& document, const std::vector<Luau::AstNode*>& ancestry)
{
	if (ancestry.empty())
	{
		return std::nullopt;
	}

	Luau::AstExprConstantString* value = ancestry.back()->as<Luau::AstExprConstantString>();

	if (value == nullptr || value->location.begin.line != value->location.end.line)
	{
		return std::nullopt;
	}

	Luau::Location inner = value->location;
	inner.begin.column += 1;

	if (inner.end.column > inner.begin.column && value->isQuoted())
	{
		inner.end.column -= 1;
	}

	return document.to_lsp_range(inner);
}

}
}

}
