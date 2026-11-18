--!strict

local Service = require("./service")

local scheduler = Service:whose()
local event = Service:get_event("data")
local status = Service:get_status()
local casted = Service:as<<Service.scheduler>>(nil)
local entries = Service:get_entries()
local watched = Service:as<<{ [string]: Service.scheduler }>>(nil)
local logger = Service:get_logger()

local function log(level: string, ...any): (...any)
	return ...
end

print(scheduler, event, status, casted, entries, watched, logger, log)
