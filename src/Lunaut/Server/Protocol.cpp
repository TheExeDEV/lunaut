#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Error.h"
#include "Luau/LinterConfig.h"
#include "Luau/ParseResult.h"

namespace lunaut
{
namespace
{
namespace priv
{

constexpr std::string_view source = "lunaut";

}
}

json protocol::get_capabilities()
{
	return {
		{"positionEncoding", "utf-16"},
		{"textDocumentSync", {{"openClose", true}, {"change", 2}, {"save", {{"includeText", false}}}}},
		{"hoverProvider", true},
		{"completionProvider", {{"triggerCharacters", {".", ":", "\"", "'", "/", "@"}}, {"resolveProvider", false}}},
		{"signatureHelpProvider", {{"triggerCharacters", {"(", ","}}}},
		{"definitionProvider", true},
		{"declarationProvider", true},
		{"typeDefinitionProvider", true},
		{"implementationProvider", true},
		{"documentSymbolProvider", true},
		{"referencesProvider", true},
		{"inlayHintProvider", true},
		{"semanticTokensProvider", {{"legend", get_semantic_legend()}, {"full", true}, {"range", false}}},
		{"codeActionProvider", {{"codeActionKinds", {"quickfix"}}}},
		{"executeCommandProvider", {{"commands", {"lunaut.server.regenerateSourcemap", "lunaut.server.showStatus"}}}},
		{"colorProvider", true},
		{"foldingRangeProvider", true},
		{"documentHighlightProvider", true},
		{"documentLinkProvider", {{"resolveProvider", false}}},
		{"workspaceSymbolProvider", true},
		{"callHierarchyProvider", true},
		{"documentOnTypeFormattingProvider", {{"firstTriggerCharacter", "{"}, {"moreTriggerCharacter", json::array({"\n"})}}},
		{"documentFormattingProvider", true},
		{"documentRangeFormattingProvider", true},
		{"codeLensProvider", {{"resolveProvider", true}}},
		{"selectionRangeProvider", true},
		{"inlineValueProvider", true},
		{"monikerProvider", true},
		{"typeHierarchyProvider", true},
		{"renameProvider", {{"prepareProvider", true}}},
		{"workspace", {{"workspaceFolders", {{"supported", true}, {"changeNotifications", true}}}}},
	};
}

json protocol::get_semantic_legend()
{
	return {
		{"tokenTypes",
		 {
			 "namespace",
			 "type",
			 "class",
			 "enum",
			 "interface",
			 "struct",
			 "typeParameter",
			 "parameter",
			 "variable",
			 "property",
			 "enumMember",
			 "event",
			 "function",
			 "method",
		 }},
		{"tokenModifiers", {"declaration", "readonly", "defaultLibrary", "deprecated"}},
	};
}

json protocol::make_markdown(const std::string& value)
{
	return {{"kind", "markdown"}, {"value", value}};
}

json protocol::make_diagnostic(
	const textdocument& document,
	const Luau::Location& location,
	enumitem_diagnostic_severity severity,
	const std::string& code,
	const std::string& message)
{
	return {
		{"range", document.to_lsp_range(location)},
		{"severity", static_cast<int>(severity)},
		{"code", code},
		{"source", priv::source},
		{"message", message},
	};
}

json protocol::make_type_error(const textdocument& document, const Luau::TypeError& error, Luau::FileResolver* resolver)
{
	std::string message = Luau::toString(error, Luau::TypeErrorToStringOptions{resolver});
	std::string code = Luau::get<Luau::SyntaxError>(error) != nullptr ? "SyntaxError" : "TypeError";

	return make_diagnostic(document, error.location, enumitem_diagnostic_severity::Error, code, message);
}

json protocol::make_parse_error(const textdocument& document, const Luau::ParseError& error)
{
	return make_diagnostic(document, error.getLocation(), enumitem_diagnostic_severity::Error, "SyntaxError", error.getMessage());
}

json protocol::make_lint_warning(const textdocument& document, const Luau::LintWarning& warning, bool fatal)
{
	enumitem_diagnostic_severity severity = fatal ? enumitem_diagnostic_severity::Error : enumitem_diagnostic_severity::Warning;
	std::string name = Luau::LintWarning::getName(warning.code);
	json diagnostic = make_diagnostic(document, warning.location, severity, name, name + ": " + warning.text);

	if (warning.code == Luau::LintWarning::Code_LocalUnused || warning.code == Luau::LintWarning::Code_FunctionUnused ||
		warning.code == Luau::LintWarning::Code_ImportUnused)
	{
		diagnostic["tags"] = {1};
	}

	if (warning.code == Luau::LintWarning::Code_DeprecatedGlobal || warning.code == Luau::LintWarning::Code_DeprecatedApi)
	{
		diagnostic["tags"] = {2};
	}

	return diagnostic;
}

}
