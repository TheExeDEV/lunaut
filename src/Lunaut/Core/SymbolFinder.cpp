#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/AstQuery.h"

namespace lunaut
{
namespace
{
namespace priv
{

struct local_collector final : Luau::AstVisitor
{
	explicit local_collector(Luau::AstLocal* local);

	bool visit(Luau::AstExprLocal* node) override;

	Luau::AstLocal* local;
	std::vector<Luau::Location> found;
};

struct member_collector final : Luau::AstVisitor
{
	member_collector(const Luau::Module& module, const symbolinfo& symbol);

	bool visit(Luau::AstExprIndexName* node) override;
	bool visit(Luau::AstExprTable* node) override;

	const Luau::Module& module;
	const symbolinfo& symbol;
	std::vector<Luau::Location> found;
};

struct write_collector final : Luau::AstVisitor
{
	write_collector(const Luau::Module& module, const symbolinfo& symbol);

	bool visit(Luau::AstStatAssign* node) override;
	bool visit(Luau::AstStatCompoundAssign* node) override;
	bool visit(Luau::AstStatFunction* node) override;
	bool visit(Luau::AstExprTable* node) override;

	void add_target(Luau::AstExpr* target);

	const Luau::Module& module;
	const symbolinfo& symbol;
	member_collector members;
	std::vector<Luau::Location> found;
};

std::optional<symbolinfo>
make_member(const Luau::Module& module, const Luau::AstExpr* object, const std::string& name, Luau::Location origin);

}
}

bool symbolinfo::is_same_owner(const symbolinfo& other) const
{
	return member && other.member && by_property == other.by_property && owner_module == other.owner_module &&
		owner_location == other.owner_location;
}

std::optional<symbolinfo> symbolfinder::find(const Luau::Module& module, const Luau::SourceModule& source, Luau::Position position)
{
	Luau::ExprOrLocal hit = Luau::findExprOrLocalAtPosition(source, position);

	if (Luau::AstLocal* local = hit.getLocal())
	{
		return symbolinfo{local, local->name.value, false, false, {}, {}, local->location};
	}

	Luau::AstExpr* expr = hit.getExpr();

	if (expr == nullptr)
	{
		return std::nullopt;
	}

	if (Luau::AstExprLocal* local = expr->as<Luau::AstExprLocal>())
	{
		return symbolinfo{local->local, local->local->name.value, false, false, {}, {}, local->location};
	}

	if (Luau::AstExprIndexName* index = expr->as<Luau::AstExprIndexName>())
	{
		if (!index->indexLocation.containsClosed(position))
		{
			return std::nullopt;
		}

		std::string name = index->index.value;
		std::optional<symbolinfo> member = priv::make_member(module, index->expr, name, index->indexLocation);
		return member ? member : std::optional<symbolinfo>(symbolinfo{nullptr, name, false, false, {}, {}, index->indexLocation});
	}

	Luau::AstExprConstantString* key = expr->as<Luau::AstExprConstantString>();

	if (key == nullptr)
	{
		return std::nullopt;
	}

	for (Luau::AstNode* node : Luau::findAstAncestryOfPosition(source, position))
	{
		Luau::AstExprTable* table = node->as<Luau::AstExprTable>();

		if (table == nullptr)
		{
			continue;
		}

		for (const Luau::AstExprTable::Item& item : table->items)
		{
			if (item.kind == Luau::AstExprTable::Item::Kind::Record && item.key == key)
			{
				std::string name(key->value.data, key->value.size);
				std::optional<symbolinfo> member = priv::make_member(module, table, name, key->location);
				return member ? member : std::optional<symbolinfo>(symbolinfo{nullptr, name, false, false, {}, {}, key->location});
			}
		}
	}

	return std::nullopt;
}

std::optional<symbolinfo> symbolfinder::get_member(const Luau::Module& module, const Luau::AstExpr* object, const std::string& name)
{
	return priv::make_member(module, object, name, object->location);
}

std::vector<Luau::Location> symbolfinder::find_local_uses(const Luau::SourceModule& source, Luau::AstLocal* local)
{
	priv::local_collector collected(local);

	if (source.root != nullptr)
	{
		source.root->visit(&collected);
	}

	return collected.found;
}

std::vector<Luau::Location>
symbolfinder::find_member_uses(const Luau::Module& module, const Luau::SourceModule& source, const symbolinfo& symbol)
{
	priv::member_collector collected(module, symbol);

	if (source.root != nullptr)
	{
		source.root->visit(&collected);
	}

	return collected.found;
}

std::vector<Luau::Location>
symbolfinder::find_writes(const Luau::Module& module, const Luau::SourceModule& source, const symbolinfo& symbol)
{
	priv::write_collector collected(module, symbol);

	if (symbol.local != nullptr)
	{
		collected.found.push_back(symbol.local->location);
	}

	if (source.root != nullptr)
	{
		source.root->visit(&collected);
	}

	return collected.found;
}

namespace
{
namespace priv
{

local_collector::local_collector(Luau::AstLocal* local)
	: local(local)
{
}

bool local_collector::visit(Luau::AstExprLocal* node)
{
	if (node->local == local)
	{
		found.push_back(node->location);
	}

	return true;
}

member_collector::member_collector(const Luau::Module& module, const symbolinfo& symbol)
	: module(module)
	, symbol(symbol)
{
}

bool member_collector::visit(Luau::AstExprIndexName* node)
{
	if (symbol.name == node->index.value)
	{
		std::optional<symbolinfo> found_member = make_member(module, node->expr, symbol.name, node->indexLocation);

		if (found_member && found_member->is_same_owner(symbol))
		{
			found.push_back(node->indexLocation);
		}
	}

	return true;
}

bool member_collector::visit(Luau::AstExprTable* node)
{
	for (const Luau::AstExprTable::Item& item : node->items)
	{
		if (item.kind != Luau::AstExprTable::Item::Kind::Record || item.key == nullptr)
		{
			continue;
		}

		Luau::AstExprConstantString* key = item.key->as<Luau::AstExprConstantString>();

		if (key == nullptr || std::string(key->value.data, key->value.size) != symbol.name)
		{
			continue;
		}

		std::optional<symbolinfo> found_member = make_member(module, node, symbol.name, key->location);

		if (found_member && found_member->is_same_owner(symbol))
		{
			found.push_back(key->location);
		}
	}

	return true;
}

write_collector::write_collector(const Luau::Module& module, const symbolinfo& symbol)
	: module(module)
	, symbol(symbol)
	, members(module, symbol)
{
}

bool write_collector::visit(Luau::AstStatAssign* node)
{
	for (Luau::AstExpr* target : node->vars)
	{
		add_target(target);
	}

	return true;
}

bool write_collector::visit(Luau::AstStatCompoundAssign* node)
{
	add_target(node->var);
	return true;
}

bool write_collector::visit(Luau::AstStatFunction* node)
{
	add_target(node->name);
	return true;
}

bool write_collector::visit(Luau::AstExprTable* node)
{
	if (symbol.member)
	{
		members.found.clear();
		members.visit(node);
		found.insert(found.end(), members.found.begin(), members.found.end());
	}

	return true;
}

void write_collector::add_target(Luau::AstExpr* target)
{
	if (symbol.local != nullptr)
	{
		Luau::AstExprLocal* local = target->as<Luau::AstExprLocal>();

		if (local != nullptr && local->local == symbol.local)
		{
			found.push_back(local->location);
		}

		return;
	}

	Luau::AstExprIndexName* index = target->as<Luau::AstExprIndexName>();

	if (!symbol.member || index == nullptr || symbol.name != index->index.value)
	{
		return;
	}

	std::optional<symbolinfo> found_member = make_member(module, index->expr, symbol.name, index->indexLocation);

	if (found_member && found_member->is_same_owner(symbol))
	{
		found.push_back(index->indexLocation);
	}
}

std::optional<symbolinfo>
make_member(const Luau::Module& module, const Luau::AstExpr* object, const std::string& name, Luau::Location origin)
{
	const Luau::TypeId* type = module.astTypes.find(object);

	if (type == nullptr)
	{
		return std::nullopt;
	}

	Luau::TypeId followed = Luau::follow(*type);
	const Luau::TableType* table = Luau::get<Luau::TableType>(followed);

	if (table == nullptr)
	{
		if (const Luau::MetatableType* metatable = Luau::get<Luau::MetatableType>(followed))
		{
			table = Luau::get<Luau::TableType>(Luau::follow(metatable->table));
		}
	}

	if (table == nullptr)
	{
		return std::nullopt;
	}

	if (!table->definitionModuleName.empty())
	{
		return symbolinfo{nullptr, name, true, false, table->definitionModuleName, table->definitionLocation, origin};
	}

	const auto property = table->props.find(name);

	if (property != table->props.end() && property->second.location)
	{
		return symbolinfo{nullptr, name, true, true, "", *property->second.location, origin};
	}

	return std::nullopt;
}

}
}

}
