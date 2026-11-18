--!strict

export type handler = (...any) -> ()
export type observer<T = unknown> = {
	notify: (self: observer<T>, value: T) -> (),
}
export type observer_var<T = unknown> = handler | thread | observer<T>

export type line = {
	read: (self: line) -> string,
}

export type tval = string | { read: string }

export type FuncDB = {
	write: <T>(self: FuncDB, func: T, rtype: tval, ...tval) -> T,
}
