--!strict

type entry = {
	name: string,
}

local source: { [string]: entry } = {
	a = { name = "a" },
}

local function collect(): { entry }
	local known = source["a"] or {}
	return { known }
end

print(collect())
