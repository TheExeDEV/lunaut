#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/BuiltinDefinitions.h"
#include "Luau/Common.h"
#include "Luau/ConstraintSolver.h"
#include "Luau/LValue.h"
#include "Luau/TypeInfer.h"

#include <algorithm>

LUAU_FASTFLAG(LuauCyclicRequireTypeInference)

namespace lunaut
{
namespace
{
namespace priv
{

constexpr std::string_view metadata_prefix = "--#METADATA#";

std::optional<Luau::TypeId> lookup_class(const Luau::Scope& scope, const std::string& name);
std::optional<std::string> get_string_argument(const Luau::AstExprCall& call, size_t index);
void report_old(Luau::TypeChecker& checker, const Luau::Location& location, Luau::TypeErrorData data);
void report_new(const Luau::MagicFunctionCallContext& context, const Luau::Location& location, Luau::TypeErrorData data);
void bind_result(const Luau::MagicFunctionCallContext& context, Luau::TypeId type);

struct magic_is_a final : Luau::MagicFunction
{
	std::optional<Luau::WithPredicate<Luau::TypePackId>> handleOldSolver(
		Luau::TypeChecker& checker,
		const Luau::ScopePtr& scope,
		const Luau::AstExprCall& call,
		Luau::WithPredicate<Luau::TypePackId> predicate) override;
	bool infer(const Luau::MagicFunctionCallContext& context) override;
	void refine(const Luau::MagicRefinementContext& context) override;
};

struct magic_clone final : Luau::MagicFunction
{
	std::optional<Luau::WithPredicate<Luau::TypePackId>> handleOldSolver(
		Luau::TypeChecker& checker,
		const Luau::ScopePtr& scope,
		const Luau::AstExprCall& call,
		Luau::WithPredicate<Luau::TypePackId> predicate) override;
	bool infer(const Luau::MagicFunctionCallContext& context) override;
};

struct magic_from_existing final : Luau::MagicFunction
{
	std::optional<Luau::WithPredicate<Luau::TypePackId>> handleOldSolver(
		Luau::TypeChecker& checker,
		const Luau::ScopePtr& scope,
		const Luau::AstExprCall& call,
		Luau::WithPredicate<Luau::TypePackId> predicate) override;
	bool infer(const Luau::MagicFunctionCallContext& context) override;
};

struct magic_find_class final : Luau::MagicFunction
{
	std::optional<Luau::WithPredicate<Luau::TypePackId>> handleOldSolver(
		Luau::TypeChecker& checker,
		const Luau::ScopePtr& scope,
		const Luau::AstExprCall& call,
		Luau::WithPredicate<Luau::TypePackId> predicate) override;
	bool infer(const Luau::MagicFunctionCallContext& context) override;
};

struct magic_lookup final : Luau::MagicFunction
{
	magic_lookup(std::vector<std::string> allowed, std::string error);

	std::optional<Luau::WithPredicate<Luau::TypePackId>> handleOldSolver(
		Luau::TypeChecker& checker,
		const Luau::ScopePtr& scope,
		const Luau::AstExprCall& call,
		Luau::WithPredicate<Luau::TypePackId> predicate) override;
	bool infer(const Luau::MagicFunctionCallContext& context) override;

	std::vector<std::string> allowed;
	std::string error;
};

using magic_factory = std::shared_ptr<Luau::MagicFunction> (*)();

const std::vector<std::pair<std::string_view, magic_factory>> relevant_object_magic = {
	{"IsA", []() -> std::shared_ptr<Luau::MagicFunction> { return std::make_shared<magic_is_a>(); }},
};

const std::vector<std::pair<std::string_view, magic_factory>> relevant_instance_magic = {
	{"Clone", []() -> std::shared_ptr<Luau::MagicFunction> { return std::make_shared<magic_clone>(); }},
	{"FindFirstChildWhichIsA", []() -> std::shared_ptr<Luau::MagicFunction> { return std::make_shared<magic_find_class>(); }},
	{"FindFirstChildOfClass", []() -> std::shared_ptr<Luau::MagicFunction> { return std::make_shared<magic_find_class>(); }},
	{"FindFirstAncestorWhichIsA", []() -> std::shared_ptr<Luau::MagicFunction> { return std::make_shared<magic_find_class>(); }},
	{"FindFirstAncestorOfClass", []() -> std::shared_ptr<Luau::MagicFunction> { return std::make_shared<magic_find_class>(); }},
};

void attach(Luau::TableType::Props& props, std::string_view name, const std::shared_ptr<Luau::MagicFunction>& magic);
void apply_instance(Luau::GlobalTypes& globals);
void apply_constructors(Luau::GlobalTypes& globals, const json& metadata);
void apply_enums(Luau::GlobalTypes& globals);
std::vector<std::string> get_list(const json& metadata, const char* key);

}
}

std::optional<json> robloxmagic::get_metadata(const std::string& source)
{
	if (!stdx::starts_with(source, priv::metadata_prefix))
	{
		return std::nullopt;
	}

	size_t end = source.find('\n');
	std::string line =
		source.substr(priv::metadata_prefix.size(), end == std::string::npos ? std::string::npos : end - priv::metadata_prefix.size());

	try
	{
		return json::parse(line);
	}
	catch (const json::exception&)
	{
		return std::nullopt;
	}
}

void robloxmagic::apply(Luau::GlobalTypes& globals, const json& metadata)
{
	priv::apply_instance(globals);
	priv::apply_constructors(globals, metadata);
	priv::apply_enums(globals);
}

namespace
{
namespace priv
{

std::optional<Luau::TypeId> lookup_class(const Luau::Scope& scope, const std::string& name)
{
	std::optional<Luau::TypeFun> found = scope.lookupType(name);

	if (!found || !found->typeParams.empty() || !found->typePackParams.empty())
	{
		return std::nullopt;
	}

	return Luau::follow(found->type);
}

std::optional<std::string> get_string_argument(const Luau::AstExprCall& call, size_t index)
{
	if (call.args.size <= index)
	{
		return std::nullopt;
	}

	Luau::AstExprConstantString* value = call.args.data[index]->as<Luau::AstExprConstantString>();

	if (value == nullptr)
	{
		return std::nullopt;
	}

	return std::string(value->value.data, value->value.size);
}

void report_old(Luau::TypeChecker& checker, const Luau::Location& location, Luau::TypeErrorData data)
{
	checker.reportError(Luau::TypeError{location, std::move(data)});
}

void report_new(const Luau::MagicFunctionCallContext& context, const Luau::Location& location, Luau::TypeErrorData data)
{
	if (FFlag::LuauCyclicRequireTypeInference)
	{
		context.solver->reportError(std::move(data), location, *context.constraint->moduleName);
	}
	else
	{
		context.solver->DEPRECATED_reportError(Luau::TypeError{location, std::move(data)});
	}
}

void bind_result(const Luau::MagicFunctionCallContext& context, Luau::TypeId type)
{
	Luau::asMutable(context.result)->ty.emplace<Luau::BoundTypePack>(context.solver->arena->addTypePack({type}));
}

std::optional<Luau::WithPredicate<Luau::TypePackId>> magic_is_a::handleOldSolver(
	Luau::TypeChecker& checker, const Luau::ScopePtr& scope, const Luau::AstExprCall& call, Luau::WithPredicate<Luau::TypePackId> predicate)
{
	Luau::AstExprIndexName* index = call.func->as<Luau::AstExprIndexName>();
	std::optional<std::string> name = get_string_argument(call, 0);

	if (call.args.size != 1 || index == nullptr || !name)
	{
		return std::nullopt;
	}

	std::optional<Luau::LValue> target = Luau::tryGetLValue(*index->expr);

	if (!target)
	{
		return std::nullopt;
	}

	std::optional<Luau::TypeId> type = lookup_class(*checker.globalScope, *name);

	if (!type)
	{
		report_old(checker, call.args.data[0]->location, Luau::UnknownSymbol{*name, Luau::UnknownSymbol::Type});
		return std::nullopt;
	}

	Luau::TypePackId boolean = checker.currentModule->internalTypes->addTypePack({checker.booleanType});
	return Luau::WithPredicate<Luau::TypePackId>{boolean, {Luau::IsAPredicate{std::move(*target), call.location, *type}}};
}

bool magic_is_a::infer(const Luau::MagicFunctionCallContext& context)
{
	std::optional<std::string> name = get_string_argument(*context.callSite, 0);

	if (context.callSite->args.size == 1 && name && !lookup_class(*context.solver->rootScope, *name))
	{
		report_new(context, context.callSite->args.data[0]->location, Luau::UnknownSymbol{*name, Luau::UnknownSymbol::Type});
	}

	return false;
}

void magic_is_a::refine(const Luau::MagicRefinementContext& context)
{
	if (context.callSite == nullptr || context.callSite->args.size != 1 || context.discriminantTypes.empty())
	{
		return;
	}

	std::optional<std::string> name = get_string_argument(*context.callSite, 0);
	std::optional<Luau::TypeId> discriminant = context.discriminantTypes[0];

	if (!name || !discriminant)
	{
		return;
	}

	if (std::optional<Luau::TypeId> type = lookup_class(*context.scope, *name))
	{
		Luau::asMutable(*discriminant)->ty.emplace<Luau::BoundType>(*type);
	}
}

std::optional<Luau::WithPredicate<Luau::TypePackId>> magic_clone::handleOldSolver(
	Luau::TypeChecker& checker, const Luau::ScopePtr& scope, const Luau::AstExprCall& call, Luau::WithPredicate<Luau::TypePackId> predicate)
{
	Luau::AstExprIndexName* index = call.func->as<Luau::AstExprIndexName>();

	if (index == nullptr)
	{
		return std::nullopt;
	}

	Luau::TypeId self = checker.checkExpr(scope, *index->expr).type;
	return Luau::WithPredicate<Luau::TypePackId>{checker.currentModule->internalTypes->addTypePack({self})};
}

bool magic_clone::infer(const Luau::MagicFunctionCallContext& context)
{
	std::optional<Luau::TypeId> self = Luau::first(context.arguments);

	if (!self || context.callSite->func->as<Luau::AstExprIndexName>() == nullptr)
	{
		return false;
	}

	bind_result(context, *self);
	return true;
}

std::optional<Luau::WithPredicate<Luau::TypePackId>> magic_from_existing::handleOldSolver(
	Luau::TypeChecker& checker, const Luau::ScopePtr& scope, const Luau::AstExprCall& call, Luau::WithPredicate<Luau::TypePackId> predicate)
{
	if (call.args.size < 1)
	{
		return std::nullopt;
	}

	Luau::TypeId existing = checker.checkExpr(scope, *call.args.data[0]).type;
	return Luau::WithPredicate<Luau::TypePackId>{checker.currentModule->internalTypes->addTypePack({existing})};
}

bool magic_from_existing::infer(const Luau::MagicFunctionCallContext& context)
{
	std::optional<Luau::TypeId> existing = Luau::first(context.arguments);

	if (context.callSite->args.size < 1 || !existing)
	{
		return false;
	}

	bind_result(context, *existing);
	return true;
}

std::optional<Luau::WithPredicate<Luau::TypePackId>> magic_find_class::handleOldSolver(
	Luau::TypeChecker& checker, const Luau::ScopePtr& scope, const Luau::AstExprCall& call, Luau::WithPredicate<Luau::TypePackId> predicate)
{
	std::optional<std::string> name = get_string_argument(call, 0);

	if (!name)
	{
		return std::nullopt;
	}

	std::optional<Luau::TypeId> type = lookup_class(*checker.globalScope, *name);

	if (!type)
	{
		report_old(checker, call.args.data[0]->location, Luau::UnknownSymbol{*name, Luau::UnknownSymbol::Type});
		return std::nullopt;
	}

	Luau::TypeArena& arena = *checker.currentModule->internalTypes;
	return Luau::WithPredicate<Luau::TypePackId>{arena.addTypePack({Luau::makeOption(checker.builtinTypes, arena, *type)})};
}

bool magic_find_class::infer(const Luau::MagicFunctionCallContext& context)
{
	std::optional<std::string> name = get_string_argument(*context.callSite, 0);

	if (!name)
	{
		return false;
	}

	std::optional<Luau::TypeId> type = lookup_class(*context.solver->rootScope, *name);

	if (!type)
	{
		report_new(context, context.callSite->args.data[0]->location, Luau::UnknownSymbol{*name, Luau::UnknownSymbol::Type});
		return false;
	}

	bind_result(context, Luau::makeOption(context.solver->builtinTypes, *context.solver->arena, *type));
	return true;
}

magic_lookup::magic_lookup(std::vector<std::string> allowed, std::string error)
	: allowed(std::move(allowed))
	, error(std::move(error))
{
}

std::optional<Luau::WithPredicate<Luau::TypePackId>> magic_lookup::handleOldSolver(
	Luau::TypeChecker& checker, const Luau::ScopePtr& scope, const Luau::AstExprCall& call, Luau::WithPredicate<Luau::TypePackId> predicate)
{
	std::optional<std::string> name = get_string_argument(call, 0);

	if (!name)
	{
		return std::nullopt;
	}

	if (std::find(allowed.begin(), allowed.end(), *name) == allowed.end())
	{
		report_old(checker, call.args.data[0]->location, Luau::GenericError{error + " '" + *name + "'"});
		return std::nullopt;
	}

	std::optional<Luau::TypeId> type = lookup_class(*checker.globalScope, *name);

	if (!type)
	{
		report_old(checker, call.args.data[0]->location, Luau::UnknownSymbol{*name, Luau::UnknownSymbol::Type});
		return std::nullopt;
	}

	return Luau::WithPredicate<Luau::TypePackId>{checker.currentModule->internalTypes->addTypePack({*type})};
}

bool magic_lookup::infer(const Luau::MagicFunctionCallContext& context)
{
	std::optional<std::string> name = get_string_argument(*context.callSite, 0);

	if (!name)
	{
		return false;
	}

	if (std::find(allowed.begin(), allowed.end(), *name) == allowed.end())
	{
		report_new(context, context.callSite->args.data[0]->location, Luau::GenericError{error + " '" + *name + "'"});
		return false;
	}

	std::optional<Luau::TypeId> type = lookup_class(*context.solver->rootScope, *name);

	if (!type)
	{
		report_new(context, context.callSite->args.data[0]->location, Luau::UnknownSymbol{*name, Luau::UnknownSymbol::Type});
		return false;
	}

	bind_result(context, *type);
	return true;
}

void attach(Luau::TableType::Props& props, std::string_view name, const std::shared_ptr<Luau::MagicFunction>& magic)
{
	const auto found = props.find(std::string(name));

	if (found == props.end() || !found->second.readTy || !Luau::get<Luau::FunctionType>(*found->second.readTy))
	{
		return;
	}

	Luau::attachMagicFunction(*found->second.readTy, magic);
}

void apply_instance(Luau::GlobalTypes& globals)
{
	if (std::optional<Luau::TypeFun> object = globals.globalScope->lookupType("Object"))
	{
		if (Luau::ExternType* object_class = Luau::getMutable<Luau::ExternType>(object->type))
		{
			for (const auto& [name, factory] : relevant_object_magic)
			{
				attach(object_class->props, name, factory());
			}
		}
	}

	std::optional<Luau::TypeFun> instance = globals.globalScope->lookupType("Instance");

	if (!instance)
	{
		return;
	}

	Luau::ExternType* instance_class = Luau::getMutable<Luau::ExternType>(instance->type);

	if (instance_class == nullptr)
	{
		return;
	}

	for (const auto& [name, factory] : relevant_instance_magic)
	{
		attach(instance_class->props, name, factory());
	}

	if (instance_class->props.find("IsA") != instance_class->props.end())
	{
		attach(instance_class->props, "IsA", std::make_shared<magic_is_a>());
	}

	for (auto& [name, definition] : globals.globalScope->exportedTypeBindings)
	{
		Luau::ExternType* extern_class = Luau::getMutable<Luau::ExternType>(definition.type);

		if (extern_class != nullptr && Luau::isSubclass(extern_class, instance_class))
		{
			extern_class->metatable = instance_class->metatable;
		}
	}
}

void apply_constructors(Luau::GlobalTypes& globals, const json& metadata)
{
	std::vector<std::string> creatable = get_list(metadata, "CREATABLE_INSTANCES");
	std::vector<std::string> services = get_list(metadata, "SERVICES");

	if (std::optional<Luau::TypeId> instance = globals.globalScope->lookup(Luau::AstName("Instance")))
	{
		if (const Luau::TableType* table = Luau::get<Luau::TableType>(Luau::follow(*instance)))
		{
			Luau::TableType::Props& props = const_cast<Luau::TableType*>(table)->props;

			if (!creatable.empty())
			{
				attach(props, "new", std::make_shared<magic_lookup>(creatable, "Invalid class name"));
			}

			attach(props, "fromExisting", std::make_shared<magic_from_existing>());
		}
	}

	if (services.empty())
	{
		return;
	}

	if (std::optional<Luau::TypeFun> provider = globals.globalScope->lookupType("ServiceProvider"))
	{
		if (Luau::ExternType* provider_class = Luau::getMutable<Luau::ExternType>(provider->type))
		{
			attach(provider_class->props, "GetService", std::make_shared<magic_lookup>(services, "Invalid service name"));
		}
	}
}

void apply_enums(Luau::GlobalTypes& globals)
{
	std::unordered_map<Luau::Name, Luau::TypeFun> enums;
	std::unordered_map<Luau::Name, Luau::TypeFun>& exported = globals.globalScope->exportedTypeBindings;

	for (auto it = exported.begin(); it != exported.end();)
	{
		Luau::ExternType* extern_class = Luau::getMutable<Luau::ExternType>(it->second.type);

		if (extern_class == nullptr || !stdx::starts_with(extern_class->name, "Enum"))
		{
			++it;
			continue;
		}

		extern_class->metatable = std::nullopt;

		if (extern_class->name == "Enum" || extern_class->name == "Enums" || extern_class->name == "EnumItem")
		{
			++it;
			continue;
		}

		std::string name = extern_class->name.substr(4);

		if (stdx::ends_with(name, "_INTERNAL"))
		{
			name.erase(name.size() - 9);
		}
		else
		{
			enums.emplace(name, it->second);
		}

		extern_class->name = "Enum." + name;
		it = exported.erase(it);
	}

	globals.globalScope->importedTypeBindings["Enum"] = enums;
}

std::vector<std::string> get_list(const json& metadata, const char* key)
{
	std::vector<std::string> list;

	if (!metadata.is_object() || !metadata.contains(key) || !metadata[key].is_array())
	{
		return list;
	}

	for (const json& item : metadata[key])
	{
		if (item.is_string())
		{
			list.push_back(item.get<std::string>());
		}
	}

	return list;
}

}
}

}
