--!strict
local config = require("./config")
local shape = require("./shapes")
local node = require("./nodes")
local factory = require("./factory")
local made = factory()
print(config.maxPlayers, shape.count, node.value, made.count)