import pathlib
import shutil
import string
import subprocess
import sys
import tempfile
import unittest

from lsp_client import FIXTURES, LspClient, LspError, position_of, run_cli, to_uri


def hover_text(client, uri, text, needle, offset=0):
	line, character = position_of(text, needle, offset)
	result = client.at("textDocument/hover", uri, line, character)
	return "" if result is None else result["contents"]["value"]


def completion_items(client, uri, text, needle, offset=0):
	line, character = position_of(text, needle, offset)
	result = client.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}})
	return result["items"] if isinstance(result, dict) else result


class DocumentationTests(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.client = LspClient(FIXTURES / "documentation")
		cls.main = (FIXTURES / "documentation" / "main.luau").read_text(encoding="utf-8")
		cls.uri, _ = cls.client.open("main.luau")

	@classmethod
	def tearDownClass(cls):
		cls.client.close()

	def test_type_alias_comment_from_definition_file(self):
		value = hover_text(self.client, self.uri, self.main, "local point: Point", len("local point: "))
		self.assertIn("type Point", value)
		self.assertIn("A point in world space.", value)

	def test_intersection_comment_from_definition_file(self):
		value = hover_text(self.client, self.uri, self.main, "local moving: MovingPoint", len("local moving: "))
		self.assertIn("type MovingPoint", value)
		self.assertIn("A moving point.", value)

	def test_intersection_comment_on_local_value(self):
		value = hover_text(self.client, self.uri, self.main, "local moving: MovingPoint", len("local "))
		self.assertIn("local moving: MovingPoint", value)
		self.assertIn("A moving point.", value)

	def test_function_comment_from_definition_file(self):
		value = hover_text(self.client, self.uri, self.main, "add(1, 2)")
		self.assertIn("function add", value)
		self.assertIn("Adds two numbers.", value)

	def test_field_comment_from_definition_file(self):
		value = hover_text(self.client, self.uri, self.main, "point.x", len("point."))
		self.assertIn("x: number", value)
		self.assertIn("Horizontal coordinate.", value)

		value = hover_text(self.client, self.uri, self.main, "moving.speed", len("moving."))
		self.assertIn("speed: number", value)
		self.assertIn("Speed in studs per second.", value)

	def test_type_alias_definition_jump(self):
		line, character = position_of(self.main, "local point: Point", len("local point: "))
		location = self.client.at("textDocument/definition", self.uri, line, character)
		self.assertTrue(location["uri"].endswith(".d.luau"), location)
		self.assertEqual(location["range"]["start"]["line"], 1)

	def test_type_alias_name_stays_at_declaration(self):
		path = FIXTURES / "documentation" / "types" / "base.d.luau"
		text = path.read_text(encoding="utf-8")
		uri = to_uri(path)
		self.client.notify(
			"textDocument/didOpen",
			{"textDocument": {"uri": uri, "languageId": "luau", "version": 1, "text": text}},
		)
		line, character = position_of(text, "export type Point", len("export type "))
		location = self.client.at("textDocument/definition", uri, line, character)
		self.assertTrue(location["uri"].endswith("base.d.luau"), location)
		self.assertEqual(location["range"]["start"]["line"], 1)
		self.assertLessEqual(location["range"]["start"]["character"], len("export type Point"))

	def test_local_imported_type_keeps_name(self):
		value = hover_text(self.client, self.uri, self.main, "local object: LuaObject", len("local "))
		self.assertIn("local object: LuaObject", value)
		self.assertNotIn("name: string", value)

	def test_completion_shows_field_and_function_comments(self):
		items = completion_items(self.client, self.uri, self.main, "point.x", len("point."))
		field = next(item for item in items if item["label"] == "x")
		self.assertIn("Horizontal coordinate.", field["documentation"]["value"])
		function = next(item for item in items if item["label"] == "move")
		self.assertIn("Moves the point by the given offset.", function["documentation"]["value"])

		items = completion_items(self.client, self.uri, self.main, "moving.speed", len("moving."))
		field = next(item for item in items if item["label"] == "speed")
		self.assertIn("Speed in studs per second.", field["documentation"]["value"])

	def test_completion_shows_function_comment(self):
		items = completion_items(self.client, self.uri, self.main, "add(1, 2)", 1)
		function = next(item for item in items if item["label"] == "add")
		self.assertIn("Adds two numbers.", function["documentation"]["value"])


class RequireAliasTests(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.client = LspClient(FIXTURES / "annotations")
		cls.consumer = (FIXTURES / "annotations" / "consumer.luau").read_text(encoding="utf-8")
		cls.uri, _ = cls.client.open("consumer.luau")

	@classmethod
	def tearDownClass(cls):
		cls.client.close()

	def test_require_preserves_annotated_alias(self):
		value = hover_text(self.client, self.uri, self.consumer, "local config = require", 6)
		self.assertIn("local config: Config", value)
		self.assertNotIn("maxPlayers", value)

	def test_require_call_preserves_annotated_alias(self):
		value = hover_text(self.client, self.uri, self.consumer, 'require("./config")')
		self.assertEqual(value.strip(), "```luau\nConfig\n```")

	def test_require_preserves_intersection_alias(self):
		value = hover_text(self.client, self.uri, self.consumer, "local shape = require", 6)
		self.assertIn("local shape: Shape", value)
		self.assertIn("Intersected shape.", value)

	def test_require_preserves_recursive_alias(self):
		value = hover_text(self.client, self.uri, self.consumer, "local node = require", 6)
		self.assertIn("local node: Node", value)
		self.assertIn("Recursive node.", value)
		self.assertNotIn("where", value)


class TypeStatsTests(unittest.TestCase):
	def test_type_stats_request(self):
		client = LspClient(FIXTURES / "definitions")

		try:
			main = (FIXTURES / "definitions" / "main.luau").read_text(encoding="utf-8")
			uri, _ = client.open("main.luau")
			client.at("textDocument/hover", uri, 0, 12)

			stats = client.request("lunaut/typeStats", {})["workspaces"][0]["stats"]
			self.assertGreater(stats["total"], 0)
			self.assertIn("table", stats["types"]["kinds"])
			self.assertIn("extern", stats["types"]["kinds"])
			self.assertTrue(any(origin["name"] == "@base" for origin in stats["origins"]), stats["origins"])
			self.assertIn({"name": "Car", "parent": "Vehicle"}, stats["hierarchy"])
			self.assertTrue(
				any(entry["name"] == "Vehicle" and "Car" in entry["children"] for entry in stats["subtypes"]),
				stats["subtypes"],
			)
			self.assertTrue(any(alias["name"] == "Car" for alias in stats["aliases"]), stats["aliases"])
			self.assertIn("analyzer", stats)
			self.assertTrue(main)
		finally:
			client.close()

	def test_stats_cli_json(self):
		result = run_cli("stats", ".", "--json", cwd=FIXTURES / "definitions")
		self.assertEqual(result.returncode, 0, result.stderr)
		self.assertIn('"hierarchy"', result.stdout)
		self.assertIn('"Car"', result.stdout)
		self.assertIn('"Vehicle"', result.stdout)

	def test_stats_cli_text(self):
		result = run_cli("stats", ".", cwd=FIXTURES / "definitions")
		self.assertEqual(result.returncode, 0, result.stderr)
		self.assertIn("Types:", result.stdout)
		self.assertIn("Origins:", result.stdout)
		self.assertIn("Car extends Vehicle", result.stdout)


class ProtectionTests(unittest.TestCase):
	def test_broken_edit_does_not_crash(self):
		client = LspClient(FIXTURES / "basic")

		try:
			uri, _ = client.open("main.luau")
			diagnostics = client.change(uri, "local x = \n", version=2)
			self.assertTrue(any(diagnostic["code"] == "SyntaxError" for diagnostic in diagnostics), diagnostics)
			client.at("textDocument/hover", uri, 0, 6)
			self.assertEqual(client.request("lunaut/status", {})["name"], "lunaut")
			self.assertEqual(client.change(uri, "local x = 1\nprint(x)\n", version=3), [])
		finally:
			client.close()

	def test_malformed_request_does_not_crash(self):
		client = LspClient(FIXTURES / "basic")

		try:
			uri, _ = client.open("main.luau")
			self.assertIsNone(client.request("textDocument/hover", []))
			self.assertIsNone(client.request("textDocument/hover", {}))

			with self.assertRaises(LspError):
				client.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": "x", "character": 0}})

			status = client.request("lunaut/status", {})
			self.assertEqual(status["name"], "lunaut")
			self.assertTrue(uri)
		finally:
			client.close()


PLUGIN_TEMPLATE = string.Template(r"""
#include <stdint.h>
#include <stddef.h>

struct lunaut_analyzer_info
{
	uint32_t size;
	uint32_t abi;
	const char* name;
	const char* luau_version;
	uint32_t luau_version_code;
	uint32_t frontend_api;
};

struct lunaut_analyzer_request
{
	uint32_t size;
	const char* module;
	const char* path;
	const char* source;
	const char* settings;
};

struct lunaut_analyzer_response
{
	uint32_t size;
	const char* diagnostics;
	const char* error;
};

static const struct lunaut_analyzer_info info = {
	sizeof(struct lunaut_analyzer_info),
	$abi,
	"$name",
	"$version",
	$version_code,
	$frontend_api,
};

const struct lunaut_analyzer_info* lunaut_analyzer_info_v1(void)
{
	return &info;
}

int lunaut_analyzer_check_v1(const struct lunaut_analyzer_request* request, struct lunaut_analyzer_response* response)
{
	response->diagnostics = "$diagnostics";
	response->error = NULL;
	return 0;
}

void lunaut_analyzer_free_v1(const char* text)
{
	(void)text;
}
""")


def build_plugin(directory, name="test-analyzer", version="0.747.0", version_code=74700, frontend_api=2, abi=1, diagnostics="[]"):
	compiler = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")

	if compiler is None:
		return None

	root = pathlib.Path(directory)
	source = root / "plugin.c"
	target = root / f"lib{name}.so"
	source.write_text(
		PLUGIN_TEMPLATE.substitute(
			abi=abi,
			name=name,
			version=version,
			version_code=version_code,
			frontend_api=frontend_api,
			diagnostics=diagnostics.replace('"', '\\"'),
		),
		encoding="utf-8",
	)
	subprocess.run([compiler, "-shared", "-fPIC", "-o", str(target), str(source)], check=True)
	return target


@unittest.skipIf(sys.platform == "win32", "plugins are built with a POSIX C compiler")
class AnalyzerTests(unittest.TestCase):
	def setUp(self):
		self.tools = tempfile.TemporaryDirectory()

	def tearDown(self):
		self.tools.cleanup()

	def client(self, plugin, diagnostics=False):
		return LspClient(
			FIXTURES / "basic",
			{"analyzer": {"enabled": True, "path": str(plugin), "diagnostics": diagnostics}},
		)

	def test_compatible_analyzer_is_loaded(self):
		plugin = build_plugin(self.tools.name)

		if plugin is None:
			self.skipTest("no C compiler")

		client = self.client(plugin)

		try:
			analyzer = client.request("lunaut/status", {})["workspaces"][0]["analyzer"]
			self.assertTrue(analyzer["enabled"])
			self.assertTrue(analyzer["loaded"])
			self.assertEqual(analyzer["name"], "test-analyzer")
			self.assertEqual(analyzer["frontend_api"], "post-0.743")
			self.assertEqual(analyzer["error"], "")
		finally:
			client.close()

	def test_pre_0743_analyzer_is_supported(self):
		plugin = build_plugin(self.tools.name, name="old-analyzer", version="0.742.0", version_code=74200, frontend_api=1)

		if plugin is None:
			self.skipTest("no C compiler")

		client = self.client(plugin)

		try:
			analyzer = client.request("lunaut/status", {})["workspaces"][0]["analyzer"]
			self.assertTrue(analyzer["loaded"])
			self.assertEqual(analyzer["frontend_api"], "pre-0.743")
		finally:
			client.close()

	def test_analyzer_diagnostics_are_merged(self):
		payload = (
			'[{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},'
			'"severity":1,"source":"test-analyzer","message":"external diagnostic"}]'
		)
		plugin = build_plugin(self.tools.name, name="diagnostic-analyzer", diagnostics=payload)

		if plugin is None:
			self.skipTest("no C compiler")

		client = self.client(plugin, diagnostics=True)

		try:
			uri, diagnostics = client.open("main.luau")
			self.assertTrue(any(diagnostic.get("message") == "external diagnostic" for diagnostic in diagnostics), diagnostics)
			self.assertIsNotNone(uri)
		finally:
			client.close()

	def test_incompatible_abi_is_reported(self):
		plugin = build_plugin(self.tools.name, name="bad-abi", abi=99)

		if plugin is None:
			self.skipTest("no C compiler")

		client = self.client(plugin)

		try:
			analyzer = client.request("lunaut/status", {})["workspaces"][0]["analyzer"]
			self.assertFalse(analyzer["loaded"])
			self.assertIn("incompatible analyzer ABI", analyzer["error"])

			uri, _ = client.open("main.luau")
			self.assertIsNotNone(client.at("textDocument/hover", uri, 2, 6))
		finally:
			client.close()

	def test_version_and_api_mismatch_is_reported(self):
		plugin = build_plugin(self.tools.name, name="bad-version", version="0.747.0", version_code=74700, frontend_api=1)

		if plugin is None:
			self.skipTest("no C compiler")

		client = self.client(plugin)

		try:
			analyzer = client.request("lunaut/status", {})["workspaces"][0]["analyzer"]
			self.assertFalse(analyzer["loaded"])
			self.assertIn("pre-0.743 frontend API cannot serve", analyzer["error"])
		finally:
			client.close()
