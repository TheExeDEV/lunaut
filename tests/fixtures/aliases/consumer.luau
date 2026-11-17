--!strict

local Service = require("./service")

local scheduler = Service:whose()
local event = Service:get_event("data")
local status = Service:get_status()
local casted = Service:as<<Service.scheduler>>(nil)

print(scheduler, event, status, casted)
