#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include <iostream>

#if LUNAUT_PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

namespace lunaut
{
namespace
{
namespace priv
{

struct commandinfo
{
	std::string command = "lsp";
	optionsinfo options;
	std::vector<std::string> inputs;
	bool verbose = false;
	bool json_output = false;
};

using argument_handler = result<nothing> (*)(commandinfo& info, const std::string& value);

result<nothing> read_flag(commandinfo& info, const std::string& value);
result<nothing> read_definition(commandinfo& info, const std::string& value);
result<nothing> read_settings(commandinfo& info, const std::string& value);
result<nothing> read_no_flags(commandinfo& info, const std::string& value);
result<nothing> read_verbose(commandinfo& info, const std::string& value);
result<nothing> read_json(commandinfo& info, const std::string& value);
result<nothing> read_ignored(commandinfo& info, const std::string& value);

const std::vector<std::pair<std::string_view, argument_handler>> relevant_arguments = {
	{"--flag:", &read_flag},
	{"--definition=", &read_definition},
	{"--definitions=", &read_definition},
	{"--settings=", &read_settings},
	{"--no-flags-enabled", &read_no_flags},
	{"--verbose", &read_verbose},
	{"--json", &read_json},
	{"--stdio", &read_ignored},
	{"--clientProcessId=", &read_ignored},
};

const std::vector<std::string_view> commands = {"lsp", "analyze", "flags", "version", "help"};

result<commandinfo> parse(int argc, char** argv);
int invoke_lsp(const commandinfo& info);
int invoke_flags(const commandinfo& info);
void print_help();

}
}

}

int main(int argc, char** argv)
{
	using namespace lunaut;

	result<priv::commandinfo> parsed = priv::parse(argc, argv);

	if (!parsed)
	{
		std::cerr << parsed.message << "\n\n";
		priv::print_help();
		return 2;
	}

	const priv::commandinfo& info = parsed.value;

	if (info.command == "version")
	{
		std::cout << LUNAUT_NAME << " " << LUNAUT_VERSION << " (Luau " << LUNAUT_LUAU_VERSION << ", " << LUNAUT_COMPILER << ")\n";
		return 0;
	}

	if (info.command == "help")
	{
		priv::print_help();
		return 0;
	}

	if (info.command == "flags")
	{
		return priv::invoke_flags(info);
	}

	if (info.command == "analyze")
	{
		console output(info.verbose);
		return output.invoke_analyze(info.inputs, info.options);
	}

	return priv::invoke_lsp(info);
}

namespace lunaut
{
namespace
{
namespace priv
{

result<commandinfo> parse(int argc, char** argv)
{
	commandinfo info;

	for (int i = 1; i < argc; ++i)
	{
		std::string argument = argv[i];

		if (argument == "--version" || argument == "-v")
		{
			info.command = "version";
			continue;
		}

		if (argument == "--help" || argument == "-h")
		{
			info.command = "help";
			continue;
		}

		if (argument == "--show-flags")
		{
			info.command = "flags";
			continue;
		}

		bool handled = false;

		for (const auto& [prefix, handler] : relevant_arguments)
		{
			if (!stdx::starts_with(argument, prefix))
			{
				continue;
			}

			result<nothing> outcome = handler(info, argument.substr(prefix.size()));

			if (!outcome)
			{
				return result<commandinfo>::fail(outcome.message);
			}

			handled = true;
			break;
		}

		if (handled)
		{
			continue;
		}

		if (stdx::starts_with(argument, "--"))
		{
			return result<commandinfo>::fail("unknown option: " + argument);
		}

		if (i == 1 && std::find(commands.begin(), commands.end(), argument) != commands.end())
		{
			info.command = argument;
			continue;
		}

		info.inputs.push_back(argument);
	}

	if (info.command == "analyze" && info.inputs.empty())
	{
		info.inputs.emplace_back(".");
	}

	return result<commandinfo>::ok(info);
}

result<nothing> read_flag(commandinfo& info, const std::string& value)
{
	size_t equals = value.find('=');

	if (equals == std::string::npos || equals == 0)
	{
		return result<nothing>::fail("expected --flag:Name=Value, got --flag:" + value);
	}

	info.options.fflags[value.substr(0, equals)] = value.substr(equals + 1);
	return result<nothing>::ok({});
}

result<nothing> read_definition(commandinfo& info, const std::string& value)
{
	definitioninfo definition;
	definition.source = enumitem_definition_source::Cli;
	definition.path = value;

	size_t hash = value.rfind('#');

	if (hash != std::string::npos)
	{
		try
		{
			definition.priority = std::stoi(value.substr(hash + 1));
			definition.path = value.substr(0, hash);
		}
		catch (const std::exception&)
		{
			return result<nothing>::fail("invalid definition priority: " + value);
		}
	}

	size_t equals = definition.path.find('=');

	if (equals != std::string::npos && stdx::starts_with(definition.path, "@"))
	{
		definition.name = definition.path.substr(0, equals);
		definition.path = definition.path.substr(equals + 1);
	}

	info.options.definitions.push_back(definition);
	return result<nothing>::ok({});
}

result<nothing> read_settings(commandinfo& info, const std::string& value)
{
	info.options.settings_file = value;
	return result<nothing>::ok({});
}

result<nothing> read_no_flags(commandinfo& info, const std::string& value)
{
	info.options.no_flags_enabled = true;
	return result<nothing>::ok({});
}

result<nothing> read_verbose(commandinfo& info, const std::string& value)
{
	info.verbose = true;
	return result<nothing>::ok({});
}

result<nothing> read_json(commandinfo& info, const std::string& value)
{
	info.json_output = true;
	return result<nothing>::ok({});
}

result<nothing> read_ignored(commandinfo& info, const std::string& value)
{
	return result<nothing>::ok({});
}

int invoke_lsp(const commandinfo& info)
{
#if LUNAUT_PLATFORM_WINDOWS
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif

	std::ios::sync_with_stdio(false);

	jsonrpc rpc(std::cin, std::cout);
	server instance(rpc, info.options);
	return instance.run();
}

int invoke_flags(const commandinfo& info)
{
	console output(info.verbose);
	settingsinfo settings;
	settings.fflags.sync = false;

	fflagregistry::apply(settings.fflags, info.options.fflags, info.options.no_flags_enabled, output);

	if (info.json_output)
	{
		json flags = json::array();

		for (const fflagstateinfo& flag : fflagregistry::get_flags())
		{
			flags.push_back(flag.to_json());
		}

		std::cout << flags.dump(2) << '\n';
		return 0;
	}

	for (const fflagstateinfo& flag : fflagregistry::get_flags())
	{
		std::cout << flag.name << "=" << flag.value << "  (" << flag.kind << ", " << enum_fflag_source::get_name(flag.source) << ")\n";
	}

	return 0;
}

void print_help()
{
	std::cout << LUNAUT_DISPLAY_NAME << " " << LUNAUT_VERSION << "\n\n"
			  << "Usage:\n"
			  << "  lunaut [lsp] [options]            start the language server on stdio\n"
			  << "  lunaut analyze [options] <paths>  check files or folders and print diagnostics\n"
			  << "  lunaut flags [options]            list Luau FFlags and their values\n"
			  << "  lunaut version                    print the version\n\n"
			  << "Options:\n"
			  << "  --flag:<Name>=<Value>             set a Luau FFlag\n"
			  << "  --no-flags-enabled                do not enable Luau FFlags by default\n"
			  << "  --definition=[@name=]<path>[#priority]\n"
			  << "                                    load a definitions file (.d.luau)\n"
			  << "  --settings=<file>                 extra settings file (.vscode/settings.json format)\n"
			  << "  --verbose                         print server log to stderr\n"
			  << "  --json                            print flags as JSON (flags command)\n";
}

}
}

}
