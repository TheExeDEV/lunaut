--!strict
local point: Point = { x = 1, y = 2 }
local moving: MovingPoint = { x = 1, y = 2, speed = 3 }
local sum: number = add(1, 2)
local object: LuaObject = { name = "Part" }

--[==[
    Multiplies two numbers.

    @param value Input value.
    @param factor Multiplier.
    @return Product.
]==]
local function multiply(value: number, factor: number): number
	return value * factor
end

local product = multiply(1, 2)
print(point.x, moving.speed, sum, object.name, product)
