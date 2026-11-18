--!strict

-- Recursive node.
export type Node = {
	value: number,
	next: Node?,
}

local node: Node = { value = 1, next = nil }

return node