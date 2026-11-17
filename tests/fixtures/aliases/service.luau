--!strict

export type scheduler = {
	cancel: (self: scheduler) -> (),
}

type net_event = {
	name: string,
}

type status = "safe" | "unsafe"

export type service = {
	whose: (self: service) -> scheduler?,
	get_event: (self: service, name: string) -> net_event,
	get_status: (self: service) -> status?,
	get_entries: (self: service) -> { net_event },
	as: <T>(self: service, value: any) -> T,
}

local events: { [string]: net_event } = {
	data = { name = "data" },
}

local service: service = {
	whose = function(self: service): scheduler?
		return nil
	end,
	get_event = function(self: service, name: string): net_event
		return events[name]
	end,
	get_status = function(self: service): status?
		return nil
	end,
	get_entries = function(self: service): { net_event }
		return {}
	end,
	as = function<T>(self: service, value: any): T
		return value
	end,
}

local existing = events["data"]

return service
