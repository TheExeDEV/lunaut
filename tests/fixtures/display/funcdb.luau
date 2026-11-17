--!strict

export type tval = string | { read: string }

export type FuncDB = {
	write: <T>(self: FuncDB, func: T, rtype: tval, ...tval) -> T,
}

local class: FuncDB = {
	write = function<T>(self: FuncDB, func: T, rtype: tval, ...tval): T
		return func
	end,
}

return class
