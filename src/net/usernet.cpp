#include "usernet.hpp"

#ifdef _WIN32
// Before winsock2.h: the default of 64 sockets per select is a handful of
// connections.
#define FD_SETSIZE 1024
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>

#ifdef _WIN32

namespace {

using Clock = std::chrono::steady_clock;

// The guest's network, in host byte order.
constexpr uint32_t GATEWAY = 0x0A000202;   // 10.0.2.2
constexpr uint32_t DNS     = 0x0A000203;   // 10.0.2.3
constexpr uint32_t GUEST   = 0x0A00020F;   // 10.0.2.15
constexpr uint32_t NETMASK = 0xFFFFFF00;
constexpr uint8_t GATEWAY_MAC[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};

constexpr uint16_t ETH_IP = 0x0800, ETH_ARP = 0x0806;
constexpr uint8_t IP_ICMP = 1, IP_TCP = 6, IP_UDP = 17;
constexpr uint8_t TCP_FIN = 0x01, TCP_SYN = 0x02, TCP_RST = 0x04, TCP_PSH = 0x08, TCP_ACK = 0x10;
constexpr uint32_t WINDOW = 65535;     // no window scaling: never more in flight either way
constexpr uint16_t OUR_MSS = 1460;

uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

// The Internet checksum: a ones'-complement sum of 16-bit words.
uint32_t sum16(const uint8_t *p, size_t n, uint32_t s = 0)
{
	for (; n > 1; p += 2, n -= 2) s += (uint32_t)(p[0] << 8 | p[1]);
	if (n) s += (uint32_t)p[0] << 8;
	return s;
}
uint16_t fold(uint32_t s)
{
	while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
	return (uint16_t)~s;
}
// TCP and UDP checksum over the pseudo-header and the segment.
uint16_t l4_checksum(uint8_t proto, uint32_t src, uint32_t dst, const uint8_t *seg, size_t len)
{
	uint8_t pseudo[12];
	put32(pseudo, src);
	put32(pseudo + 4, dst);
	pseudo[8] = 0;
	pseudo[9] = proto;
	put16(pseudo + 10, (uint16_t)len);
	return fold(sum16(seg, len, sum16(pseudo, 12)));
}

// Sequence numbers wrap; compare them as a signed difference.
bool seq_after(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

sockaddr_in host_addr(uint32_t ip, uint16_t port)
{
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(ip);
	a.sin_port = htons(port);
	return a;
}

// The host's own DNS server, the first IPv4 one it has.
uint32_t host_dns_server()
{
	ULONG size = 0;
	GetNetworkParams(nullptr, &size);
	std::vector<uint8_t> buf(size ? size : sizeof(FIXED_INFO));
	FIXED_INFO *info = reinterpret_cast<FIXED_INFO *>(buf.data());
	if (GetNetworkParams(info, &size) == ERROR_SUCCESS) {
		for (IP_ADDR_STRING *a = &info->DnsServerList; a; a = a->Next) {
			in_addr ip{};
			if (inet_pton(AF_INET, a->IpAddress.String, &ip) == 1 && ip.s_addr) return ntohl(ip.s_addr);
		}
	}
	return 0x08080808;   // 8.8.8.8, when the host names none
}

} // namespace

struct UserNet::Impl {
	struct Tcp {
		SOCKET s = INVALID_SOCKET;
		uint32_t remote_ip = 0;     // as the guest addressed it
		uint16_t remote_port = 0, guest_port = 0;
		enum State { Connecting, SynReceived, Established } state = Connecting;
		uint32_t iss = 0;
		uint32_t snd_una = 0, snd_nxt = 0;   // our side of the stream, toward the guest
		uint32_t rcv_nxt = 0;                // the next byte expected from the guest
		uint32_t guest_wnd = 0;
		uint16_t guest_mss = 536;
		uint32_t last_wnd = 0;               // the window we last advertised
		std::vector<uint8_t> to_host;        // the guest's bytes, not yet written to the host
		bool guest_fin = false, host_shut = false, fin_sent = false, dead = false;
	};
	struct Udp {
		SOCKET s = INVALID_SOCKET;
		uint32_t remote_ip = 0;
		uint16_t remote_port = 0, guest_port = 0;
		Clock::time_point last;
	};
	struct Ping {
		uint32_t dst;
		std::vector<uint8_t> echo;   // id, sequence and data of the request
	};

	std::thread thread, ping_thread;
	std::atomic<bool> stopping{false};

	std::mutex in_mutex;
	std::deque<std::vector<uint8_t>> in;    // frames from the guest
	std::mutex out_mutex;
	std::deque<std::vector<uint8_t>> out;   // frames for the guest
	std::atomic<bool> out_ready{false};

	// A loopback socket the guest side writes a byte to, so the network
	// thread wakes for a frame at once rather than at its next timeout.
	SOCKET wake_recv = INVALID_SOCKET, wake_send = INVALID_SOCKET;
	sockaddr_in wake_addr{};

	std::mutex ping_mutex;
	std::condition_variable ping_cv;
	std::deque<Ping> pings;

	uint8_t guest_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
	uint32_t dns_server = 0;
	std::atomic<uint16_t> ip_id{0};
	uint32_t next_iss = 0x10000000;
	std::map<std::tuple<uint16_t, uint32_t, uint16_t>, Tcp> tcp;   // guest port, remote ip, remote port
	std::map<std::tuple<uint16_t, uint32_t, uint16_t>, Udp> udp;

	// Where the guest's address points on the host: the gateway is the
	// host itself, the DNS address is the host's resolver.
	uint32_t real_ip(uint32_t ip) const
	{
		if (ip == GATEWAY) return INADDR_LOOPBACK;
		if (ip == DNS) return dns_server;
		return ip;
	}

	// ---- frames to the guest ---------------------------------------------------

	void emit(std::vector<uint8_t> frame)
	{
		std::lock_guard<std::mutex> lock(out_mutex);
		out.push_back(std::move(frame));
		out_ready = true;
	}

	void emit_eth(uint16_t type, const uint8_t *dst_mac, const uint8_t *payload, size_t len)
	{
		std::vector<uint8_t> f(14 + len);
		std::memcpy(f.data(), dst_mac, 6);
		std::memcpy(f.data() + 6, GATEWAY_MAC, 6);
		put16(f.data() + 12, type);
		std::memcpy(f.data() + 14, payload, len);
		emit(std::move(f));
	}

	void emit_ip(uint8_t proto, uint32_t src, uint32_t dst, const std::vector<uint8_t> &l4, bool broadcast = false)
	{
		std::vector<uint8_t> p(20 + l4.size());
		p[0] = 0x45;
		put16(p.data() + 2, (uint16_t)p.size());
		put16(p.data() + 4, ip_id++);
		put16(p.data() + 6, 0x4000);   // don't fragment
		p[8] = 64;
		p[9] = proto;
		put32(p.data() + 12, src);
		put32(p.data() + 16, dst);
		put16(p.data() + 10, fold(sum16(p.data(), 20)));
		std::memcpy(p.data() + 20, l4.data(), l4.size());
		static const uint8_t BROADCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
		emit_eth(ETH_IP, broadcast ? BROADCAST : guest_mac, p.data(), p.size());
	}

	void emit_udp(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data, size_t len,
	              bool broadcast = false)
	{
		std::vector<uint8_t> u(8 + len);
		put16(u.data(), sport);
		put16(u.data() + 2, dport);
		put16(u.data() + 4, (uint16_t)u.size());
		std::memcpy(u.data() + 8, data, len);
		uint16_t cs = l4_checksum(IP_UDP, src, dst, u.data(), u.size());
		put16(u.data() + 6, cs ? cs : 0xFFFF);
		emit_ip(IP_UDP, src, dst, u, broadcast);
	}

	void emit_tcp(Tcp &c, uint32_t seq, uint8_t flags, const uint8_t *data = nullptr, size_t len = 0)
	{
		const bool syn = flags & TCP_SYN;
		const size_t hdr = syn ? 24 : 20;
		std::vector<uint8_t> t(hdr + len);
		put16(t.data(), c.remote_port);
		put16(t.data() + 2, c.guest_port);
		put32(t.data() + 4, seq);
		put32(t.data() + 8, (flags & TCP_ACK) ? c.rcv_nxt : 0);
		t[12] = (uint8_t)((hdr / 4) << 4);
		t[13] = flags;
		c.last_wnd = window(c);
		put16(t.data() + 14, (uint16_t)c.last_wnd);
		if (syn) {   // MSS, and nothing else: no window scaling either way
			t[20] = 2; t[21] = 4;
			put16(t.data() + 22, OUR_MSS);
		}
		if (len) std::memcpy(t.data() + hdr, data, len);
		put16(t.data() + 16, l4_checksum(IP_TCP, c.remote_ip, GUEST, t.data(), t.size()));
		emit_ip(IP_TCP, c.remote_ip, GUEST, t);
	}

	static uint32_t window(const Tcp &c)
	{
		return c.to_host.size() >= WINDOW ? 0 : WINDOW - (uint32_t)c.to_host.size();
	}

	// A reset for a segment that belongs to no connection.
	void emit_reset(uint32_t src, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, bool with_ack)
	{
		Tcp c;
		c.remote_ip = src;
		c.remote_port = sport;
		c.guest_port = dport;
		c.rcv_nxt = ack;
		emit_tcp(c, seq, (uint8_t)(TCP_RST | (with_ack ? TCP_ACK : 0)));
	}

	// ---- frames from the guest --------------------------------------------------

	void handle(const std::vector<uint8_t> &f)
	{
		if (f.size() < 14) return;
		std::memcpy(guest_mac, f.data() + 6, 6);
		const uint16_t type = get16(f.data() + 12);
		if (type == ETH_ARP) handle_arp(f.data() + 14, f.size() - 14);
		else if (type == ETH_IP) handle_ip(f.data() + 14, f.size() - 14);
	}

	void handle_arp(const uint8_t *p, size_t n)
	{
		if (n < 28 || get16(p) != 1 || get16(p + 2) != ETH_IP || get16(p + 6) != 1) return;
		const uint32_t target = get32(p + 24);
		// The addresses this network has. The guest's own address never
		// answers, or its duplicate-address check would fail.
		if (target != GATEWAY && target != DNS) return;
		uint8_t r[28];
		put16(r, 1);
		put16(r + 2, ETH_IP);
		r[4] = 6; r[5] = 4;
		put16(r + 6, 2);                       // reply
		std::memcpy(r + 8, GATEWAY_MAC, 6);
		put32(r + 14, target);
		std::memcpy(r + 18, p + 8, 6);         // to the asker
		std::memcpy(r + 24, p + 14, 4);
		emit_eth(ETH_ARP, p + 8, r, sizeof r);
	}

	void handle_ip(const uint8_t *p, size_t n)
	{
		if (n < 20 || (p[0] >> 4) != 4) return;
		const size_t ihl = (size_t)(p[0] & 15) * 4;
		const size_t total = get16(p + 2);
		if (ihl < 20 || total < ihl || total > n) return;
		if (get16(p + 6) & 0x3FFF) return;     // fragments: nothing here sends them
		const uint32_t src = get32(p + 12), dst = get32(p + 16);
		const uint8_t *l4 = p + ihl;
		const size_t len = total - ihl;
		switch (p[9]) {
		case IP_ICMP: handle_icmp(dst, l4, len); break;
		case IP_UDP:  handle_udp(src, dst, l4, len); break;
		case IP_TCP:  handle_tcp(dst, l4, len); break;
		default: break;
		}
	}

	void handle_icmp(uint32_t dst, const uint8_t *p, size_t n)
	{
		if (n < 8 || p[0] != 8) return;        // echo requests only
		std::vector<uint8_t> echo(p + 4, p + n);
		if (dst == GATEWAY || dst == DNS) {
			reply_echo(dst, echo);
			return;
		}
		if ((dst & NETMASK) == (GATEWAY & NETMASK)) return;
		std::lock_guard<std::mutex> lock(ping_mutex);
		if (pings.size() < 64) pings.push_back({dst, std::move(echo)});
		ping_cv.notify_one();
	}

	void reply_echo(uint32_t from, const std::vector<uint8_t> &echo)
	{
		std::vector<uint8_t> r(4 + echo.size());
		std::memcpy(r.data() + 4, echo.data(), echo.size());
		put16(r.data() + 2, fold(sum16(r.data(), r.size())));
		emit_ip(IP_ICMP, from, GUEST, r);
	}

	// A ping to the outside world is a host ping, one at a time on a thread
	// of its own: IcmpSendEcho waits for its answer.
	void ping_loop()
	{
		HANDLE icmp = IcmpCreateFile();
		std::vector<uint8_t> reply(65536);
		while (!stopping) {
			Ping ping;
			{
				std::unique_lock<std::mutex> lock(ping_mutex);
				ping_cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return !pings.empty() || stopping; });
				if (pings.empty()) continue;
				ping = std::move(pings.front());
				pings.pop_front();
			}
			if (icmp == INVALID_HANDLE_VALUE || ping.echo.size() < 4) continue;
			const DWORD got = IcmpSendEcho(icmp, htonl(ping.dst), ping.echo.data() + 4, (WORD)(ping.echo.size() - 4),
			                               nullptr, reply.data(), (DWORD)reply.size(), 2000);
			if (got && reinterpret_cast<ICMP_ECHO_REPLY *>(reply.data())->Status == IP_SUCCESS)
				reply_echo(ping.dst, ping.echo);
		}
		if (icmp != INVALID_HANDLE_VALUE) IcmpCloseHandle(icmp);
	}

	void handle_udp(uint32_t src, uint32_t dst, const uint8_t *p, size_t n)
	{
		if (n < 8) return;
		const uint16_t sport = get16(p), dport = get16(p + 2);
		const size_t len = std::min<size_t>(get16(p + 4), n);
		if (len < 8) return;
		const uint8_t *data = p + 8;
		const size_t dlen = len - 8;
		if (dport == 67) {
			handle_dhcp(data, dlen);
			return;
		}
		(void)src;
		if (dst == 0xFFFFFFFF || (dst >> 28) == 0xE) return;   // broadcast, multicast
		if ((dst & NETMASK) == (GATEWAY & NETMASK) && dst != GATEWAY && dst != DNS) return;
		const auto key = std::make_tuple(sport, dst, dport);
		auto it = udp.find(key);
		if (it == udp.end()) {
			Udp u;
			u.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (u.s == INVALID_SOCKET) return;
			u_long nonblocking = 1;
			ioctlsocket(u.s, FIONBIO, &nonblocking);
			const sockaddr_in to = host_addr(real_ip(dst), dport);
			if (connect(u.s, (const sockaddr *)&to, sizeof to) != 0) {
				closesocket(u.s);
				return;
			}
			u.remote_ip = dst;
			u.remote_port = dport;
			u.guest_port = sport;
			it = udp.emplace(key, u).first;
		}
		it->second.last = Clock::now();
		send(it->second.s, (const char *)data, (int)dlen, 0);
	}

	// One lease, always the same: 10.0.2.15, with the gateway and DNS server.
	void handle_dhcp(const uint8_t *p, size_t n)
	{
		if (n < 240 || p[0] != 1 || get32(p + 236) != 0x63825363) return;
		uint8_t kind = 0;
		for (size_t i = 240; i + 1 < n && p[i] != 255;) {
			if (p[i] == 0) { i++; continue; }
			if (p[i] == 53 && p[i + 1] >= 1 && i + 2 < n) kind = p[i + 2];
			i += 2 + (size_t)p[i + 1];
		}
		uint8_t reply_kind;
		if (kind == 1) reply_kind = 2;         // discover -> offer
		else if (kind == 3) reply_kind = 5;    // request -> ack
		else return;
		std::vector<uint8_t> r(300, 0);
		r[0] = 2;                              // reply
		r[1] = 1; r[2] = 6;                    // Ethernet
		std::memcpy(r.data() + 4, p + 4, 4);   // xid
		std::memcpy(r.data() + 10, p + 10, 2); // flags
		put32(r.data() + 16, GUEST);           // yiaddr
		put32(r.data() + 20, GATEWAY);         // siaddr
		std::memcpy(r.data() + 28, p + 28, 16);   // chaddr
		put32(r.data() + 236, 0x63825363);
		size_t o = 240;
		const auto opt = [&](uint8_t code, std::initializer_list<uint8_t> bytes) {
			r[o++] = code;
			r[o++] = (uint8_t)bytes.size();
			for (uint8_t b : bytes) r[o++] = b;
		};
		opt(53, {reply_kind});
		opt(54, {10, 0, 2, 2});               // server
		opt(51, {0, 1, 0x51, 0x80});          // lease: a day
		opt(1, {255, 255, 255, 0});           // netmask
		opt(3, {10, 0, 2, 2});                // router
		opt(6, {10, 0, 2, 3});                // DNS
		r[o++] = 255;
		emit_udp(GATEWAY, 67, 0xFFFFFFFF, 68, r.data(), r.size(), true);
	}

	void handle_tcp(uint32_t dst, const uint8_t *p, size_t n)
	{
		if (n < 20) return;
		const uint16_t sport = get16(p), dport = get16(p + 2);
		const uint32_t seq = get32(p + 4), ack = get32(p + 8);
		const size_t off = (size_t)(p[12] >> 4) * 4;
		const uint8_t flags = p[13];
		const uint16_t wnd = get16(p + 14);
		if (off < 20 || off > n) return;
		const uint8_t *data = p + off;
		const size_t len = n - off;
		const auto key = std::make_tuple(sport, dst, dport);
		auto it = tcp.find(key);

		if (flags & TCP_RST) {
			if (it != tcp.end()) it->second.dead = true;
			return;
		}
		if (it == tcp.end()) {
			if ((flags & TCP_SYN) && !(flags & TCP_ACK)) open_tcp(key, dst, sport, dport, seq, wnd, p, off);
			else emit_reset(dst, dport, sport, (flags & TCP_ACK) ? ack : 0,
			                seq + (uint32_t)len + ((flags & (TCP_SYN | TCP_FIN)) ? 1 : 0), !(flags & TCP_ACK));
			return;
		}
		Tcp &c = it->second;
		if (c.state == Tcp::Connecting || (flags & TCP_SYN)) return;   // a repeated SYN
		if (flags & TCP_ACK) {
			if (c.state == Tcp::SynReceived && ack == c.snd_nxt) c.state = Tcp::Established;
			if (seq_after(ack, c.snd_una) && !seq_after(ack, c.snd_nxt)) c.snd_una = ack;
			c.guest_wnd = wnd;
		}
		if (c.state != Tcp::Established) return;

		bool need_ack = false;
		if (len) {
			if (seq == c.rcv_nxt && !c.guest_fin) {
				const size_t take = std::min<size_t>(len, window(c));
				c.to_host.insert(c.to_host.end(), data, data + take);
				c.rcv_nxt += (uint32_t)take;
			}
			need_ack = true;   // a repeat or an overrun is answered with where we are
		}
		if (flags & TCP_FIN) {
			if (seq + (uint32_t)len == c.rcv_nxt && !c.guest_fin) {
				c.rcv_nxt++;
				c.guest_fin = true;
			}
			need_ack = true;
		}
		write_host(c);
		if (need_ack) emit_tcp(c, c.snd_nxt, TCP_ACK);
	}

	void open_tcp(const std::tuple<uint16_t, uint32_t, uint16_t> &key, uint32_t dst, uint16_t sport, uint16_t dport,
	              uint32_t seq, uint16_t wnd, const uint8_t *hdr, size_t off)
	{
		if ((dst & NETMASK) == (GATEWAY & NETMASK) && dst != GATEWAY && dst != DNS) {
			emit_reset(dst, dport, sport, 0, seq + 1, true);
			return;
		}
		Tcp c;
		c.remote_ip = dst;
		c.remote_port = dport;
		c.guest_port = sport;
		c.rcv_nxt = seq + 1;
		c.guest_wnd = wnd;
		c.iss = next_iss += 0x10000;
		c.snd_una = c.snd_nxt = c.iss;
		for (size_t i = 20; i + 1 < off;) {   // the guest's MSS, if it says
			if (hdr[i] == 0) break;
			if (hdr[i] == 1) { i++; continue; }
			if (hdr[i] == 2 && hdr[i + 1] == 4 && i + 3 < off) c.guest_mss = get16(hdr + i + 2);
			i += hdr[i + 1] ? hdr[i + 1] : 1;
		}
		c.s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (c.s == INVALID_SOCKET) {
			emit_reset(dst, dport, sport, 0, c.rcv_nxt, true);
			return;
		}
		u_long nonblocking = 1;
		ioctlsocket(c.s, FIONBIO, &nonblocking);
		BOOL nodelay = TRUE;
		setsockopt(c.s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);
		const sockaddr_in to = host_addr(real_ip(dst), dport);
		if (connect(c.s, (const sockaddr *)&to, sizeof to) != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
			closesocket(c.s);
			emit_reset(dst, dport, sport, 0, c.rcv_nxt, true);
			return;
		}
		tcp.emplace(key, std::move(c));
	}

	// The guest's bytes, to the host, as far as it will take them now.
	void write_host(Tcp &c)
	{
		const uint32_t before = window(c);
		while (!c.to_host.empty()) {
			const int sent = send(c.s, (const char *)c.to_host.data(), (int)c.to_host.size(), 0);
			if (sent <= 0) {
				if (WSAGetLastError() != WSAEWOULDBLOCK) c.dead = true;
				break;
			}
			c.to_host.erase(c.to_host.begin(), c.to_host.begin() + sent);
		}
		if (c.guest_fin && c.to_host.empty() && !c.host_shut) {
			shutdown(c.s, SD_SEND);
			c.host_shut = true;
		}
		// Tell the guest the window opened, when it opened by much.
		if (c.state == Tcp::Established && window(c) >= c.last_wnd + 8192 && window(c) > before)
			emit_tcp(c, c.snd_nxt, TCP_ACK);
	}

	// The host's bytes, to the guest, as far as its window allows.
	void read_host(Tcp &c)
	{
		const uint32_t in_flight = c.snd_nxt - c.snd_una;
		const uint32_t limit = std::min<uint32_t>(c.guest_wnd, WINDOW);
		if (in_flight >= limit) return;
		std::vector<uint8_t> buf(limit - in_flight);
		const int got = recv(c.s, (char *)buf.data(), (int)buf.size(), 0);
		if (got > 0) {
			const size_t mss = std::min<size_t>(OUR_MSS, c.guest_mss ? c.guest_mss : 536);
			for (size_t at = 0; at < (size_t)got;) {
				const size_t take = std::min(mss, (size_t)got - at);
				emit_tcp(c, c.snd_nxt, TCP_ACK | TCP_PSH, buf.data() + at, take);
				c.snd_nxt += (uint32_t)take;
				at += take;
			}
		} else if (got == 0) {
			emit_tcp(c, c.snd_nxt, TCP_FIN | TCP_ACK);
			c.snd_nxt++;
			c.fin_sent = true;
		} else if (WSAGetLastError() != WSAEWOULDBLOCK) {
			emit_tcp(c, c.snd_nxt, TCP_RST | TCP_ACK);
			c.dead = true;
		}
	}

	bool wants_read(const Tcp &c) const
	{
		return c.state == Tcp::Established && !c.fin_sent
		       && c.snd_nxt - c.snd_una < std::min<uint32_t>(c.guest_wnd, WINDOW);
	}

	// ---- the thread -------------------------------------------------------------

	void loop()
	{
		std::vector<std::vector<uint8_t>> frames;
		std::vector<char> buf(65536);
		Clock::time_point last_sweep = Clock::now();
		while (!stopping) {
			{
				std::lock_guard<std::mutex> lock(in_mutex);
				frames.assign(std::make_move_iterator(in.begin()), std::make_move_iterator(in.end()));
				in.clear();
			}
			for (const auto &f : frames) handle(f);
			frames.clear();

			fd_set rd, wr, ex;
			FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex);
			FD_SET(wake_recv, &rd);
			for (auto &kv : udp) FD_SET(kv.second.s, &rd);
			for (auto &kv : tcp) {
				Tcp &c = kv.second;
				if (c.dead) continue;
				if (c.state == Tcp::Connecting) { FD_SET(c.s, &wr); FD_SET(c.s, &ex); continue; }
				if (wants_read(c)) FD_SET(c.s, &rd);
				if (!c.to_host.empty()) FD_SET(c.s, &wr);
			}
			timeval tv{0, 50000};
			if (select(0, &rd, &wr, &ex, &tv) > 0) {
				if (FD_ISSET(wake_recv, &rd)) recv(wake_recv, buf.data(), (int)buf.size(), 0);
				for (auto &kv : udp) {
					Udp &u = kv.second;
					if (!FD_ISSET(u.s, &rd)) continue;
					const int got = recv(u.s, buf.data(), (int)buf.size(), 0);
					if (got > 0) {
						u.last = Clock::now();
						emit_udp(u.remote_ip, u.remote_port, GUEST, u.guest_port, (const uint8_t *)buf.data(), (size_t)got);
					}
				}
				for (auto &kv : tcp) {
					Tcp &c = kv.second;
					if (c.dead) continue;
					if (c.state == Tcp::Connecting) {
						if (FD_ISSET(c.s, &ex)) {
							emit_reset(c.remote_ip, c.remote_port, c.guest_port, 0, c.rcv_nxt, true);
							c.dead = true;
						} else if (FD_ISSET(c.s, &wr)) {
							emit_tcp(c, c.iss, TCP_SYN | TCP_ACK);
							c.snd_nxt = c.iss + 1;
							c.state = Tcp::SynReceived;
						}
						continue;
					}
					if (FD_ISSET(c.s, &wr)) write_host(c);
					if (FD_ISSET(c.s, &rd)) read_host(c);
				}
			}
			// Connections that are over: reset, or both sides closed and
			// everything acknowledged.
			for (auto it = tcp.begin(); it != tcp.end();) {
				Tcp &c = it->second;
				const bool done = c.dead || (c.guest_fin && c.fin_sent && c.snd_una == c.snd_nxt);
				if (done) {
					closesocket(c.s);
					it = tcp.erase(it);
				} else {
					++it;
				}
			}
			// UDP flows idle for two minutes.
			const Clock::time_point now = Clock::now();
			if (now - last_sweep > std::chrono::seconds(10)) {
				last_sweep = now;
				for (auto it = udp.begin(); it != udp.end();) {
					if (now - it->second.last > std::chrono::minutes(2)) {
						closesocket(it->second.s);
						it = udp.erase(it);
					} else {
						++it;
					}
				}
			}
		}
		for (auto &kv : tcp) closesocket(kv.second.s);
		for (auto &kv : udp) closesocket(kv.second.s);
	}
};

UserNet::UserNet() : impl(new Impl) {}

UserNet::~UserNet()
{
	impl->stopping = true;
	impl->ping_cv.notify_all();
	if (impl->thread.joinable()) impl->thread.join();
	if (impl->ping_thread.joinable()) impl->ping_thread.join();
	if (impl->wake_recv != INVALID_SOCKET) closesocket(impl->wake_recv);
	if (impl->wake_send != INVALID_SOCKET) closesocket(impl->wake_send);
}

bool UserNet::start(std::string &error)
{
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		error = "Windows sockets are not available";
		return false;
	}
	Impl &n = *impl;
	n.dns_server = host_dns_server();
	n.wake_recv = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	n.wake_send = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	n.wake_addr = host_addr(INADDR_LOOPBACK, 0);
	int size = sizeof n.wake_addr;
	if (n.wake_recv == INVALID_SOCKET || n.wake_send == INVALID_SOCKET
	    || bind(n.wake_recv, (const sockaddr *)&n.wake_addr, sizeof n.wake_addr) != 0
	    || getsockname(n.wake_recv, (sockaddr *)&n.wake_addr, &size) != 0) {
		error = "cannot open a loopback socket";
		return false;
	}
	u_long nonblocking = 1;
	ioctlsocket(n.wake_recv, FIONBIO, &nonblocking);
	n.thread = std::thread([&n] { n.loop(); });
	n.ping_thread = std::thread([&n] { n.ping_loop(); });
	return true;
}

void UserNet::from_guest(const uint8_t *frame, size_t len)
{
	{
		std::lock_guard<std::mutex> lock(impl->in_mutex);
		if (impl->in.size() < 4096) impl->in.emplace_back(frame, frame + len);
	}
	const char byte = 0;
	sendto(impl->wake_send, &byte, 1, 0, (const sockaddr *)&impl->wake_addr, sizeof impl->wake_addr);
}

void UserNet::to_guest(std::vector<std::vector<uint8_t>> &out)
{
	if (!impl->out_ready) return;
	std::lock_guard<std::mutex> lock(impl->out_mutex);
	for (auto &f : impl->out) out.push_back(std::move(f));
	impl->out.clear();
	impl->out_ready = false;
}

bool UserNet::has_frames() const { return impl->out_ready; }

#else   // not Windows: nothing yet

struct UserNet::Impl {};
UserNet::UserNet() : impl(new Impl) {}
UserNet::~UserNet() = default;
bool UserNet::start(std::string &error) { error = "user-mode networking is only built for Windows hosts"; return false; }
void UserNet::from_guest(const uint8_t *, size_t) {}
void UserNet::to_guest(std::vector<std::vector<uint8_t>> &) {}
bool UserNet::has_frames() const { return false; }

#endif
