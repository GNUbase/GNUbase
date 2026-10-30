// SPDX-License-Identifier: GPL-3.0-or-later
#include "mcp/server.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace gnubase::mcp
{
	using engine::CommandInfo;
	using engine::Json;

	namespace
	{
		// JSON-RPC and MCP error codes.
		constexpr int g_parseError = -32700;
		constexpr int g_invalidRequest = -32600;
		constexpr int g_methodNotFound = -32601;
		constexpr int g_invalidParams = -32602;
		constexpr int g_internalError = -32603;
		constexpr int g_resourceNotFound = -32002;

		// Long enough for an offline render, which runs as one command.
		constexpr std::chrono::minutes g_callTimeout{10};

		// events_take waits at most this long, less than clients wait for a tool.
		constexpr double g_maxEventWait = 60.0;
		constexpr const char* g_eventsTool = "events_take";

		constexpr std::string_view g_resourceScheme = "gnubase://";

		class ProtocolError : public std::runtime_error
		{
		public:
			ProtocolError(int code, const std::string& message) : std::runtime_error(message), m_code(code) {}

			int code() const { return m_code; }

		private:
			int m_code;
		};

		// Compact JSON; invalid UTF-8 in strings is replaced rather than thrown on.
		std::string text(const Json& value)
		{
			return value.dump(-1, ' ', false, Json::error_handler_t::replace);
		}

		Json errorResponse(const Json& id, int code, const std::string& message)
		{
			return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
		}

		Json textContent(const std::string& value)
		{
			return Json::array({Json{{"type", "text"}, {"text", value}}});
		}

		std::string base64(const std::vector<uint8_t>& bytes)
		{
			constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string text;
			text.reserve((bytes.size() + 2) / 3 * 4);
			for (size_t i = 0; i < bytes.size(); i += 3)
			{
				const size_t count = std::min<size_t>(3, bytes.size() - i);
				uint32_t bits = static_cast<uint32_t>(bytes[i]) << 16U;
				if (count > 1)
					bits |= static_cast<uint32_t>(bytes[i + 1]) << 8U;
				if (count > 2)
					bits |= bytes[i + 2];
				for (size_t c = 0; c < 4; ++c)
					text += c <= count ? alphabet[(bits >> (18 - (6 * c))) & 0x3FU] : '=';
			}
			return text;
		}

		std::string toolName(std::string command)
		{
			std::replace(command.begin(), command.end(), '.', '_');
			return command;
		}

		bool isResource(const CommandInfo& info)
		{
			return info.query && info.parameters.value("required", Json::array()).empty();
		}

		std::string resourceUri(const CommandInfo& info)
		{
			return std::string(g_resourceScheme) + info.name;
		}

		const std::string& stringParameter(const Json& params, const char* name, const char* method)
		{
			const auto value = params.find(name);
			if (value == params.end() || !value->is_string())
				throw ProtocolError(g_invalidParams, std::string(method) + " needs " + name);
			return value->get_ref<const std::string&>();
		}
	}

	Server::Server(engine::Engine& engine, SessionLimits limits) : m_engine(engine), m_sessions(engine, limits)
	{
		// Runs in Server::takeEvents, which knows the session.
		addTool({g_eventsTool,
			"Takes the engine events of this session that haven't been taken yet, oldest first, each with name "
			"(e.g. track.added), data (what changed) and request (the engine request that caused it). Events come "
			"from every client: this one, other MCP clients, Lua scripts and the UI. names takes only events with "
			"these names and leaves the others queued. seconds (0 to 60, default 0) waits up to that long for the "
			"first event. A session keeps the newest " +
				std::to_string(limits.queuedEvents) +
				" events; dropped counts the older ones dropped since the last call. A session starts with "
				"initialize; over HTTP it is the one of the Mcp-Session-Id header.",
			Json::parse(R"({"type": "object", "additionalProperties": false, "properties": {
				"names": {"type": "array", "items": {"type": "string"}}, "seconds": {"type": "number"}}})"),
			Json::parse(R"({"type": "object", "required": ["events", "dropped"], "properties": {
				"events": {"type": "array", "items": {"type": "object", "required": ["name", "data", "request"],
					"properties": {"name": {"type": "string"}, "data": {}, "request": {"type": "integer"}}}},
				"dropped": {"type": "integer"}}})"),
			true, nullptr});
	}

	std::string Server::startSession(bool expires)
	{
		return m_sessions.start(expires);
	}

	bool Server::endSession(const std::string& id)
	{
		return m_sessions.end(id);
	}

	bool Server::hasSession(const std::string& id)
	{
		return m_sessions.find(id) != nullptr;
	}

	std::optional<Json> Server::handle(const Json& message)
	{
		return respond(message, nullptr);
	}

	std::optional<Json> Server::handle(const Json& message, std::string& session)
	{
		return respond(message, &session);
	}

	std::optional<std::string> Server::handle(std::string_view message)
	{
		return respond(message, nullptr);
	}

	std::optional<std::string> Server::handle(std::string_view message, std::string& session)
	{
		return respond(message, &session);
	}

	std::optional<Json> Server::respond(const Json& message, std::string* session)
	{
		if (!message.is_object())
			return errorResponse(nullptr, g_invalidRequest,
				message.is_array() ? "batches are not supported" : "a message must be a JSON object");
		const auto method = message.find("method");
		if (method == message.end())
			return std::nullopt; // a response; this server sends no requests
		const Json id = message.contains("id") ? message.at("id") : Json();
		const auto version = message.find("jsonrpc");
		if (version == message.end() || *version != "2.0" || !method->is_string())
			return errorResponse(id, g_invalidRequest, "not a JSON-RPC 2.0 request");

		const bool notification = !message.contains("id");
		try
		{
			Json result = dispatch(method->get<std::string>(), message.value("params", Json::object()), session);
			if (notification)
				return std::nullopt;
			return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
		}
		catch (const ProtocolError& error)
		{
			if (notification)
				return std::nullopt;
			return errorResponse(id, error.code(), error.what());
		}
		catch (const std::exception& error)
		{
			if (notification)
				return std::nullopt;
			return errorResponse(id, g_internalError, error.what());
		}
	}

	std::optional<std::string> Server::respond(std::string_view message, std::string* session)
	{
		const Json parsed = Json::parse(message, nullptr, false);
		if (parsed.is_discarded())
			return text(errorResponse(nullptr, g_parseError, "not valid JSON"));
		const std::optional<Json> response = respond(parsed, session);
		if (!response)
			return std::nullopt;
		return text(*response);
	}

	Json Server::dispatch(const std::string& method, const Json& params, std::string* session)
	{
		if (method == "initialize")
		{
			if (session != nullptr && session->empty())
				*session = m_sessions.start(true);
			// One revision only: a client that asked for another decides whether it can use this one.
			return {{"protocolVersion", g_protocolVersion},
				{"capabilities",
					{{"tools", {{"listChanged", false}}},
						{"resources", {{"subscribe", false}, {"listChanged", false}}}}},
				{"serverInfo", {{"name", "gnubase"}, {"title", "GNUbase"}, {"version", engine::version()}}},
				{"instructions",
					"GNUbase is a digital audio workstation. Every tool runs one engine command; tools marked "
					"read-only "
					"only read. engine_commands lists every command with its documentation."}};
		}
		if (method == "ping")
			return Json::object();
		if (method == "tools/list")
			return listTools();
		if (method == "tools/call")
			return callTool(params, session != nullptr ? *session : std::string());
		if (method == "resources/list")
			return listResources();
		if (method == "resources/read")
			return readResource(params);
		if (method == "resources/templates/list")
			return {{"resourceTemplates", Json::array()}};
		if (method.rfind("notifications/", 0) == 0)
			return nullptr; // initialized, cancelled: nothing to do
		throw ProtocolError(g_methodNotFound, "no method " + method);
	}

	void Server::addTool(Tool tool)
	{
		m_tools.push_back(std::move(tool));
	}

	Json Server::listTools() const
	{
		Json tools = Json::array();
		for (const Tool& extra : m_tools)
		{
			Json tool = {{"name", extra.name}, {"description", extra.description}, {"inputSchema", extra.inputSchema},
				{"annotations", {{"readOnlyHint", extra.readOnly}}}};
			if (!extra.outputSchema.is_null())
				tool["outputSchema"] = extra.outputSchema;
			tools.push_back(std::move(tool));
		}
		for (const CommandInfo& info : m_engine.commands())
		{
			Json tool = {{"name", toolName(info.name)}, {"title", info.name}, {"description", info.description},
				{"inputSchema", info.parameters}, {"annotations", {{"readOnlyHint", info.query}}}};
			if (info.result.value("type", Json()) == "object")
				tool["outputSchema"] = info.result;
			tools.push_back(std::move(tool));
		}
		return {{"tools", std::move(tools)}};
	}

	Json Server::callTool(const Json& params, const std::string& session)
	{
		const std::string& name = stringParameter(params, "name", "tools/call");
		const Json arguments = params.value("arguments", Json::object());
		if (const auto extra =
				std::find_if(m_tools.begin(), m_tools.end(), [&name](const Tool& tool) { return tool.name == name; });
			extra != m_tools.end())
		{
			try
			{
				if (const std::string problem = engine::validate(arguments, extra->inputSchema, "arguments");
					!problem.empty())
					throw std::invalid_argument(problem);
				Json value = extra->name == g_eventsTool ? takeEvents(arguments, session) : extra->run(arguments);
				if (value.is_binary())
					return {{"content",
								Json::array({Json{{"type", "image"}, {"data", base64(value.get_binary())},
									{"mimeType", "image/png"}}})},
						{"isError", false}};
				Json response = {{"content", textContent(text(value))}, {"isError", false}};
				response["structuredContent"] = std::move(value);
				return response;
			}
			catch (const std::exception& error)
			{
				return {{"content", textContent(error.what())}, {"isError", true}};
			}
		}

		const std::vector<CommandInfo> commands = m_engine.commands();
		const auto command = std::find_if(
			commands.begin(), commands.end(), [&name](const CommandInfo& info) { return toolName(info.name) == name; });
		if (command == commands.end())
			throw ProtocolError(g_invalidParams, "no tool " + name);

		const engine::Result result = engine::Client(m_engine).call(command->name, arguments, g_callTimeout);
		if (!result.ok())
			return {{"content", textContent(result.error)}, {"isError", true}};
		Json response = {{"content", textContent(text(result.value))}, {"isError", false}};
		if (result.value.is_object())
			response["structuredContent"] = result.value;
		return response;
	}

	Json Server::takeEvents(const Json& arguments, const std::string& session)
	{
		const std::shared_ptr<EventQueue> events = session.empty() ? nullptr : m_sessions.find(session);
		if (!events)
			throw std::runtime_error(session.empty() ? "events_take needs a session, which initialize starts"
													 : "the session ended; initialize starts a new one");
		const double seconds = arguments.value("seconds", 0.0);
		if (seconds < 0.0 || seconds > g_maxEventWait)
			throw std::invalid_argument("seconds must be 0 to 60");
		const EventQueue::Taken taken = events->take(arguments.value("names", std::vector<std::string>()),
			std::chrono::milliseconds(std::llround(seconds * 1000.0)));
		Json list = Json::array();
		for (const engine::Event& event : taken.events)
			list.push_back({{"name", event.name}, {"data", event.data}, {"request", event.request}});
		return {{"events", std::move(list)}, {"dropped", taken.dropped}};
	}

	Json Server::listResources() const
	{
		Json resources = Json::array();
		for (const CommandInfo& info : m_engine.commands())
		{
			if (isResource(info))
				resources.push_back({{"uri", resourceUri(info)}, {"name", info.name}, {"description", info.description},
					{"mimeType", "application/json"}});
		}
		return {{"resources", std::move(resources)}};
	}

	Json Server::readResource(const Json& params)
	{
		const std::string& uri = stringParameter(params, "uri", "resources/read");
		const std::vector<CommandInfo> commands = m_engine.commands();
		const auto command = std::find_if(commands.begin(), commands.end(),
			[&uri](const CommandInfo& info) { return isResource(info) && resourceUri(info) == uri; });
		if (command == commands.end())
			throw ProtocolError(g_resourceNotFound, "no resource " + uri);

		const engine::Result result = engine::Client(m_engine).call(command->name, Json::object(), g_callTimeout);
		if (!result.ok())
			throw ProtocolError(g_internalError, result.error);
		return {{"contents",
			Json::array({Json{{"uri", uri}, {"mimeType", "application/json"}, {"text", text(result.value)}}})}};
	}
}
