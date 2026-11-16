--!strict
local point: Point = { x = 1, y = 2 }
local moving: MovingPoint = { x = 1, y = 2, speed = 3 }
local sum: number = add(1, 2)
local object: LuaObject = { name = "Part" }
print(point.x, moving.speed, sum, object.name)
