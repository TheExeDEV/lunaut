--!strict
local car = garage[1]
local brand: string = car.brand
local travelled: number = car:drive(10)
local sound: string = honk(2)
print(brand, travelled, sound)
