export type luaobject = {
	read as: <T>(self: luaobject, name: string) -> T & luaobject,
	read id: string,
}

export type nameable<T = string> = {
	name: T | string,
}

export type extend<extension = {}, base = luaobject> = extension & base

export type enumobject = extend<{
	read count: number,
	read name: string,
	read list: (self: enumobject) -> { [unknown]: marked },
}, luaobject>

export type marked<T = enumobject?> = extend<{
	read owner: T,
	read tag: string,
} & nameable, luaobject>
