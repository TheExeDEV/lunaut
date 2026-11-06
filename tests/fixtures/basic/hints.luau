--!strict
local function scale(value: number, factor: number)
	return value * factor
end

local result = scale(2, 3)
local label = "lunaut"
for index, item in ipairs({ "a", "b" }) do
	print(index, item)
end
print(result, label)
