// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace httplib
{
	class Server;
}

namespace gnubase::mcp
{
	class Server;

	// The stdio transport: one JSON-RPC message per line in, one response per line out. Returns when the input ends.
	void serveStdio(Server& server, std::istream& in, std::ostream& out);

	// The streamable HTTP transport on 127.0.0.1: POST /mcp with one JSON-RPC message, answered with JSON, or with
	// 202 for notifications. Every request needs "Authorization: Bearer <token>". Requests from other machines, from
	// web pages of other origins (DNS rebinding) and without the token are refused. There is no stream of
	// server-initiated messages yet, so GET answers 405.
	class HttpServer
	{
	public:
		// Listens on the port, or on a free one for 0, with threads of its own. Throws std::runtime_error if it can't.
		HttpServer(Server& server, const std::string& token, int port = 0);
		~HttpServer();
		HttpServer(const HttpServer&) = delete;
		HttpServer& operator=(const HttpServer&) = delete;

		int port() const { return m_port; }
		std::string url() const;

	private:
		std::unique_ptr<httplib::Server> m_http;
		int m_port = 0;
		std::thread m_thread;
	};

	// A random access token for the HTTP transport: 32 bytes as hex digits.
	std::string makeToken();

	// Whether a peer address is this machine: 127.0.0.0/8, ::1, or 127.x mapped to IPv6.
	bool isLoopback(std::string_view address);
}
