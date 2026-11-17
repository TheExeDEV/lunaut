--!strict

local Service = require("./service")

local scheduler = Service:whose()
local event = Service:get_event("data")
local status = Service:get_status()
local casted = Service:as<<Service.scheduler>>(nil)
local entries = Service:get_entries()
local watched = Service:as<<{ [string]: Service.scheduler }>>(nil)

print(scheduler, event, status, casted, entries, watched)
