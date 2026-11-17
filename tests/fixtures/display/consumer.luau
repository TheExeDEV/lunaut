--!strict

local Types = require("./types")
local FuncDB = require("./funcdb")
local Service = require("./service")

local scheduler = Service:whose()
local write = FuncDB.write

print(scheduler, write)
