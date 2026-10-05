#pragma once
// The transport half of -gdb=<port>: gdb's remote serial protocol over TCP.
//
// One connection at a time, on 127.0.0.1 only. A thread of its own accepts,
// reads packets ($payload#checksum), acknowledges them and queues their
// payloads; a Ctrl-C from gdb (the 0x03 byte outside a packet) only raises a
// flag. Nothing here touches the machine: the CPU thread takes the queued
// packets while the machine is halted and answers them (gdb_target.cpp), so
// gdb reads and changes the machine only between instructions, on the thread
// that owns it.
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

class GdbServer {
public:
	~GdbServer();
	// Listen on address:port (an IPv4 address; 127.0.0.1 unless the user
	// asks for another, such as the one WSL reaches Windows by) and start
	// accepting. False, with the reason, if it cannot be had.
	bool start(const std::string &address, int port, std::string &error);

	// A packet gdb sent, if one is waiting.
	bool next_packet(std::string &payload);
	// Send a reply ("OK", "S05", ...), framed and checksummed.
	void send(const std::string &payload);
	// gdb pressed Ctrl-C: true once, then false until the next one.
	bool take_interrupt() { return interrupt.exchange(false); }
	// A debugger is connected now.
	bool connected() const { return client_fd.load() != ~0ull; }
	// gdb has connected since this was last asked: its first look should
	// find the machine stopped.
	bool take_new_connection() { return new_connection.exchange(false); }
	// QStartNoAckMode: gdb and DoomV stop acknowledging packets.
	void set_no_ack() { no_ack = true; }

private:
	void serve();
	void handle_bytes(const char *data, size_t n);

	uint64_t listen_fd = ~0ull;
	std::atomic<uint64_t> client_fd{~0ull};
	std::thread thread;
	std::atomic<bool> stopping{false};
	std::atomic<bool> interrupt{false};
	std::atomic<bool> new_connection{false};
	std::atomic<bool> no_ack{false};
	std::mutex queue_mutex, send_mutex;
	std::deque<std::string> queue;
	std::string partial;   // bytes of a packet still arriving
};
