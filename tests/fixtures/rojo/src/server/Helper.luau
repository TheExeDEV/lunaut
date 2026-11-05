--!strict
local Helper = {}

function Helper.describe(value: number): string
	return "value: " .. tostring(value)
end

return Helper
