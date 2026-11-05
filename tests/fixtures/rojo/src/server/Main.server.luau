--!strict
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local Util = require(ReplicatedStorage.Shared.Util)
local Helper = require(script.Parent.Helper)
local Sibling = require("./Helper")
local total: number = Util.double(21)
local text: string = Helper.describe(total)
print(text, Sibling.describe(1))
