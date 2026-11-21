--!strict

local HttpService = game:GetService("HttpService")
local RunService = game:GetService("RunService")

if not RunService:IsEdit() then
	return
end

local Sender = require(script.Sender)
local Tree = require(script.Tree)

local defaultPort = 3667
local portSetting = "LunautPort"
local syncSetting = "LunautSync"

local toolbar = plugin:CreateToolbar("Lunaut")
local syncButton = toolbar:CreateButton("LunautSync", "Keep the editor in sync with the instances of this place", "", "Sync")
local sendButton = toolbar:CreateButton("LunautSend", "Send the instances of this place to the editor once", "", "Send")
local clearButton = toolbar:CreateButton("LunautClear", "Remove the instances this plugin sent from the editor", "", "Clear")
local portButton = toolbar:CreateButton("LunautPort", "Change the port the editor listens on", "", "Port")

for _, button in { syncButton, sendButton, clearButton, portButton } do
	button.ClickableWhenViewportHidden = true
end

local widget = plugin:CreateDockWidgetPluginGuiAsync(
	"LunautPortWidget",
	DockWidgetPluginGuiInfo.new(Enum.InitialDockState.Float, false, true, 260, 90, 220, 80)
)
widget.Title = "Lunaut port"

local portBox = Instance.new("TextBox")
portBox.Size = UDim2.new(1, -16, 0, 28)
portBox.Position = UDim2.fromOffset(8, 8)
portBox.ClearTextOnFocus = false
portBox.PlaceholderText = tostring(defaultPort)
portBox.Parent = widget

local hint = Instance.new("TextLabel")
hint.Size = UDim2.new(1, -16, 0, 40)
hint.Position = UDim2.fromOffset(8, 40)
hint.BackgroundTransparency = 1
hint.TextWrapped = true
hint.TextXAlignment = Enum.TextXAlignment.Left
hint.Text = "The same number as lunaut.studioPlugin.port in your editor. Press Enter to save."
hint.Parent = widget

local syncing = false
local pending = false
local reachable: boolean? = nil
local connections: { RBXScriptConnection } = {}

local function getPort(): number
	return tonumber(plugin:GetSetting(portSetting)) or defaultPort
end

local function send(announce: boolean): boolean
	local port = getPort()
	local tree, count = Tree.build(game)
	local ok, problem = Sender.post(port, "/full", HttpService:JSONEncode({ tree = tree }))

	if ok and (announce or reachable ~= true) then
		print(`[Lunaut] sent {count} instances to the editor on port {port}`)
	elseif not ok and (announce or reachable ~= false) then
		warn(`[Lunaut] the editor did not answer on port {port} ({problem}). Turn on lunaut.studioPlugin.enabled in the editor and check the port.`)
	end

	reachable = ok
	return ok
end

local function schedule()
	if not syncing or pending then
		return
	end

	pending = true

	task.delay(0.5, function()
		pending = false

		if syncing then
			send(false)
		end
	end)
end

local function setSyncing(enabled: boolean)
	if syncing == enabled then
		return
	end

	syncing = enabled
	syncButton:SetActive(enabled)
	plugin:SetSetting(syncSetting, enabled)

	for _, connection in connections do
		connection:Disconnect()
	end

	table.clear(connections)

	if not enabled then
		print("[Lunaut] sync stopped")
		return
	end

	table.insert(connections, game.DescendantAdded:Connect(schedule))
	table.insert(connections, game.DescendantRemoving:Connect(schedule))

	reachable = nil
	send(true)
end

syncButton.Click:Connect(function()
	setSyncing(not syncing)
end)

sendButton.Click:Connect(function()
	send(true)
end)

clearButton.Click:Connect(function()
	local port = getPort()
	local ok, problem = Sender.post(port, "/clear")

	if ok then
		print("[Lunaut] cleared the instances in the editor")
	else
		warn(`[Lunaut] the editor did not answer on port {port} ({problem})`)
	end
end)

portButton.Click:Connect(function()
	portBox.Text = tostring(getPort())
	widget.Enabled = not widget.Enabled
end)

portBox.FocusLost:Connect(function(enterPressed: boolean)
	local port = tonumber(portBox.Text)

	if not enterPressed then
		return
	end

	if port == nil or port < 1 or port > 65535 or port % 1 ~= 0 then
		warn(`[Lunaut] {portBox.Text} is not a port number`)
		return
	end

	plugin:SetSetting(portSetting, port)
	print(`[Lunaut] port set to {port}`)
	widget.Enabled = false

	if syncing then
		reachable = nil
		send(true)
	end
end)

plugin.Unloading:Connect(function()
	for _, connection in connections do
		connection:Disconnect()
	end
end)

if plugin:GetSetting(syncSetting) == true then
	setSyncing(true)
end
