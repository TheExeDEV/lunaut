import unittest

from lsp_client import FIXTURES, LspClient, position_of

TYPES = """--!strict
type Point = { x: number, y: number }
local origin: Point = { x = 0, y = 0 }
local copy = origin
local function move(p: Point): Point
	return p
end
print(copy.x, move(origin))
"""

ARMS = """--!strict
local Arms = {}
Arms.__index = Arms
function Arms:update(dt: number)
	self.speed = (self.speed or 0) * dt
end
function Arms:lerp(other, alpha: number)
	return other
end
return Arms
"""

MAGIC = """--!strict
local part = Instance.new("Part")
local clone = part:Clone()
local found = workspace:FindFirstChildWhichIsA("Folder")
print(part, clone, found)
"""


def contains(outer, inner):
	start = (outer["start"]["line"], outer["start"]["character"])
	end = (outer["end"]["line"], outer["end"]["character"])
	inner_start = (inner["start"]["line"], inner["start"]["character"])
	inner_end = (inner["end"]["line"], inner["end"]["character"])
	return start <= inner_start and inner_end <= end


class SymbolTests(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.client = LspClient(FIXTURES / "basic", {"fflags": {"sync": False}})
		cls.main = (FIXTURES / "basic" / "main.luau").read_text(encoding="utf-8")
		cls.main_uri, _ = cls.client.open("main.luau")
		cls.types_uri, _ = cls.client.open("types.luau", TYPES)

	@classmethod
	def tearDownClass(cls):
		cls.client.close()

	def test_capabilities(self):
		for name in (
			"declarationProvider",
			"typeDefinitionProvider",
			"implementationProvider",
			"codeLensProvider",
			"selectionRangeProvider",
			"inlineValueProvider",
			"monikerProvider",
			"typeHierarchyProvider",
		):
			self.assertIn(name, self.client.capabilities)

	def test_type_definition_of_variable(self):
		line, character = position_of(TYPES, "copy =", 1)
		location = self.client.at("textDocument/typeDefinition", self.types_uri, line, character)
		self.assertEqual(location["uri"], self.types_uri)
		self.assertEqual(location["range"]["start"]["line"], 1)

	def test_type_definition_of_annotation(self):
		line, character = position_of(TYPES, "Point = {", 1)
		location = self.client.at("textDocument/typeDefinition", self.types_uri, line + 1, len("local origin: P"))
		self.assertEqual(location["range"]["start"]["line"], line)

	def test_declaration(self):
		line, character = position_of(TYPES, "copy.x", 5)
		location = self.client.at("textDocument/declaration", self.types_uri, line, character)
		self.assertEqual(location["range"]["start"]["line"], 1)

		line, character = position_of(TYPES, "copy.x", 1)
		location = self.client.at("textDocument/declaration", self.types_uri, line, character)
		self.assertEqual(location["range"]["start"], {"line": 3, "character": 6})

	def test_implementation(self):
		line, character = position_of(self.main, "util.add", 6)
		locations = self.client.at("textDocument/implementation", self.main_uri, line, character)
		self.assertEqual([(location["uri"].endswith("util.luau"), location["range"]["start"]["line"]) for location in locations], [(True, 3)])

	def test_code_lens(self):
		uri, _ = self.client.open("util.luau")
		lenses = self.client.request("textDocument/codeLens", {"textDocument": {"uri": uri}})
		self.assertEqual([lens["range"]["start"]["line"] for lens in lenses], [3, 7])

		resolved = self.client.request("codeLens/resolve", lenses[0])
		self.assertEqual(resolved["command"]["title"], "1 reference")
		self.assertEqual(resolved["command"]["command"], "lunaut.showReferences")
		self.assertTrue(resolved["command"]["arguments"][2][0]["uri"].endswith("main.luau"))

	def test_document_symbols_stay_inside_their_range(self):
		sources = [
			"local first = 1\nconst",
			"local first = 1\nlocal",
			"local function\n",
			"function module.\n",
			"type\n",
			"export type Name = \n",
			"const value = 1\nconst other",
		]

		for index, source in enumerate(sources):
			uri, _ = self.client.open(f"broken_symbols_{index}.luau", source)
			symbols = self.client.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})

			for symbol in symbols:
				outer, inner = symbol["range"], symbol["selectionRange"]
				self.assertFalse(symbol["name"].startswith("%"), (source, symbol))
				self.assertLessEqual((outer["start"]["line"], outer["start"]["character"]), (inner["start"]["line"], inner["start"]["character"]), (source, symbol))
				self.assertLessEqual((inner["end"]["line"], inner["end"]["character"]), (outer["end"]["line"], outer["end"]["character"]), (source, symbol))

	def test_selection_range(self):
		line, character = position_of(self.main, "1, 2")
		ranges = self.client.request(
			"textDocument/selectionRange",
			{"textDocument": {"uri": self.main_uri}, "positions": [{"line": line, "character": character}]},
		)
		chain = []
		current = ranges[0]

		while current:
			chain.append(current["range"])
			current = current.get("parent")

		self.assertEqual(chain[0], {"start": {"line": line, "character": character}, "end": {"line": line, "character": character + 1}})
		self.assertTrue(all(contains(outer, inner) for inner, outer in zip(chain, chain[1:])))
		self.assertEqual(chain[-1]["start"]["line"], 0)

	def test_inline_values(self):
		uri = self.client.open("hints.luau")[0]
		values = self.client.request(
			"textDocument/inlineValue",
			{
				"textDocument": {"uri": uri},
				"range": {"start": {"line": 0, "character": 0}, "end": {"line": 10, "character": 0}},
				"context": {"frameId": 1, "stoppedLocation": {"start": {"line": 5, "character": 0}, "end": {"line": 5, "character": 0}}},
			},
		)
		names = {value["variableName"] for value in values}
		self.assertTrue({"value", "factor", "result"} <= names, names)
		self.assertNotIn("label", names)

	def test_method_generics_are_named(self):
		uri, _ = self.client.open("arms.luau", ARMS)
		signatures = {}

		for needle in ("update(dt", "lerp(other"):
			line, character = position_of(ARMS, needle, 1)
			hover = self.client.at("textDocument/hover", uri, line, character)["contents"]["value"]
			signatures[needle] = hover.splitlines()[1]

		self.assertEqual(signatures["update(dt"], "function Arms:update(dt: number): ()")
		self.assertEqual(signatures["lerp(other"], "function Arms:lerp<a>(other: a, alpha: number): a")

	def test_monikers(self):
		line, character = position_of(self.main, "util.add", 6)
		moniker = self.client.at("textDocument/moniker", self.main_uri, line, character)[0]
		self.assertEqual(moniker["identifier"], "util.luau.add")
		self.assertEqual((moniker["unique"], moniker["kind"]), ("project", "import"))

		line, character = position_of(self.main, "count", 1)
		moniker = self.client.at("textDocument/moniker", self.main_uri, line, character)[0]
		self.assertEqual((moniker["unique"], moniker["kind"]), ("document", "local"))

		line, character = position_of(self.main, "print", 1)
		moniker = self.client.at("textDocument/moniker", self.main_uri, line, character)[0]
		self.assertEqual(moniker, {"scheme": "luau", "identifier": "print", "unique": "global", "kind": "import"})


class RobloxSymbolTests(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.client = LspClient(FIXTURES / "rojo")
		cls.path = "src/server/Main.server.luau"
		cls.text = (FIXTURES / "rojo" / cls.path).read_text(encoding="utf-8")
		cls.uri, _ = cls.client.open(cls.path)

	@classmethod
	def tearDownClass(cls):
		cls.client.close()

	def test_definition_of_global(self):
		line, character = position_of(self.text, "game:", 1)
		location = self.client.at("textDocument/definition", self.uri, line, character)
		self.assertTrue(location["uri"].endswith("roblox.d.luau"))
		self.assertEqual(location["range"]["start"]["line"], 72)

	def test_type_definition_of_instance(self):
		line, character = position_of(self.text, "ReplicatedStorage =", 1)
		location = self.client.at("textDocument/typeDefinition", self.uri, line, character)
		self.assertTrue(location["uri"].endswith("roblox.d.luau"))
		self.assertEqual(location["range"]["start"]["line"], 62)

	def test_magic_results_are_displayed(self):
		uri, _ = self.client.open("src/server/Display.server.luau", MAGIC)
		hints = self.client.request(
			"textDocument/inlayHint",
			{"textDocument": {"uri": uri}, "range": {"start": {"line": 0, "character": 0}, "end": {"line": 5, "character": 0}}},
		)
		labels = [hint["label"] if isinstance(hint["label"], str) else "".join(part["value"] for part in hint["label"]) for hint in hints]
		self.assertEqual([label for label in labels if label.startswith(":")], [": Part", ": Part", ": Folder?"])

		hover = self.client.at("textDocument/hover", uri, 2, 7)["contents"]["value"]
		self.assertIn("local clone: Part", hover)

	def test_type_hierarchy(self):
		line, character = position_of(self.text, "ReplicatedStorage =", 1)
		items = self.client.at("textDocument/prepareTypeHierarchy", self.uri, line, character)
		self.assertEqual(items[0]["name"], "ReplicatedStorage")
		self.assertEqual(items[0]["detail"], "extends Instance")

		supertypes = self.client.request("typeHierarchy/supertypes", {"item": items[0]})
		self.assertEqual([item["name"] for item in supertypes], ["Instance"])
		self.assertEqual(supertypes[0]["range"]["start"]["line"], 27)

		subtypes = self.client.request("typeHierarchy/subtypes", {"item": supertypes[0]})
		names = [item["name"] for item in subtypes]
		self.assertTrue({"Folder", "Part", "ReplicatedStorage", "ServiceProvider"} <= set(names), names)
		self.assertEqual(names, sorted(names))


if __name__ == "__main__":
	unittest.main()
