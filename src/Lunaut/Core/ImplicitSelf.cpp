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

class selfwriter : public Luau::AstVisitor
{
public:
	selfwriter(Luau::SourceModule& source, const Luau::ScopePtr& globals);

	bool visit(Luau::AstStatBlock* node) override;
	bool visit(Luau::AstStatFunction* node) override;

	size_t get_written() const;

private:
	bool is_known_type(const std::string& name) const;
	std::optional<std::string> get_target(const Luau::AstStatFunction* node) const;
	void write_self(Luau::AstExprFunction* function, const std::string& name);

	Luau::SourceModule& source;
	const Luau::ScopePtr& globals;
	std::vector<std::set<std::string>> aliases;
	size_t written = 0;
};

}
}

size_t implicitself::apply(Luau::SourceModule& source, const Luau::ScopePtr& globals)
{
	if (source.root == nullptr || !source.allocator || !source.names)
	{
		return 0;
	}

	priv::selfwriter writer(source, globals);
	source.root->visit(&writer);

	return writer.get_written();
}

namespace
{
namespace priv
{

selfwriter::selfwriter(Luau::SourceModule& source, const Luau::ScopePtr& globals)
	: source(source)
	, globals(globals)
{
}

bool selfwriter::visit(Luau::AstStatBlock* node)
{
	std::set<std::string> declared;

	for (Luau::AstStat* statement : node->body)
	{
		const Luau::AstStatTypeAlias* alias = statement->as<Luau::AstStatTypeAlias>();

		if (alias != nullptr && alias->generics.size == 0 && alias->genericPacks.size == 0)
		{
			declared.insert(alias->name.value);
		}
	}

	aliases.push_back(std::move(declared));

	for (Luau::AstStat* statement : node->body)
	{
		statement->visit(this);
	}

	aliases.pop_back();
	return false;
}

bool selfwriter::visit(Luau::AstStatFunction* node)
{
	std::optional<std::string> target = get_target(node);

	if (target && is_known_type(*target))
	{
		write_self(node->func, *target);
	}

	return true;
}

size_t selfwriter::get_written() const
{
	return written;
}

bool selfwriter::is_known_type(const std::string& name) const
{
	for (const std::set<std::string>& declared : aliases)
	{
		if (declared.count(name) != 0)
		{
			return true;
		}
	}

	if (!globals)
	{
		return false;
	}

	const auto found = globals->exportedTypeBindings.find(name);

	return found != globals->exportedTypeBindings.end() && found->second.typeParams.empty() && found->second.typePackParams.empty();
}

std::optional<std::string> selfwriter::get_target(const Luau::AstStatFunction* node) const
{
	const Luau::AstExprIndexName* index = node->name->as<Luau::AstExprIndexName>();
	const Luau::AstExprFunction* function = node->func;

	if (index == nullptr || index->op != ':' || function == nullptr || function->self == nullptr || function->body == nullptr ||
		!function->body->hasEnd)
	{
		return std::nullopt;
	}

	if (const Luau::AstExprLocal* local = index->expr->as<Luau::AstExprLocal>())
	{
		return std::string(local->local->name.value);
	}

	if (const Luau::AstExprGlobal* global = index->expr->as<Luau::AstExprGlobal>())
	{
		return std::string(global->name.value);
	}

	return std::nullopt;
}

void selfwriter::write_self(Luau::AstExprFunction* function, const std::string& name)
{
	Luau::AstLocal* self = function->self;
	self->annotation = source.allocator->alloc<Luau::AstTypeReference>(
		self->location, std::nullopt, source.names->getOrAdd(name.c_str()), std::nullopt, self->location);

	size_t count = function->args.size + 1;
	Luau::AstLocal** arguments = static_cast<Luau::AstLocal**>(source.allocator->allocate(sizeof(Luau::AstLocal*) * count));
	arguments[0] = self;

	for (size_t index = 0; index < function->args.size; ++index)
	{
		arguments[index + 1] = function->args.data[index];
	}

	function->args = Luau::AstArray<Luau::AstLocal*>{arguments, count};
	function->self = nullptr;
	++written;
}

}
}

}
