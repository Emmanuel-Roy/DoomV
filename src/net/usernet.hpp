#pragma once
// User-mode networking: the network behind DoomV's virtio-net card, made of
// ordinary host sockets, so it needs no driver and no administrator -- the
// same idea as QEMU's `-netdev user` (slirp).
//
// The guest sees a small private network, the same one QEMU's gives it:
//
//   10.0.2.15   the guest, handed out by DHCP
//   10.0.2.2    the gateway: the host itself (connections to it go to 127.0.0.1)
//   10.0.2.3    the DNS server: queries are passed to the host's own resolver
//
// What goes out:
//   ARP      the gateway and DNS addresses answer
//   DHCP     one lease, 10.0.2.15
//   ICMP     echo, to the gateway (answered here) or anywhere (a host ping)
//   UDP      each flow becomes a host UDP socket
//   TCP      each connection becomes a host TCP connection; this end speaks
//            TCP to the guest and the host's stack speaks it to the world
//
// Nothing comes in that the guest did not start: there is no port
// forwarding, so a server in the guest is not reachable from outside.
//
// Everything runs on a thread of its own, so a host socket never blocks the
// CPU. Frames from the guest are queued to it; frames for the guest are
// queued back and committed by DoomSystem at its input points, which is what
// keeps a run reproducible under -record and -replay.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class UserNet {
public:
	UserNet();
	~UserNet();

	// Starts the network thread. False, with the reason, if sockets are
	// unavailable.
	bool start(std::string &error);

	// A frame the guest sent. Any thread.
	void from_guest(const uint8_t *frame, size_t len);
	// Frames for the guest since the last call, in order. Any thread.
	void to_guest(std::vector<std::vector<uint8_t>> &out);
	bool has_frames() const;

	struct Impl;

private:
	std::unique_ptr<Impl> impl;
};
