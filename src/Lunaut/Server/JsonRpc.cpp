#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include <istream>
#include <ostream>

namespace lunaut
{
namespace
{
namespace priv
{

constexpr std::string_view content_length = "content-length:";

}
}

jsonrpc::jsonrpc(std::istream& input, std::ostream& output)
	: input(input)
	, output(output)
{
}

result<json> jsonrpc::read_message()
{
	size_t length = 0;
	std::string line;

	while (std::getline(input, line))
	{
		if (!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}

		if (line.empty())
		{
			break;
		}

		const std::string lowered = stdx::to_lower(line);
		if (stdx::starts_with(lowered, priv::content_length))
		{
			length = static_cast<size_t>(std::stoull(stdx::trim(line.substr(priv::content_length.size()))));
		}
	}

	if (!input)
	{
		return result<json>::fail("input stream closed");
	}

	if (length == 0 || length > CORE_JSONRPC_MAX_MESSAGE)
	{
		return result<json>::fail("invalid Content-Length");
	}

	std::string body(length, '\0');
	input.read(body.data(), static_cast<std::streamsize>(length));

	if (static_cast<size_t>(input.gcount()) != length)
	{
		return result<json>::fail("unexpected end of message body");
	}

	try
	{
		return result<json>::ok(json::parse(body));
	}
	catch (const json::exception& error)
	{
		return result<json>::fail(std::string("malformed json: ") + error.what());
	}
}

void jsonrpc::send_message(const json& message)
{
	const std::string body = message.dump(-1, ' ', false, json::error_handler_t::replace);

	std::lock_guard<std::mutex> guard(lock);
	output << "Content-Length: " << body.size() << "\r\n\r\n" << body;
	output.flush();
}

void jsonrpc::send_response(const json& id, const json& value)
{
	send_message({{"jsonrpc", "2.0"}, {"id", id}, {"result", value}});
}

void jsonrpc::send_error(const json& id, enumitem_error_code code, const std::string& message)
{
	send_message({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", static_cast<int>(code)}, {"message", message}}}});
}

void jsonrpc::send_notification(const std::string& method, const json& params)
{
	send_message({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}

void jsonrpc::send_request(const std::string& method, const json& params)
{
	send_message({{"jsonrpc", "2.0"}, {"id", next_id++}, {"method", method}, {"params", params}});
}

}
