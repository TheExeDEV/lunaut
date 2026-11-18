--!strict
const Direction = require("./direction")
local holder = require("./holder")

local function pick(value: number): number
	if value == Direction.Up then
		return 1
	elseif value == Direction.Left then
		return 2
	elseif value == Direction.Bottom then
		return 3
	end

	return 0
end

local function empty()
	return holder.table
end

print(pick(1), empty())
