#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

namespace lunaut
{

json workspace::get_declaration(const json& params)
{
	return find_symbol_location(params, true);
}

}
