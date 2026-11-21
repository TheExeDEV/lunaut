--!strict

local HttpService = game:GetService("HttpService")
local RunService = game:GetService("RunService")

if not RunService:IsEdit() then
	return
end

local Sender = require(script.Sender)
local Tree = require(script.Tree)
local Ui = require(script.Ui)

local defaultPort = 3667
local portSetting = "LunautPort"
local autoSyncSetting = "LunautSync"
local silentSetting = "LunautSilent"
local openSetting = "LunautOpenOnStart"

local toolbar = plugin:CreateToolbar("Lunaut")
local openButton = toolbar:CreateButton("LunautOpen", "Open the Lunaut window", "", "Lunaut")
openButton.ClickableWhenViewportHidden = true

local widget = plugin:CreateDockWidgetPluginGuiAsync(
	"LunautWindow",
	DockWidgetPluginGuiInfo.new(Enum.InitialDockState.Float, false, false, 320, 560, 260, 320)
)
widget.Title = "Lunaut"
widget.Name = "Lunaut"

local root = Instance.new("ScrollingFrame")
root.Size = UDim2.fromScale(1, 1)
root.BorderSizePixel = 0
root.ScrollBarThickness = 6
root.CanvasSize = UDim2.new()
root.AutomaticCanvasSize = Enum.AutomaticSize.Y
root:SetAttribute("Role", "Background")
root.Parent = widget

local padding = Instance.new("UIPadding")
padding.PaddingTop = UDim.new(0, 14)
padding.PaddingBottom = UDim.new(0, 14)
padding.PaddingLeft = UDim.new(0, 14)
padding.PaddingRight = UDim.new(0, 18)
padding.Parent = root

local layout = Instance.new("UIListLayout")
layout.Padding = UDim.new(0, 10)
layout.SortOrder = Enum.SortOrder.LayoutOrder
layout.Parent = root

local title = Ui.label(root, "Lunaut", 30, true, 1)
title.TextXAlignment = Enum.TextXAlignment.Center

local primary = Ui.button(root, "Start Sync", 40, 2)
primary:SetAttribute("Role", "Primary")

local status = Ui.label(root, "Not syncing", 15, false, 3)
status:SetAttribute("Role", "Hint")

Ui.label(root, "Behavior", 20, true, 4)
local autoSync = Ui.checkbox(root, "Sync on Start", "Start syncing as soon as Studio opens a place.", 5)
local silent = Ui.checkbox(root, "Silent Mode", "Only errors go to the Output window.", 6)
local openOnStart = Ui.checkbox(root, "Open on Start", "Open this window when Studio starts.", 7)

Ui.label(root, "Actions", 20, true, 8)

local actions = Instance.new("Frame")
actions.BackgroundTransparency = 1
actions.Size = UDim2.new(1, 0, 0, 34)
actions.LayoutOrder = 9
actions.Parent = root

local actionLayout = Instance.new("UIListLayout")
actionLayout.FillDirection = Enum.FillDirection.Horizontal
actionLayout.Padding = UDim.new(0, 8)
actionLayout.Parent = actions

local sendButton = Ui.button(actions, "Send Once", 34, 1)
sendButton.Size = UDim2.new(0.5, -4, 1, 0)
sendButton:SetAttribute("Role", "Secondary")

local clearButton = Ui.button(actions, "Clear Editor", 34, 2)
clearButton.Size = UDim2.new(0.5, -4, 1, 0)
clearButton:SetAttribute("Role", "Secondary")

Ui.label(root, "Connection", 20, true, 10)
Ui.label(root, "Port", 17, false, 11)

local portBox = Instance.new("TextBox")
portBox.Size = UDim2.new(1, 0, 0, 32)
portBox.ClearTextOnFocus = false
portBox.Font = Enum.Font.SourceSans
portBox.TextSize = 18
portBox.TextXAlignment = Enum.TextXAlignment.Left
portBox.PlaceholderText = tostring(defaultPort)
portBox.LayoutOrder = 12
portBox:SetAttribute("Role", "Field")
Ui.corner(portBox, 4)
portBox.Parent = root

local portPadding = Instance.new("UIPadding")
portPadding.PaddingLeft = UDim.new(0, 8)
portPadding.Parent = portBox

local portStroke = Instance.new("UIStroke")
portStroke.ApplyStrokeMode = Enum.ApplyStrokeMode.Border
portStroke.Parent = portBox

local portHint = Ui.label(root, "The same number as lunaut.studioPlugin.port in your editor. Press Enter to save.", 15, false, 13)
portHint:SetAttribute("Role", "Hint")

local syncing = false
local pending = false
local reachable: boolean? = nil
local connections: { RBXScriptConnection } = {}

local function getPort(): number
	return tonumber(plugin:GetSetting(portSetting)) or defaultPort
end

local function repaint()
	Ui.paint(root, Ui.getTheme())
end

local function say(text: string)
	if plugin:GetSetting(silentSetting) ~= true then
		print(`[Lunaut] {text}`)
	end
end

local function send(announce: boolean): boolean
	local port = getPort()
	local tree, count = Tree.build(game)
	local ok, problem = Sender.post(port, "/full", HttpService:JSONEncode({ tree = tree }))

	if ok then
		status.Text = `Sent {count} instances to port {port} at {os.date("%H:%M:%S")}`

		if announce or reachable ~= true then
			say(`sent {count} instances to the editor on port {port}`)
		end
	else
		status.Text = `The editor did not answer on port {port}`

		if announce or reachable ~= false then
			warn(`[Lunaut] the editor did not answer on port {port} ({problem}). Turn on lunaut.studioPlugin.enabled in the editor and check the port.`)
		end
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
	primary.Text = if enabled then "Stop Sync" else "Start Sync"
	primary:SetAttribute("Active", enabled)
	repaint()

	for _, connection in connections do
		connection:Disconnect()
	end

	table.clear(connections)

	if not enabled then
		status.Text = "Not syncing"
		say("sync stopped")
		return
	end

	table.insert(connections, game.DescendantAdded:Connect(schedule))
	table.insert(connections, game.DescendantRemoving:Connect(schedule))
	reachable = nil
	send(true)
end

primary.Activated:Connect(function()
	setSyncing(not syncing)
end)

sendButton.Activated:Connect(function()
	send(true)
end)

clearButton.Activated:Connect(function()
	local port = getPort()
	local ok, problem = Sender.post(port, "/clear")

	if ok then
		status.Text = "Cleared the instances in the editor"
		say("cleared the instances in the editor")
	else
		status.Text = `The editor did not answer on port {port}`
		warn(`[Lunaut] the editor did not answer on port {port} ({problem})`)
	end
end)

local function bindSetting(checkbox: Ui.Checkbox, key: string)
	checkbox.set(plugin:GetSetting(key) == true)

	checkbox.changed.Event:Connect(function(checked: boolean)
		plugin:SetSetting(key, checked)
		repaint()
	end)
end

bindSetting(autoSync, autoSyncSetting)
bindSetting(silent, silentSetting)
bindSetting(openOnStart, openSetting)

portBox.Text = tostring(getPort())

portBox.FocusLost:Connect(function(enterPressed: boolean)
	local port = tonumber(portBox.Text)

	if not enterPressed then
		portBox.Text = tostring(getPort())
		return
	end

	if port == nil or port < 1 or port > 65535 or port % 1 ~= 0 then
		warn(`[Lunaut] {portBox.Text} is not a port number`)
		portBox.Text = tostring(getPort())
		return
	end

	plugin:SetSetting(portSetting, port)
	say(`port set to {port}`)

	if syncing then
		reachable = nil
		send(true)
	end
end)

openButton.Click:Connect(function()
	widget.Enabled = not widget.Enabled
end)

widget:GetPropertyChangedSignal("Enabled"):Connect(function()
	openButton:SetActive(widget.Enabled)
end)

local themeConnection = settings().Studio.ThemeChanged:Connect(repaint)

plugin.Unloading:Connect(function()
	themeConnection:Disconnect()

	for _, connection in connections do
		connection:Disconnect()
	end
end)

repaint()

if plugin:GetSetting(openSetting) == true then
	widget.Enabled = true
end

openButton:SetActive(widget.Enabled)

if plugin:GetSetting(autoSyncSetting) == true then
	setSyncing(true)
end
