--!strict
local util = {}

function util.add(a: number, b: number): number
	return a + b
end

function util.greet(name: string): string
	return "hello " .. name
end

return util
