--!strict

export type Wrapper<T = number> = {
	value: T,
}

local wrapped: Wrapper = {
	value = 1,
}

return wrapped
