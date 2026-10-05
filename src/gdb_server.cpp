#include "gdb_server.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstdlib>

GdbServer::~GdbServer()
{
	stopping = true;
	if (listen_fd != ~0ull) closesocket((SOCKET)listen_fd);
	const uint64_t c = client_fd.exchange(~0ull);
	if (c != ~0ull) closesocket((SOCKET)c);
	if (thread.joinable()) thread.join();
}

bool GdbServer::start(const std::string &address, int port, std::string &error)
{
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { error = "WSAStartup failed"; return false; }
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) { error = "cannot make a socket"; return false; }
	BOOL yes = TRUE;
	setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&yes, sizeof yes);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u_short)port);
	if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
		error = "not an IPv4 address: " + address;
		closesocket(s);
		return false;
	}
	if (bind(s, (const sockaddr *)&addr, sizeof addr) != 0 || listen(s, 1) != 0) {
		error = "cannot listen on " + address + ":" + std::to_string(port);
		closesocket(s);
		return false;
	}
	listen_fd = (uint64_t)s;
	thread = std::thread(&GdbServer::serve, this);
	return true;
}

void GdbServer::serve()
{
	while (!stopping) {
		SOCKET c = accept((SOCKET)listen_fd, nullptr, nullptr);
		if (c == INVALID_SOCKET) {
			if (stopping) return;
			continue;
		}
		BOOL yes = TRUE;
		setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof yes);
		{
			std::lock_guard<std::mutex> lock(queue_mutex);
			queue.clear();
			partial.clear();
		}
		no_ack = false;
		client_fd = (uint64_t)c;
		new_connection = true;
		std::printf("gdb: connected\n");
		std::fflush(stdout);
		char buf[4096];
		for (;;) {
			const int n = recv(c, buf, sizeof buf, 0);
			if (n <= 0) break;
			handle_bytes(buf, (size_t)n);
		}
		client_fd = ~0ull;
		closesocket(c);
		std::printf("gdb: disconnected\n");
		std::fflush(stdout);
	}
}

void GdbServer::handle_bytes(const char *data, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		const char ch = data[i];
		if (partial.empty()) {
			if (ch == 0x03) { interrupt = true; continue; }   // Ctrl-C
			if (ch != '$') continue;                           // acks, and noise between packets
		}
		partial.push_back(ch);
		// $payload#xx: complete once the two checksum digits are in.
		const size_t hash = partial.find('#');
		if (hash == std::string::npos || partial.size() < hash + 3) continue;
		std::string payload = partial.substr(1, hash - 1);
		const unsigned want = (unsigned)std::strtoul(partial.substr(hash + 1, 2).c_str(), nullptr, 16);
		partial.clear();
		unsigned sum = 0;
		for (unsigned char b : payload) sum += b;
		if (!no_ack) {
			const char ack = (sum & 0xFF) == want ? '+' : '-';
			std::lock_guard<std::mutex> lock(send_mutex);
			::send((SOCKET)client_fd.load(), &ack, 1, 0);
			if (ack == '-') continue;
		}
		// Escapes (}x is x ^ 0x20) only appear in binary payloads, which
		// this server does not ask for; undone anyway, so they read right.
		std::string plain;
		for (size_t k = 0; k < payload.size(); k++)
			plain.push_back(payload[k] == '}' && k + 1 < payload.size() ? (char)(payload[++k] ^ 0x20) : payload[k]);
		std::lock_guard<std::mutex> lock(queue_mutex);
		queue.push_back(std::move(plain));
	}
}

bool GdbServer::next_packet(std::string &payload)
{
	std::lock_guard<std::mutex> lock(queue_mutex);
	if (queue.empty()) return false;
	payload = std::move(queue.front());
	queue.pop_front();
	return true;
}

void GdbServer::send(const std::string &payload)
{
	static const char hex[] = "0123456789abcdef";
	std::string frame = "$";
	unsigned sum = 0;
	for (unsigned char b : payload) {
		// $, #, } and * would end or confuse the frame: escaped.
		if (b == '$' || b == '#' || b == '}' || b == '*') {
			frame.push_back('}');
			sum += '}';
			b ^= 0x20;
		}
		frame.push_back((char)b);
		sum += b;
	}
	frame.push_back('#');
	frame.push_back(hex[(sum >> 4) & 0xF]);
	frame.push_back(hex[sum & 0xF]);
	const uint64_t c = client_fd.load();
	if (c == ~0ull) return;
	std::lock_guard<std::mutex> lock(send_mutex);
	::send((SOCKET)c, frame.data(), (int)frame.size(), 0);
}
