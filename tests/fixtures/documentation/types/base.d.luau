-- A point in world space.
export type Point = {
	-- Horizontal coordinate.
	x: number,
	-- Vertical coordinate.
	y: number,
	-- Moves the point by the given offset.
	move: (delta: number) -> (),
}

-- A moving point.
export type MovingPoint = Point & {
	-- Speed in studs per second.
	speed: number,
}

-- A low-level object.
export type LuaObject = {
	-- Object name.
	name: string,
}

-- Adds two numbers.
declare function add(a: number, b: number): number
