--!strict
local tools = {}

function tools.clamp(value: number, low: number, high: number): number
	return math.max(low, math.min(high, value))
end

return tools
