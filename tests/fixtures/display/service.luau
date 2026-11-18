--!strict

local Types = require("./types")

export type scheduler = {
	whose: (self: scheduler) -> scheduler?,
	target: string | Types.line,
}

local class: scheduler = {
	whose = function(self: scheduler): scheduler?
		return nil
	end,
	target = "" :: any,
}

const result: { Types.observer_var } = {}

local function factory()
	return result
end

local viewed = class.target

return class
