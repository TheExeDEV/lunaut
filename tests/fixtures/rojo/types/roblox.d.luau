--#METADATA#{"CREATABLE_INSTANCES": ["Folder", "Part"], "SERVICES": ["ReplicatedStorage", "ServerScriptService"]}
declare extern type Enum with
	function GetEnumItems(self): { any }
end

declare extern type EnumItem with
	Name: string
	Value: number
end

declare extern type EnumMaterial extends EnumItem with end
declare extern type EnumMaterial_INTERNAL extends Enum with
	Plastic: EnumMaterial
	Wood: EnumMaterial
end

type ENUM_LIST = {
	Material: EnumMaterial_INTERNAL,
} & { GetEnums: (self: any) -> { Enum } }

declare Enum: ENUM_LIST

declare extern type Object with
	ClassName: string
	function IsA(self, className: string): boolean
end

declare extern type Instance extends Object with
	Name: string
	Parent: Instance?
	function FindFirstChild(self, name: string, recursive: boolean?): Instance?
	function WaitForChild(self, name: string, timeout: number?): Instance
	function Clone(self): Instance
	function FindFirstChildWhichIsA(self, className: string, recursive: boolean?): Instance?
end

declare extern type Folder extends Instance with
end

declare extern type Part extends Instance with
	Anchored: boolean
	Material: EnumMaterial
end

declare extern type LuaSourceContainer extends Instance with
end

declare extern type ModuleScript extends LuaSourceContainer with
end

declare extern type Script extends LuaSourceContainer with
end

declare extern type ServiceProvider extends Instance with
	function GetService(self, service: "ReplicatedStorage"): ReplicatedStorage
	function GetService(self, service: "ServerScriptService"): ServerScriptService
	function GetService(self, service: string): Instance
end

declare extern type DataModel extends ServiceProvider with
end

declare extern type ReplicatedStorage extends Instance with
end

declare extern type ServerScriptService extends Instance with
end

declare Instance: {
	new: ((className: string, parent: Instance?) -> Instance),
}

declare game: DataModel
declare workspace: Instance
declare script: LuaSourceContainer
