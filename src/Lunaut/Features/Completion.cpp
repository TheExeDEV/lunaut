#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Autocomplete.h"
#include "Luau/ToString.h"
#include "Luau/TxnLog.h"
#include "Luau/TypePack.h"
#include "Luau/TypeUtils.h"

#include <algorithm>

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
std::pair<std::string, std::string> get_call_details(const Luau::AutocompleteEntry& entry, const Luau::FunctionType& function);
std::optional<Luau::ModuleName> get_type_module(Luau::TypeId type);
std::optional<Luau::ModuleName> get_property_module(const Luau::Module& module, const std::vector<Luau::AstNode*>& ancestry);
std::optional<json> get_attribute_items(const textdocument& document, Luau::Position position);

const std::vector<std::pair<std::string_view, std::string_view>> relevant_attributes = {
	{"native", "Compile this function to native code"},
	{"deprecated", "Warn about every use of this function"},
};

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

	if (params.value("/context/triggerCharacter"_json_pointer, "") == "\n")
	{
		invoke_end_completion(params);
		return {{"isIncomplete", false}, {"items", items}};
	}

	if (std::optional<json> attributes = priv::get_attribute_items(*document, document->to_luau_position(params["position"])))
	{
		return {{"isIncomplete", false}, {"items", *attributes}};
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
	std::optional<Luau::ModuleName> property_module;

	if (settings.features.documentation)
	{
		if (Luau::ModulePtr module = get_module(name, true))
		{
			property_module = priv::get_property_module(*module, completion.ancestry);
		}
	}

	for (const auto& [label, entry] : completion.entryMap)
	{
		json item = {
			{"label", label},
			{"kind", static_cast<int>(priv::get_kind(entry))},
			{"sortText", priv::get_sort_text(label, entry)},
		};

		if (entry.deprecated)
		{
			item["tags"] = {1};
		}

		if (settings.features.documentation)
		{
			std::optional<std::string> markdown = entry.documentationSymbol ? docs.get_markdown(*entry.documentationSymbol) : std::nullopt;
			const Luau::Property* property = entry.prop ? *entry.prop : nullptr;

			if (!markdown && property != nullptr)
			{
				Luau::ModuleName module_name = property_module.value_or(name);

				if (property->location)
				{
					markdown = get_comment_documentation(module_name, *property->location);
				}
				else if (property->typeLocation)
				{
					markdown = get_comment_documentation(module_name, *property->typeLocation);
				}
			}

			if (!markdown && entry.type)
			{
				markdown = get_type_documentation(*entry.type);
			}

			if (markdown)
			{
				item["documentation"] = protocol::make_markdown(*markdown);
			}
		}

		const Luau::FunctionType* function = entry.type ? Luau::get<Luau::FunctionType>(Luau::follow(*entry.type)) : nullptr;
		bool callable = function != nullptr && entry.kind != Luau::AutocompleteEntryKind::GeneratedFunction &&
			entry.kind != Luau::AutocompleteEntryKind::Type;

		if (entry.type)
		{
			item["detail"] = Luau::toString(Luau::follow(*entry.type));
		}

		if (callable)
		{
			auto [details, snippet] = priv::get_call_details(entry, *function);
			item["labelDetails"] = {{"detail", details}};

			if (entry.parens != Luau::ParenthesesRecommendation::None && !entry.insertText)
			{
				item["insertText"] = label + snippet + "$0";
				item["insertTextFormat"] = 2;
				item["command"] = {{"title", "Trigger Signature Help"}, {"command", "editor.action.triggerParameterHints"}};
			}
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
		else if (!item.contains("insertText") && entry.parens != Luau::ParenthesesRecommendation::None)
		{
			item["insertText"] = label + "()";
		}

		items.push_back(item);
	}

	if (completion.context == Luau::AutocompleteContext::Expression || completion.context == Luau::AutocompleteContext::Statement)
	{
		std::set<std::string> taken;

		for (const auto& [label, entry] : completion.entryMap)
		{
			taken.insert(label);
		}

		for (json& item : get_auto_imports(name, *document, taken))
		{
			items.push_back(std::move(item));
		}
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

std::optional<json> get_attribute_items(const textdocument& document, Luau::Position position)
{
	std::vector<std::string> lines = stdx::split(document.get_text(), '\n');

	if (position.line >= lines.size())
	{
		return std::nullopt;
	}

	std::string before = lines[position.line].substr(0, std::min<size_t>(position.column, lines[position.line].size()));
	size_t at = before.rfind('@');

	if (at == std::string::npos || before.find("--") < at)
	{
		return std::nullopt;
	}

	size_t quotes = std::count(before.begin(), before.begin() + static_cast<std::ptrdiff_t>(at), '"') +
		std::count(before.begin(), before.begin() + static_cast<std::ptrdiff_t>(at), '\'') +
		std::count(before.begin(), before.begin() + static_cast<std::ptrdiff_t>(at), '`');

	if (quotes % 2 != 0)
	{
		return std::nullopt;
	}

	std::string rest = before.substr(at + 1);
	bool bracket = !rest.empty() && rest[0] == '[';
	std::string word = bracket ? rest.substr(rest.find_last_of("[,") + 1) : rest;
	word = stdx::trim(word);

	if (bracket && rest.find(']') != std::string::npos)
	{
		return std::nullopt;
	}

	if (!std::all_of(word.begin(), word.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }))
	{
		return std::nullopt;
	}

	json items = json::array();

	for (const auto& [name, detail] : relevant_attributes)
	{
		items.push_back(
			{{"label", name},
			 {"kind", static_cast<int>(enumitem_completion_kind::Keyword)},
			 {"detail", detail},
			 {"sortText", "0" + std::string(name)}});
	}

	if (bracket)
	{
		items.push_back({
			{"label", "deprecated { use, reason }"},
			{"kind", static_cast<int>(enumitem_completion_kind::Snippet)},
			{"detail", "Warn about every use and say what to use instead"},
			{"filterText", "deprecated"},
			{"sortText", "1deprecated"},
			{"insertText", "deprecated { use = \"$1\", reason = \"$2\" }"},
			{"insertTextFormat", 2},
		});
	}

	return items;
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

std::pair<std::string, std::string> get_call_details(const Luau::AutocompleteEntry& entry, const Luau::FunctionType& function)
{
	std::string details = "(";
	std::string snippet = "(";
	bool comma = false;
	size_t index = 0;
	size_t stop = 1;
	auto [required, maximum] = Luau::getParameterExtents(Luau::TxnLog::empty(), function.argTypes, true);

	for (Luau::TypeId argument : function.argTypes)
	{
		if (Luau::get<Luau::UnknownType>(Luau::follow(argument)) != nullptr)
		{
			++required;
		}
	}

	Luau::TypePackIterator it = Luau::begin(function.argTypes);

	for (; it != Luau::end(function.argTypes); ++it, ++index)
	{
		if (index == 0 && entry.indexedWithSelf)
		{
			continue;
		}

		std::string name = index < function.argNames.size() && function.argNames[index] ? function.argNames[index]->name : "_";
		bool fill = index < required;

		if (comma)
		{
			details += ", ";
			snippet += fill ? ", " : "";
		}

		details += name;

		if (fill)
		{
			snippet += "${" + std::to_string(stop++) + ":" + name + "}";
		}

		comma = true;
	}

	if (std::optional<Luau::TypePackId> tail = it.tail(); tail && !Luau::isEmpty(*tail))
	{
		const Luau::VariadicTypePack* variadic = Luau::get<Luau::VariadicTypePack>(Luau::follow(*tail));

		if (variadic == nullptr || !variadic->hidden)
		{
			details += (comma ? ", " : "") + Luau::toString(*tail);
		}
	}

	if (entry.parens == Luau::ParenthesesRecommendation::CursorInside && snippet == "(")
	{
		snippet += "$1";
	}

	return {details + ")", snippet + ")"};
}

std::optional<Luau::ModuleName> get_type_module(Luau::TypeId type)
{
	Luau::TypeId followed = Luau::follow(type);

	if (const Luau::TableType* table = Luau::get<Luau::TableType>(followed); table != nullptr && !table->definitionModuleName.empty())
	{
		return table->definitionModuleName;
	}

	if (const Luau::MetatableType* metatable = Luau::get<Luau::MetatableType>(followed))
	{
		return get_type_module(metatable->table);
	}

	if (const Luau::IntersectionType* intersection = Luau::get<Luau::IntersectionType>(followed))
	{
		for (Luau::TypeId part : intersection->parts)
		{
			if (std::optional<Luau::ModuleName> module = get_type_module(part))
			{
				return module;
			}
		}
	}

	if (const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(followed);
		extern_type != nullptr && !extern_type->definitionModuleName.empty())
	{
		return extern_type->definitionModuleName;
	}

	return std::nullopt;
}

std::optional<Luau::ModuleName> get_property_module(const Luau::Module& module, const std::vector<Luau::AstNode*>& ancestry)
{
	for (auto it = ancestry.rbegin(); it != ancestry.rend(); ++it)
	{
		Luau::AstExprIndexName* index = (*it)->as<Luau::AstExprIndexName>();

		if (index == nullptr)
		{
			continue;
		}

		const Luau::TypeId* owner = module.astTypes.find(index->expr);

		if (owner != nullptr)
		{
			return get_type_module(*owner);
		}
	}

	return std::nullopt;
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
