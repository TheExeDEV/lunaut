--!strict

local HttpService = game:GetService("HttpService")

local Sender = {}

Sender.hosts = { "127.0.0.1", "localhost" }

function Sender.post(port: number, path: string, body: string?): (boolean, string?)
	local lastError = "no host answered"

	for _, host in Sender.hosts do
		local request: any = {
			Url = `http://{host}:{port}{path}`,
			Method = "POST",
			Headers = { ["Content-Type"] = "application/json" },
			Body = body or "{}",
		}
		local ok, response = pcall(HttpService.RequestAsync, HttpService, request)

		if ok and response.Success then
			return true, nil
		end

		lastError = if ok then `{response.StatusCode} {response.StatusMessage}` else tostring(response)
	end

	return false, lastError
end

return Sender
