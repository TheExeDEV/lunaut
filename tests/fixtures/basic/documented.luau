--!strict
--[=[
	Doubles a number.

	@param value number to double
	@return the doubled number
]=]
local function double(value: number): number
	return value * 2
end

--- Greets someone by name.
local function greet(name: string): string
	return "hi " .. name
end

print(double(2), greet("lunaut"))
