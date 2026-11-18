--!strict
export type holder = { read table: {}, read name: string }

local holder: holder = { table = {}, name = "holder" }

return holder
