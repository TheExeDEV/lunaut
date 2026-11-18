export type luaobject = {
	read iden: (self: luaobject) -> number,
}

export type nameable<T = string> = {
	name: T | string,
}

export type enumobject = {
	read name: string,
}

export type enumitem<T = enumobject?> = {
	read enum: T,
	read hashlink: string,
} & nameable & luaobject

export type enum_shared_net_variant = {
	read Unsafe: enumitem_shared_net_variant,
	read Safe: enumitem_shared_net_variant,
} & enumobject

export type enumitem_shared_net_variant = enumitem<enum_shared_net_variant>

declare Safe: enumitem_shared_net_variant
