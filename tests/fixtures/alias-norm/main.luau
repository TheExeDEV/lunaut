--!strict

type AdReward = any
type ContentId = string

type first = {
	name: string,
}

type second = {
	id: number,
}

local function compare(left: first, right: second): boolean
	return left.name == tostring(right.id)
end

type handler = (...any) -> ()
type observer<T = unknown> = {
	notify: (self: observer<T>, value: T) -> (),
}
type observer_var<T = unknown> = handler | thread | observer<T>

local function run(value: handler): { observer_var<any> }
	return { value }
end

const action = nil :: any
const id = nil :: string
const result = run(function() end)

print(action, id, result, compare)
