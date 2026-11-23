--!strict

export type Filter = {
	include: { Instance }?,
	exclude: { Instance },
}

local Settings = {}

Settings.name = "Lunaut_Settings"
Settings.service = "ServerStorage"

Settings.template = [[
--!strict
return {
	include = {
	},
	exclude = {
	},
}
]]

local function getService(): Instance?
	local ok, service = pcall(game.GetService, game, Settings.service)
	return if ok then service else nil
end

function Settings.find(): ModuleScript?
	local service = getService()
	local found = service and service:FindFirstChild(Settings.name)

	if found and found:IsA("ModuleScript") then
		return found
	end

	return nil
end

local function readList(value: unknown, field: string): ({ Instance }?, string?)
	if value == nil then
		return nil, nil
	end

	if type(value) ~= "table" then
		return nil, `{field} should be a list of instances`
	end

	local list: { Instance } = {}

	for index, item in value :: { [unknown]: unknown } do
		if typeof(item) ~= "Instance" then
			return nil, `{field}[{tostring(index)}] is not an instance`
		end

		table.insert(list, item :: Instance)
	end

	return list, nil
end

function Settings.load(): (Filter?, string?)
	local module = Settings.find()

	if module == nil then
		return nil, nil
	end

	local copy = module:Clone()
	local ok, result = pcall(require, copy)
	copy:Destroy()

	if not ok then
		return nil, `{Settings.name} failed to run: {result}`
	end

	if type(result) ~= "table" then
		return nil, `{Settings.name} should return a table with include and exclude`
	end

	local include, includeProblem = readList(result.include, "include")

	if includeProblem then
		return nil, `{Settings.name}: {includeProblem}`
	end

	local exclude, excludeProblem = readList(result.exclude, "exclude")

	if excludeProblem then
		return nil, `{Settings.name}: {excludeProblem}`
	end

	return {
		include = if include and #include > 0 then include else nil,
		exclude = exclude or {},
	}, nil
end

function Settings.open(plugin: Plugin): ModuleScript?
	local module = Settings.find()

	if module == nil then
		local service = getService()

		if service == nil then
			return nil
		end

		local created = Instance.new("ModuleScript")
		created.Name = Settings.name
		created.Source = Settings.template
		created.Parent = service
		module = created
	end

	plugin:OpenScript(module :: ModuleScript)
	return module
end

function Settings.isWatched(filter: Filter?, instance: Instance): boolean
	if filter == nil then
		return true
	end

	for _, excluded in filter.exclude do
		if instance == excluded or instance:IsDescendantOf(excluded) then
			return false
		end
	end

	if filter.include == nil then
		return true
	end

	for _, included in filter.include do
		if instance == included or instance:IsDescendantOf(included) then
			return true
		end
	end

	return false
end

return Settings
