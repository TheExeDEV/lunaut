-- A point in world space.
export type Point = {
	-- Horizontal coordinate.
	x: number,
	-- Vertical coordinate.
	y: number,
}

-- A moving point.
export type MovingPoint = Point & {
	-- Speed in studs per second.
	speed: number,
}

-- Adds two numbers.
declare function add(a: number, b: number): number
