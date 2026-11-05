--!strict
local config = require("./config")
local limit: number = config.maxPlayers
print(limit, config.mode)
