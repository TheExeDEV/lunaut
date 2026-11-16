--!strict
local config = require("./config")
local shape = require("./shapes")
local node = require("./nodes")
print(config.maxPlayers, shape.count, node.value)