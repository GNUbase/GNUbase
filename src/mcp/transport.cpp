// SPDX-License-Identifier: GPL-3.0-or-later
#include "mcp/transport.h"

#include "mcp/server.h"
#include "platform/log.h"

#include <httplib.h>

#include <istream>
#include <optional>
#include <ostream>
#include <random>
#include <stdexcept>

namespace gnubase::mcp
{
	namespace
	{
		constexpr std::string_view g_hexDigits = "0123456789abcdef";
		constexpr const char* g_sessionHeader = "Mcp-Session-Id";

		bool startsWith(std::string_view text, std::string_view prefix)
		{
			return text.substr(0, prefix.size()) == prefix;
		}

		// Compares every character, so the time taken doesn't tell how much of a guess was right.
		bool sameSecret(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size())
				return false;
			unsigned difference = 0;
			for (size_t i = 0; i < a.size(); ++i)
				difference |=
					static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
			return difference == 0;
		}

		// Browsers send the Origin of the page that makes a request. Only pages from this machine may use the server.
		bool isLocalOrigin(std::string_view origin)
		{
			for (const std::string_view scheme : {std::string_view("http://"), std::string_view("https://")})
			{
				if (!startsWith(origin, scheme))
					continue;
				std::string_view host = origin.substr(scheme.size());
				const bool bracketed = !host.empty() && host.front() == '[';
				const size_t end = bracketed ? host.find(']') : host.find_first_of(":/");
				host = host.substr(0, bracketed && end != std::string_view::npos ? end + 1 : end);
				return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
			}
			return false;
		}
	}

	void serveStdio(Server& server, std::istream& in, std::ostream& out)
	{
		std::string session = server.startSession(false);
		for (std::string line; std::getline(in, line);)
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty())
				continue;
			if (const std::optional<std::string> response = server.handle(std::string_view(line), session))
				out << *response << '\n' << std::flush;
		}
		server.endSession(session);
	}

	HttpServer::HttpServer(Server& server, const std::string& token, int port)
		: m_http(std::make_unique<httplib::Server>())
	{
		httplib::Server& http = *m_http;
		http.set_pre_routing_handler(
			[expected = "Bearer " + token](const httplib::Request& request, httplib::Response& response)
			{
				int status = 0;
				if (!isLoopback(request.remote_addr) ||
					(request.has_header("Origin") && !isLocalOrigin(request.get_header_value("Origin"))))
					status = 403;
				else if (!sameSecret(request.get_header_value("Authorization"), expected))
					status = 401;
				else if (request.has_header("MCP-Protocol-Version") &&
					request.get_header_value("MCP-Protocol-Version") != g_protocolVersion &&
					request.get_header_value("MCP-Protocol-Version") != "2025-03-26")
					status = 400;
				if (status == 0)
					return httplib::Server::HandlerResponse::Unhandled;
				response.status = status;
				return httplib::Server::HandlerResponse::Handled;
			});
		http.Post("/mcp",
			[&server](const httplib::Request& request, httplib::Response& response)
			{
				std::string session = request.get_header_value(g_sessionHeader);
				if (!session.empty() && !server.hasSession(session))
				{
					response.status = 404; // it ended; the client starts a new one with initialize
					return;
				}
				if (const std::optional<std::string> answer = server.handle(std::string_view(request.body), session))
					response.set_content(*answer, "application/json");
				else
					response.status = 202;
				if (!session.empty())
					response.set_header(g_sessionHeader, session);
			});
		http.Get("/mcp",
			[](const httplib::Request&, httplib::Response& response)
			{
				response.status = 405;
				response.set_header("Allow", "POST, DELETE");
			});
		http.Delete("/mcp",
			[&server](const httplib::Request& request, httplib::Response& response)
			{
				const std::string session = request.get_header_value(g_sessionHeader);
				if (session.empty())
					response.status = 400;
				else
					response.status = server.endSession(session) ? 204 : 404;
			});

		if (port == 0)
			m_port = http.bind_to_any_port("127.0.0.1");
		else if (http.bind_to_port("127.0.0.1", port))
			m_port = port;
		if (m_port <= 0)
			throw std::runtime_error("the MCP server cannot listen on port " + std::to_string(port));
		m_thread = std::thread([&http] { http.listen_after_bind(); });
		http.wait_until_ready(); // so stop() in the destructor always finds it running
		logging::info("MCP server listening on {}", url());
	}

	HttpServer::~HttpServer()
	{
		m_http->stop();
		m_thread.join();
	}

	std::string HttpServer::url() const
	{
		return "http://127.0.0.1:" + std::to_string(m_port) + "/mcp";
	}

	std::string makeToken()
	{
		std::random_device random;
		std::string token;
		for (int i = 0; i < 32; ++i)
		{
			const unsigned byte = random() & 0xffU;
			token += g_hexDigits[byte >> 4U];
			token += g_hexDigits[byte & 0xfU];
		}
		return token;
	}

	bool isLoopback(std::string_view address)
	{
		if (address == "::1")
			return true;
		if (startsWith(address, "::ffff:"))
			address.remove_prefix(7);
		return startsWith(address, "127.") && address.find_first_not_of("0123456789.") == std::string_view::npos;
	}
}
