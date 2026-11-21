#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Luau/Config.h"
#include "Luau/ConfigResolver.h"
#include "Luau/FileResolver.h"
#include "Luau/Frontend.h"
#include "Luau/TypeArena.h"

#include <nlohmann/json.hpp>

namespace lunaut
{

using json = nlohmann::json;

struct nothing
{
};

template <typename T>
struct result
{
	bool state = false;
	T value{};
	std::string message;

	static result ok(T value) { return result{true, std::move(value), {}}; }

	static result fail(std::string message) { return result{false, T{}, std::move(message)}; }

	explicit operator bool() const { return state; }

	const T& unpack() const { return value; }
};

enum class enumitem_message_type : int
{
	Error = 1,
	Warning = 2,
	Info = 3,
	Log = 4,
};

enum class enumitem_diagnostic_severity : int
{
	Error = 1,
	Warning = 2,
	Information = 3,
	Hint = 4,
};

enum class enumitem_completion_kind : int
{
	Text = 1,
	Method = 2,
	Function = 3,
	Field = 5,
	Variable = 6,
	Class = 7,
	Interface = 8,
	Module = 9,
	Property = 10,
	Keyword = 14,
	Snippet = 15,
	File = 17,
	Folder = 19,
	Constant = 21,
	TypeParameter = 25,
};

enum class enumitem_symbol_kind : int
{
	File = 1,
	Module = 2,
	Class = 5,
	Method = 6,
	Property = 7,
	Field = 8,
	Function = 12,
	Variable = 13,
	Constant = 14,
	Struct = 23,
	TypeParameter = 26,
};

enum class enumitem_error_code : int
{
	ParseError = -32700,
	InvalidRequest = -32600,
	MethodNotFound = -32601,
	InvalidParams = -32602,
	InternalError = -32603,
	ServerNotInitialized = -32002,
	RequestFailed = -32803,
	RequestCancelled = -32800,
};

enum class enumitem_file_change : int
{
	Created = 1,
	Changed = 2,
	Deleted = 3,
};

enum class enumitem_server_state
{
	Uninitialized,
	Running,
	ShuttingDown,
	Exited,
};

enum class enumitem_platform_variant
{
	Auto,
	Standard,
	Roblox,
};

enum class enumitem_definition_source
{
	Config,
	Cli,
	Discovered,
	Roblox,
};

enum class enumitem_token_type : int
{
	Namespace = 0,
	Type = 1,
	Class = 2,
	Enum = 3,
	Interface = 4,
	Struct = 5,
	TypeParameter = 6,
	Parameter = 7,
	Variable = 8,
	Property = 9,
	EnumMember = 10,
	Event = 11,
	Function = 12,
	Method = 13,
};

enum class enumitem_token_modifier : int
{
	Declaration = 1,
	Readonly = 2,
	DefaultLibrary = 4,
	Deprecated = 8,
};

enum class enumitem_parameter_hints
{
	None,
	Literals,
	All,
};

enum class enumitem_import_keyword
{
	Local,
	Const,
};

enum class enumitem_fflag_source
{
	Default,
	EnableByDefault,
	Sync,
	NewSolver,
	Override,
	Cli,
};

namespace enum_message_type
{
std::string_view get_name(enumitem_message_type item);
}

namespace enum_platform_variant
{
std::string_view get_name(enumitem_platform_variant item);
std::optional<enumitem_platform_variant> from_name(std::string_view name);
}

namespace enum_definition_source
{
std::string_view get_name(enumitem_definition_source item);
}

namespace enum_parameter_hints
{
std::string_view get_name(enumitem_parameter_hints item);
std::optional<enumitem_parameter_hints> from_name(std::string_view name);
}

namespace enum_import_keyword
{
std::string_view get_name(enumitem_import_keyword item);
std::optional<enumitem_import_keyword> from_name(std::string_view name);
}

namespace enum_fflag_source
{
std::string_view get_name(enumitem_fflag_source item);
}

struct definitioninfo
{
	std::string path;
	std::string name;
	std::optional<int> priority;
	bool show_as_module = false;
	enumitem_definition_source source = enumitem_definition_source::Config;
};

struct fflaginfo
{
	bool enable_by_default = true;
	bool sync = true;
	bool enable_new_solver = false;
	std::map<std::string, std::string> override;
};

struct robloxinfo
{
	bool download_types = true;
	std::string security_level = "PluginSecurity";
	int types_priority = 1000;
};

struct rojoinfo
{
	bool enabled = true;
	std::string project_file = "default.project.json";
	std::string sourcemap_file = "sourcemap.json";
	bool autogenerate = true;
	bool include_non_scripts = true;
	bool strict_datamodel_types = false;
	std::string executable = "rojo";
};

struct inlayhintsinfo
{
	bool variable_types = true;
	enumitem_parameter_hints parameter_names = enumitem_parameter_hints::Literals;
	bool parameter_types = false;
	bool function_return_types = true;
	bool hide_error_types = true;
	bool hide_matching_names = true;
	int max_length = 50;
	bool insertable = true;
	bool auto_require = true;
};

struct featuresinfo
{
	bool documentation = true;
	bool auto_imports = true;
	enumitem_import_keyword import_keyword = enumitem_import_keyword::Local;
	bool semantic_tokens = true;
	bool colors = true;
	bool autocomplete_end = true;
	bool convert_quotes = true;
	bool close_interpolation = true;
	bool formatting = true;
	bool code_lens = true;
	std::string stylua = "stylua";
	bool alias_types = true;
	bool refine_types = true;
	bool unknown_error = true;
};

struct diagnosticsinfo
{
	bool lint = true;
	bool include_dependents = true;
	bool workspace = false;
	int delay = 1000;
};

struct analyzerinfo
{
	bool enabled = false;
	bool diagnostics = false;
	std::string path;
};

struct settingsinfo
{
	enumitem_platform_variant platform = enumitem_platform_variant::Auto;
	fflaginfo fflags;
	std::vector<definitioninfo> definitions;
	bool discover_definitions = true;
	robloxinfo roblox;
	rojoinfo rojo;
	diagnosticsinfo diagnostics;
	analyzerinfo analyzer;
	std::vector<std::string> ignore_globs;
	bool analyze_lua_files = true;
	std::string base_luaurc;
	inlayhintsinfo inlay_hints;
	featuresinfo features;
	std::vector<std::string> documentation_files;
	std::vector<std::string> disabled_globals;

	static settingsinfo from_json(const json& data);
	static settingsinfo from_json(const json& data, const settingsinfo& base);
	static result<json> read_vscode_settings(const std::filesystem::path& file);
	static json convert_vscode_settings(const json& data);
	static std::map<std::string, std::string> get_compatible_keys();
	settingsinfo clone(const json& change) const;
	settingsinfo with_project(const std::filesystem::path& root, const std::optional<std::string>& extra_file) const;
	json to_json() const;
};

struct optionsinfo
{
	std::map<std::string, std::string> fflags;
	bool no_flags_enabled = false;
	std::vector<definitioninfo> definitions;
	std::optional<std::string> settings_file;
};

struct declarationinfo
{
	Luau::ModuleName module;
	Luau::Location location;
};

struct loadeddefinitioninfo
{
	std::string name;
	std::string path;
	int priority = 0;
	bool success = false;
	bool show_as_module = false;
	std::string message;
	enumitem_definition_source source = enumitem_definition_source::Config;

	json to_json() const;
};

struct typelinkinfo
{
	std::string name;
	Luau::ModuleName module;
	Luau::Location location;
};

struct typedisplayinfo
{
	std::string text;
	std::string name;
	std::optional<Luau::ModuleName> module;
	bool alias = false;
	bool definition_file = false;
	bool exported = false;
	bool show_as_module = false;
	bool truncated = false;
};

struct fflagstateinfo
{
	std::string name;
	std::string value;
	std::string kind;
	enumitem_fflag_source source = enumitem_fflag_source::Default;

	json to_json() const;
};

namespace stdx
{
void oute(const std::string& message);
void outi(const std::string& message);
bool check(bool condition, const std::string& message);

std::optional<std::string> read_file(const std::filesystem::path& path);
bool write_file(const std::filesystem::path& path, const std::string& contents);
bool set_hidden(const std::filesystem::path& path, bool hidden);

bool starts_with(std::string_view value, std::string_view prefix);
bool ends_with(std::string_view value, std::string_view suffix);
std::string to_lower(std::string value);
std::string trim(std::string_view value);
std::vector<std::string> split(std::string_view value, char separator);

bool has_wildcard(std::string_view pattern);
bool glob_match(std::string_view pattern, std::string_view path);
std::vector<std::filesystem::path> find_files(
	const std::filesystem::path& root, std::string_view pattern, const std::vector<std::string>& ignore_globs);
}

class uri
{
public:
	static std::string to_path(std::string_view value);
	static std::string from_path(std::string_view path);
	static std::string normalize_path(std::string_view path);
	static std::string get_relative(const std::filesystem::path& root, const std::string& path);
};

class textdocument
{
public:
	textdocument() = default;
	textdocument(std::string address, std::string text, int version);

	const std::string& get_uri() const;
	const std::string& get_text() const;
	int get_version() const;

	void apply_changes(const json& changes, int version);
	Luau::Position to_luau_position(const json& position) const;
	json to_lsp_position(const Luau::Position& position) const;
	json to_lsp_range(const Luau::Location& location) const;

private:
	size_t get_offset(const json& position) const;
	void rebuild_lines();

	std::string address;
	std::string text;
	int version = 0;
	std::vector<size_t> lines;
};

class protocol
{
public:
	static json get_capabilities();
	static json get_semantic_legend();
	static json make_markdown(const std::string& value);
	static json make_diagnostic(
		const textdocument& document,
		const Luau::Location& location,
		enumitem_diagnostic_severity severity,
		const std::string& code,
		const std::string& message);
	static json make_type_error(const textdocument& document, const Luau::TypeError& error, Luau::FileResolver* resolver);
	static json make_parse_error(const textdocument& document, const Luau::ParseError& error);
	static json make_lint_warning(const textdocument& document, const Luau::LintWarning& warning, bool fatal);
};

class jsonrpc
{
public:
	jsonrpc(std::istream& input, std::ostream& output);

	result<json> read_message();
	void send_message(const json& message);
	void send_response(const json& id, const json& value);
	void send_error(const json& id, enumitem_error_code code, const std::string& message);
	void send_notification(const std::string& method, const json& params);
	void send_request(const std::string& method, const json& params);

private:
	std::istream& input;
	std::ostream& output;
	std::mutex lock;
	int next_id = 1;
};

class client
{
public:
	virtual ~client() = default;

	virtual void send_log(enumitem_message_type type, const std::string& message) = 0;
	virtual void send_window_message(enumitem_message_type type, const std::string& message) = 0;
	virtual void send_diagnostics(const std::string& address, std::optional<int> version, const json& diagnostics) = 0;

	virtual void send_edit(const json& edit, bool move_cursor) {}
};

class process
{
public:
	static result<std::string> invoke_command(const std::string& command);
	static bool has_executable(const std::string& name);
};

class http
{
public:
	static result<std::string> get_text(const std::string& url);
};

class cache
{
public:
	static std::filesystem::path get_directory();
	static result<std::string> get_or_fetch(const std::string& name, const std::string& url, bool refresh);
	static std::optional<std::filesystem::path> get_file(const std::string& name);
};

struct sourcenode
{
	std::string name;
	std::string class_name;
	std::vector<std::string> file_paths;
	std::vector<std::unique_ptr<sourcenode>> children;
	sourcenode* parent = nullptr;
	std::string virtual_path;
	bool plugin = false;
	mutable std::unordered_map<const Luau::GlobalTypes*, Luau::TypeId> types;

	bool is_script() const;
	std::optional<std::string> get_script_path() const;
	Luau::SourceCode::Type get_source_type() const;
	const sourcenode* find_child(std::string_view child) const;
	const sourcenode* find_ancestor(std::string_view ancestor) const;
	void clear_types() const;
};

class sourcemap
{
public:
	result<nothing> load_from_file(const std::filesystem::path& file, const std::filesystem::path& root);
	result<nothing> load_from_string(const std::string& contents, const std::filesystem::path& root);
	bool merge_plugin(const json& tree);
	void clear_plugin();
	void clear();

	bool is_loaded() const;
	size_t count() const;
	const sourcenode* get_root() const;
	const sourcenode* get_by_virtual_path(const std::string& path) const;
	const sourcenode* get_by_real_path(const std::string& path) const;
	std::optional<std::string> get_real_path(const std::string& virtual_path) const;
	std::optional<std::string> get_virtual_path(const std::string& real_path) const;

	static bool is_virtual_path(const std::string& name);
	static std::optional<std::string> get_parent_path(const std::string& name);

private:
	std::unique_ptr<sourcenode> parse_node(const json& data, sourcenode* parent, const std::filesystem::path& root);
	void add_node(const sourcenode* node);
	void rebuild_maps();
	bool merge_node(sourcenode* node, const json& data);

	std::unique_ptr<sourcenode> root;
	std::unordered_map<std::string, const sourcenode*> virtual_paths;
	std::unordered_map<std::string, const sourcenode*> real_paths;
};

class robloxresolver
{
public:
	using virtualizer = std::function<std::optional<std::string>(const Luau::ModuleName&)>;

	static std::optional<Luau::ModuleInfo> resolve_expression(
		const sourcemap& map, const Luau::ModuleInfo* context, Luau::AstExpr* node, const virtualizer& to_virtual);
	static std::optional<Luau::ModuleInfo> resolve_string(
		const sourcemap& map, const Luau::ModuleInfo* context, const std::string& required);
};

class instancetypes
{
public:
	void apply(Luau::Frontend& frontend, const sourcemap& map, bool strict_datamodel_types);
	void clear(Luau::Frontend& frontend, const sourcemap& map);

private:
	void apply_globals(const Luau::GlobalTypes& globals, const sourcemap& map);
	Luau::TypeId get_node_type(const Luau::GlobalTypes& globals, const sourcenode* node);
	void add_children(const Luau::GlobalTypes& globals, Luau::TypeId type, const sourcenode* node);

	Luau::TypeArena arena;
};

class rojo
{
public:
	static bool is_project(const std::filesystem::path& root, const rojoinfo& info);
	static std::optional<std::string> find_project(const std::filesystem::path& root, const rojoinfo& info);
	static result<nothing> generate_sourcemap(const std::filesystem::path& root, const rojoinfo& info);
};

class crater
{
public:
	static std::vector<std::string> get_package_folders(const std::filesystem::path& root);
	static std::vector<std::string> get_ignore_globs(const std::filesystem::path& root);
};

class documentation
{
public:
	void load(bool roblox, const std::vector<std::filesystem::path>& files, client& owner);
	bool is_loaded() const;
	std::optional<std::string> get_markdown(const std::string& symbol) const;
	std::optional<std::string> get_parameter_markdown(const std::string& symbol, size_t index) const;
	static std::optional<std::string> get_comment_markdown(const Luau::SourceModule& source, const std::string& text, unsigned int line);
	static std::optional<std::string> get_comment_parameter(
		const Luau::SourceModule& source, const std::string& text, unsigned int line, const std::string& name);

private:
	json entries = json::object();
	bool loaded = false;
};

class robloxmagic
{
public:
	static std::optional<json> get_metadata(const std::string& source);
	static void apply(Luau::GlobalTypes& globals, const json& metadata);
};

class robloxtypes
{
public:
	static result<std::string> get_definitions_path(const std::string& security_level, bool refresh);
};

class fflagregistry
{
public:
	static std::vector<fflagstateinfo> apply(
		const fflaginfo& info, const std::map<std::string, std::string>& cli, bool no_flags_enabled, client& owner);
	static std::vector<fflagstateinfo> get_flags();
	static result<nothing> set_flag(const std::string& name, const std::string& value, enumitem_fflag_source source);
	static result<std::map<std::string, std::string>> sync_from_roblox(bool refresh);
};

class definitionloader
{
public:
	static std::vector<definitioninfo> collect(const std::filesystem::path& root, const settingsinfo& settings, bool roblox, client& owner);
	static std::optional<int> get_directive_priority(const std::string& source);
	static std::string get_package_name(const definitioninfo& info, const std::filesystem::path& path);
};

class declarefile
{
public:
	static json make_content();
	static std::filesystem::path get_path(const std::filesystem::path& root);
	static result<std::string> make_file(const std::filesystem::path& root);
	static result<bool> apply_missing(const std::filesystem::path& root);
};

struct symbolinfo
{
	Luau::AstLocal* local = nullptr;
	std::string name;
	bool member = false;
	bool by_property = false;
	Luau::ModuleName owner_module;
	Luau::Location owner_location;
	Luau::Location origin;

	bool is_same_owner(const symbolinfo& other) const;
};

class symbolfinder
{
public:
	static std::optional<symbolinfo> find(const Luau::Module& module, const Luau::SourceModule& source, Luau::Position position);
	static std::optional<symbolinfo> get_member(const Luau::Module& module, const Luau::AstExpr* object, const std::string& name);
	static std::vector<Luau::Location> find_local_uses(const Luau::SourceModule& source, Luau::AstLocal* local);
	static std::vector<Luau::Location> find_member_uses(
		const Luau::Module& module, const Luau::SourceModule& source, const symbolinfo& symbol);
	static std::vector<Luau::Location> find_writes(const Luau::Module& module, const Luau::SourceModule& source, const symbolinfo& symbol);
};

class typenormalizer
{
public:
	static Luau::TypeId strip_refinements(Luau::TypeId type);
	static bool is_refined(Luau::TypeId type);
};

struct namedtypeinfo
{
	std::string name;
	Luau::TypeFun definition;
	std::string display;
};

class typecomparer
{
public:
	static bool is_primitive_like(Luau::TypeId type);
	static bool is_same_type(Luau::TypeId left, Luau::TypeId right);
	static bool is_same_type(Luau::TypeId left, Luau::TypeId right, const Luau::TypeFun& definition);
	static bool may_be_same_type(Luau::TypeId left, Luau::TypeId right);
	static void flatten_intersection(Luau::TypeId type, std::vector<Luau::TypeId>& parts);
	static void collect_origin_modules(Luau::TypeId type, std::set<Luau::ModuleName>& modules);
	static void collect_definition_locations(Luau::TypeId type, std::vector<declarationinfo>& locations);
	static int definition_location_score(
		const Luau::ModuleName& module, const std::optional<Luau::Location>& location, const std::vector<declarationinfo>& locations);
};

class typelookup
{
public:
	static std::optional<namedtypeinfo> get_reference_alias(const Luau::ScopePtr& scope, const Luau::AstType* annotation);
	static std::optional<Luau::Property> find_property(Luau::TypeId owner, const std::string& name, Luau::TypeId& base);
	static std::optional<Luau::ModuleName> get_definition_module(Luau::TypeId type);
	static std::optional<Luau::ModuleName> find_imported_module(const Luau::ScopePtr& scope, const std::string& alias);
	static const Luau::AstExpr* find_initializer(const Luau::SourceModule& source, const Luau::AstLocal* local);
};

class fileresolver;

class requirenode final : public Luau::RequireNode
{
public:
	requirenode(const fileresolver& resolver, std::filesystem::path path, bool requirer);

	std::string getPathComponent() const override;
	std::unique_ptr<Luau::RequireNode> resolvePathToNode(const std::string& path) const override;
	std::vector<std::unique_ptr<Luau::RequireNode>> getChildren() const override;
	std::vector<Luau::RequireAlias> getAvailableAliases() const override;

private:
	const fileresolver& resolver;
	std::filesystem::path path;
	bool requirer = false;
};

class requiresuggester final : public Luau::RequireSuggester
{
public:
	explicit requiresuggester(const fileresolver& resolver);

protected:
	std::unique_ptr<Luau::RequireNode> getNode(const Luau::ModuleName& name) const override;

private:
	const fileresolver& resolver;
};

class fileresolver final
	: public Luau::FileResolver
	, public Luau::ConfigResolver
{
public:
	explicit fileresolver(std::filesystem::path root);

	std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override;
	std::optional<Luau::ModuleInfo> resolveModule(
		const Luau::ModuleInfo* context, Luau::AstExpr* expr, const Luau::TypeCheckLimits& limits) override;
	std::string getHumanReadableModuleName(const Luau::ModuleName& name) const override;
	const Luau::Config& getConfig(const Luau::ModuleName& name, const Luau::TypeCheckLimits& limits) const override;

	Luau::ModuleName get_module_name(const std::string& address) const;
	std::string get_uri(const Luau::ModuleName& name) const;
	std::optional<std::string> get_real_path(const Luau::ModuleName& name) const;
	std::optional<std::string> read_text(const std::string& real_path) const;

	const textdocument* get_document(const std::string& address) const;
	textdocument* get_document(const std::string& address);
	const std::unordered_map<std::string, textdocument>& get_documents() const;
	void set_document(textdocument document);
	void rem_document(const std::string& address);

	void set_sourcemap(const sourcemap* map);
	void set_base_config(const std::filesystem::path& path);
	void set_show_as_modules(std::set<std::string> modules);
	void clear_config_cache();

	std::optional<std::filesystem::path> get_require_target(const std::string& requirer, const std::string& required) const;
	std::vector<Luau::RequireAlias> get_aliases(const std::filesystem::path& directory) const;
	const Luau::Config& get_directory_config(const std::filesystem::path& directory) const;

private:
	std::optional<Luau::ModuleInfo> resolve_string_require(const Luau::ModuleInfo* context, const std::string& required);
	std::string resolve_file_candidate(const std::filesystem::path& base) const;
	void read_config_file(const std::filesystem::path& directory, Luau::Config& config) const;

	std::filesystem::path root;
	std::unordered_map<std::string, textdocument> documents;
	std::set<std::string> show_as_modules;
	const sourcemap* map = nullptr;
	mutable std::unordered_map<std::string, std::unique_ptr<Luau::Config>> configs;
	Luau::Config default_config;
};

class analyzerplugin
{
public:
	~analyzerplugin();

	bool load(const std::string& path);
	void unload();

	bool is_loaded() const;
	const std::string& get_name() const;
	const std::string& get_version() const;
	const std::string& get_error() const;
	const std::string& get_frontend_api() const;
	json to_json() const;
	std::optional<json> check(const std::string& module, const std::string& path, const std::string& source, const json& settings) const;

private:
	using info_function = const void* (*)();
	using check_function = int (*)(const void*, void*);
	using free_function = void (*)(const void*);

	void* handle = nullptr;
	info_function get_info = nullptr;
	check_function run_check = nullptr;
	free_function release = nullptr;
	bool enabled = false;
	bool loaded = false;
	std::string name;
	std::string version;
	std::string error;
	std::string frontend_api = "unknown";
};

class workspace
{
public:
	workspace(client& owner, std::filesystem::path root, settingsinfo settings);

	void initialize();
	bool is_ready() const;
	bool is_roblox() const;
	bool is_inside(const std::string& address) const;
	const std::filesystem::path& get_root() const;
	const settingsinfo& get_settings() const;
	const std::vector<loadeddefinitioninfo>& get_definitions() const;
	json get_status() const;
	json get_type_stats();

	void open_document(const std::string& address, const std::string& text, int version);
	void update_document(const std::string& address, const json& changes, int version);
	void close_document(const std::string& address);
	void save_document(const std::string& address);
	void apply_watched_changes(const json& changes);
	void set_settings(settingsinfo next);
	result<nothing> rebuild_sourcemap(bool regenerate);

	json get_diagnostics(const std::string& address);
	void publish_diagnostics(const std::string& address);
	void flush_diagnostics();
	std::optional<std::chrono::steady_clock::time_point> diagnostics_deadline() const;
	bool has_pending_diagnostics() const;
	json get_hover(const json& params);
	json get_completion(const json& params);
	json get_definition(const json& params);
	json get_declaration(const json& params);
	json get_type_definition(const json& params);
	json get_implementations(const json& params);
	json get_document_symbols(const json& params);
	json get_signature_help(const json& params);
	json get_references(const json& params);
	json get_prepare_rename(const json& params);
	json get_rename(const json& params);
	json get_inlay_hints(const json& params);
	std::optional<std::string> get_type_alias_name(
		Luau::TypeId type,
		const Luau::Module& module,
		const Luau::ScopePtr& scope,
		const Luau::AstType* annotation,
		const Luau::AstExpr* initializer = nullptr) const;
	typedisplayinfo get_type_display(
		Luau::TypeId type,
		const Luau::Module& module,
		const Luau::ScopePtr& scope,
		const Luau::AstType* annotation,
		const Luau::AstExpr* initializer = nullptr,
		size_t max_length = 0,
		std::vector<typelinkinfo>* links = nullptr) const;
	typedisplayinfo get_type_display(
		Luau::TypePackId pack, const Luau::Module& module, const Luau::ScopePtr& scope, size_t max_length = 0) const;
	std::string get_import_symbol(const Luau::ModuleName& name) const;
	std::optional<std::string> get_import_statement(
		const Luau::ModuleName& from, const Luau::ModuleName& target, const std::string& symbol = std::string()) const;
	unsigned int get_import_line(const Luau::ModuleName& name, const textdocument& document);
	json get_semantic_tokens(const json& params);
	json get_code_actions(const json& params);
	json get_document_colors(const json& params);
	json get_color_presentations(const json& params);
	json get_folding_ranges(const json& params);
	json get_document_highlights(const json& params);
	json get_document_links(const json& params);
	json get_workspace_symbols(const json& params);
	json get_call_hierarchy_items(const json& params);
	json get_incoming_calls(const json& params);
	json get_outgoing_calls(const json& params);
	json get_on_type_edits(const json& params);
	json get_formatting(const json& params);
	json get_code_lenses(const json& params);
	json resolve_code_lens(const json& lens);
	json get_selection_ranges(const json& params);
	json get_inline_values(const json& params);
	json get_monikers(const json& params);
	json get_type_hierarchy_items(const json& params);
	json get_supertypes(const json& params);
	json get_subtypes(const json& params);
	void invoke_end_completion(const json& params);
	json get_end_edits(const json& params);
	std::optional<Luau::TypeId> get_initializer_type(const Luau::Module& module, const Luau::AstExpr* initializer) const;
	void publish_workspace_diagnostics();
	void apply_plugin_tree(const json& tree);

private:
	void index_workspace();
	json get_auto_imports(const Luau::ModuleName& name, const textdocument& document, const std::set<std::string>& taken);
	std::optional<json> make_import_edit(const Luau::ModuleName& name, const textdocument& document, const std::string& symbol);
	std::optional<std::string> get_comment_documentation(const Luau::ModuleName& name, const Luau::Location& location);
	std::optional<std::string> get_annotation_text(const Luau::ModuleName& name, const Luau::AstType* annotation) const;
	std::optional<Luau::TypeFun> get_annotation_alias(
		const Luau::ModuleName& name,
		const Luau::Module* module,
		const Luau::ScopePtr& scope,
		const Luau::AstType* annotation,
		std::string& alias,
		std::string* display = nullptr) const;
	std::optional<std::string> get_type_documentation(Luau::TypeId type);
	std::optional<Luau::Location> find_property_implementation(const Luau::ModuleName& name, const std::string& property) const;
	std::string render_type_display(
		Luau::TypeId type,
		const Luau::ModuleName& module_name,
		const Luau::ScopePtr& scope,
		size_t max_length,
		bool named_function = false,
		bool hide_self = false,
		const std::string* return_alias = nullptr,
		std::vector<typelinkinfo>* links = nullptr) const;
	std::string render_type_pack_display(
		Luau::TypePackId pack, const Luau::ModuleName& module_name, const Luau::ScopePtr& scope, size_t max_length) const;
	std::string format_alias_display(const Luau::ModuleName& module, const std::string& name) const;
	std::optional<Luau::TypeFun> find_type_alias(
		Luau::TypeId type,
		Luau::ModuleName& module,
		std::string& name,
		const std::optional<Luau::ModuleName>& preferred = std::nullopt) const;
	std::optional<Luau::TypeFun> find_display_alias(
		Luau::TypeId type, const std::optional<Luau::ModuleName>& preferred, std::string& name, std::string& display) const;
	std::optional<Luau::TypeFun> find_expression_alias(
		const Luau::Module& module,
		const Luau::ScopePtr& scope,
		const Luau::AstExpr* expression,
		std::string& name,
		std::string& display) const;
	std::optional<Luau::TypeFun> find_return_alias(Luau::TypeId type, std::string& name) const;
	std::optional<Luau::TypeFun> find_function_return_alias(
		const Luau::FunctionType& function, Luau::ModuleName& module, std::string& name, std::string* display = nullptr) const;
	std::optional<Luau::TypeFun> find_annotation_return_alias(
		const Luau::ModuleName& module,
		const Luau::Location& location,
		const std::string& property,
		std::string& name,
		std::string* display = nullptr) const;
	std::optional<Luau::ModuleName> find_definition_module(Luau::TypeId type) const;
	std::optional<Luau::ModuleName> find_definition_module(const Luau::Location& location, const std::string& name = std::string()) const;
	std::vector<std::pair<Luau::ModuleName, Luau::Location>> find_references(
		const Luau::ModuleName& name,
		Luau::Position position,
		std::optional<Luau::Location>& origin,
		std::optional<Luau::Location>& declaration);
	void register_types();
	void load_definitions();
	void load_analyzer();
	void apply_disabled_globals(Luau::GlobalTypes& globals);
	Luau::CheckResult check_module(const Luau::ModuleName& name, bool for_autocomplete);
	Luau::ModulePtr get_module(const Luau::ModuleName& name, bool for_autocomplete) const;
	bool is_definition_file(const std::string& path) const;
	bool is_ignored(const std::string& path) const;
	std::optional<textdocument> get_text_for(const Luau::ModuleName& name) const;
	json get_location(const Luau::ModuleName& name, const Luau::Location& location) const;
	json find_symbol_location(const json& params, bool declaration);
	json get_declared_type_definition(const json& params);
	std::optional<declarationinfo> get_alias_declaration(
		const Luau::ModuleName& name, const Luau::Scope& scope, const Luau::AstTypeReference& reference) const;
	std::optional<declarationinfo> get_type_declaration(Luau::TypeId type) const;
	json make_type_hierarchy_item(Luau::TypeId type, const std::string& origin) const;

	client& owner;
	std::filesystem::path root;
	settingsinfo settings;
	fileresolver resolver;
	Luau::Frontend frontend;
	sourcemap map;
	instancetypes instances;
	documentation docs;
	json roblox_metadata = json::object();
	std::vector<loadeddefinitioninfo> definitions;
	std::set<std::string> definition_paths;
	std::unordered_map<std::string, declarationinfo> declared_globals;
	std::unordered_map<std::string, declarationinfo> declared_types;
	std::unordered_map<std::string, std::string> definition_module_paths;
	std::unordered_map<std::string, Luau::SourceModule> definition_sources;
	std::set<std::string> definition_show_as_module;
	std::unordered_map<std::string, Luau::ModulePtr> definition_modules;
	analyzerplugin analyzer;
	bool analyzer_reported = false;
	std::set<std::string> pending;
	std::chrono::steady_clock::time_point last_change;
	bool change_pending = false;
	json plugin_tree = nullptr;
	bool ready = false;
	bool roblox = false;
};

class server final : public client
{
public:
	server(jsonrpc& rpc, optionsinfo options);

	int run();

	void send_log(enumitem_message_type type, const std::string& message) override;
	void send_window_message(enumitem_message_type type, const std::string& message) override;
	void send_diagnostics(const std::string& address, std::optional<int> version, const json& diagnostics) override;
	void send_edit(const json& edit, bool move_cursor) override;

	json on_initialize(const json& params);
	json on_shutdown(const json& params);
	json on_hover(const json& params);
	json on_completion(const json& params);
	json on_definition(const json& params);
	json on_declaration(const json& params);
	json on_type_definition(const json& params);
	json on_implementation(const json& params);
	json on_document_symbol(const json& params);
	json on_signature_help(const json& params);
	json on_references(const json& params);
	json on_prepare_rename(const json& params);
	json on_rename(const json& params);
	json on_inlay_hint(const json& params);
	json on_semantic_tokens(const json& params);
	json on_code_action(const json& params);
	json on_document_color(const json& params);
	json on_color_presentation(const json& params);
	json on_folding_range(const json& params);
	json on_document_highlight(const json& params);
	json on_document_link(const json& params);
	json on_workspace_symbol(const json& params);
	json on_prepare_call_hierarchy(const json& params);
	json on_incoming_calls(const json& params);
	json on_outgoing_calls(const json& params);
	json on_on_type_formatting(const json& params);
	json on_formatting(const json& params);
	json on_range_formatting(const json& params);
	json on_code_lens(const json& params);
	json on_code_lens_resolve(const json& params);
	json on_selection_range(const json& params);
	json on_inline_value(const json& params);
	json on_moniker(const json& params);
	json on_prepare_type_hierarchy(const json& params);
	json on_supertypes(const json& params);
	json on_subtypes(const json& params);
	json on_lunaut_status(const json& params);
	json on_lunaut_type_stats(const json& params);
	json on_lunaut_fflags(const json& params);
	json on_lunaut_definitions(const json& params);
	json on_lunaut_sourcemap(const json& params);
	json on_execute_command(const json& params);
	json invoke_regenerate_sourcemap(const json& arguments);
	json invoke_show_status(const json& arguments);

	void on_initialized(const json& params);
	void on_exit(const json& params);
	void on_did_open(const json& params);
	void on_did_change(const json& params);
	void on_did_close(const json& params);
	void on_did_save(const json& params);
	void on_did_change_configuration(const json& params);
	void on_did_change_watched_files(const json& params);
	void on_did_change_workspace_folders(const json& params);
	void on_plugin_full(const json& params);
	void on_plugin_clear(const json& params);
	void on_ignore(const json& params);

private:
	void process_message(const json& message);
	void read_messages();
	std::optional<json> take_message(std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);
	void add_workspace(const std::string& address);
	void register_watchers();
	workspace& get_workspace(const std::string& address);
	settingsinfo get_settings_for(const std::filesystem::path& root) const;

	jsonrpc& rpc;
	optionsinfo options;
	enumitem_server_state state = enumitem_server_state::Uninitialized;
	json client_settings = json::object();
	json client_capabilities = json::object();
	std::vector<std::unique_ptr<workspace>> workspaces;
	std::unique_ptr<workspace> null_workspace;
	std::vector<fflagstateinfo> fflags;
	int exit_code = 1;
	bool extension_client = false;
	std::deque<json> queue;
	std::mutex queue_lock;
	std::condition_variable queue_ready;
	std::set<std::string> cancelled;
	std::atomic<bool> input_closed = false;
};

class console final : public client
{
public:
	explicit console(bool verbose, bool json_output = false);

	int invoke_analyze(const std::vector<std::string>& files, const optionsinfo& options);
	int invoke_stats(const std::vector<std::string>& files, const optionsinfo& options);

	void send_log(enumitem_message_type type, const std::string& message) override;
	void send_window_message(enumitem_message_type type, const std::string& message) override;
	void send_diagnostics(const std::string& address, std::optional<int> version, const json& diagnostics) override;

private:
	bool verbose = false;
	bool json_output = false;
	bool diagnostics_output = true;
	size_t errors = 0;
	size_t warnings = 0;
};

}
