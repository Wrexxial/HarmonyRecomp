#!/usr/bin/env python3
"""HarmonyRecomp online server: rooms, room codes, public room list, connection
help (UDP hole punching) and relay for online co-op without a VPN.

The server never sees game data it can understand: players connect to each
other directly whenever their routers allow it; only when that fails, their
game packets are forwarded through here (a few KB/s per player).

    python3 hrnp_server.py [--port 47660]

One UDP port, no dependencies (Python 3.8+). Protocol: see PROTOCOL below
and src/netplay/online.cpp.
"""

from __future__ import annotations

import argparse
import logging
import os
import random
import signal
import socket
import struct
import time

PROTOCOL = """
All integers big-endian. Every datagram: u32 magic 'HRSV', u8 version (1), u8 type.
Strings: u8 length + UTF-8 bytes. Endpoint: u32 ip + u16 port.

client -> server                         server -> client
 1 HELLO  u64 token, str build,           101 WELCOME  u32 virtual_ip, endpoint public
          str name, u8 n, n*endpoint      102 PONG
 2 PING                                   103 ROOM_OPENED  str code
 3 ROOM_OPEN  u8 public, str name,        104 ROOM_LIST  u8 n, n*(str code, str name,
          u8 players, u8 max                               u8 players, u8 max)
 4 ROOM_CLOSE                             105 CONNECTED  u8 ok, str message, u32 host_vip
 5 ROOM_LIST                              106 PEER  u32 vip, str name, endpoint public,
 6 CONNECT  str code                                u8 n, n*endpoint (local)
 7 RELAY  u32 dst_vip, payload            107 RELAYED  u32 src_vip, payload
 8 BYE                                    108 ERROR  str message (e.g. say HELLO again)
"""

MAGIC = 0x48525356  # "HRSV"
VERSION = 1

HELLO, PING, ROOM_OPEN, ROOM_CLOSE, ROOM_LIST, CONNECT, RELAY, BYE = range(1, 9)
WELCOME, PONG, ROOM_OPENED, ROOM_LIST_RESULT, CONNECTED, PEER, RELAYED, ERROR = range(101, 109)

CLIENT_TIMEOUT = 45.0  # seconds without any packet
CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"  # no 0/O, 1/I
CODE_LENGTH = 6
MAX_LIST = 20
MAX_RELAY = 1400
MAX_ROOM_MEMBERS = 16
STATS_INTERVAL = 300.0  # seconds between "stats:" log lines (also on SIGUSR1)

log = logging.getLogger("hrnp")


def ip_str(ip: int) -> str:
    return socket.inet_ntoa(struct.pack(">I", ip))


class Reader:
    def __init__(self, data: bytes, pos: int = 0):
        self.data, self.pos = data, pos

    def take(self, n: int) -> bytes:
        if self.pos + n > len(self.data):
            raise ValueError("short packet")
        chunk = self.data[self.pos:self.pos + n]
        self.pos += n
        return chunk

    def u8(self) -> int:
        return self.take(1)[0]

    def u16(self) -> int:
        return struct.unpack(">H", self.take(2))[0]

    def u32(self) -> int:
        return struct.unpack(">I", self.take(4))[0]

    def u64(self) -> int:
        return struct.unpack(">Q", self.take(8))[0]

    def string(self) -> str:
        return self.take(self.u8()).decode("utf-8", "replace")

    def rest(self) -> bytes:
        chunk = self.data[self.pos:]
        self.pos = len(self.data)
        return chunk


def pack_str(text: str) -> bytes:
    raw = text.encode("utf-8")[:64]
    return bytes([len(raw)]) + raw


def pack_ep(ip: int, port: int) -> bytes:
    return struct.pack(">IH", ip, port)


def header(msg_type: int) -> bytes:
    return struct.pack(">IBB", MAGIC, VERSION, msg_type)


class Client:
    def __init__(self, token: int, addr, vip: int, build: str, name: str, local):
        self.token = token
        self.addr = addr  # (ip str, port) as seen by the server
        self.vip = vip
        self.build = build
        self.name = name
        self.local = local  # [(ip int, port)]
        self.last_seen = time.monotonic()
        self.rooms: set[str] = set()
        self.relayed_bytes = 0  # game data this player sent through the relay

    @property
    def public(self):
        return struct.unpack(">I", socket.inet_aton(self.addr[0]))[0], self.addr[1]


class Room:
    def __init__(self, code: str, host: Client, public: bool, name: str, players: int, maximum: int):
        self.code = code
        self.host = host
        self.public = public
        self.name = name
        self.players = players
        self.max = maximum
        self.build = host.build
        self.members: dict[int, Client] = {host.vip: host}
        self.opened_at = time.monotonic()
        self.joined = 0  # players that connected with the code / from the list
        self.relayed = False  # some game data went through the relay


class Server:
    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.by_addr: dict[tuple, Client] = {}
        self.by_token: dict[int, Client] = {}
        self.by_vip: dict[int, Client] = {}
        self.rooms: dict[str, Room] = {}
        self.hosted: dict[int, str] = {}  # host vip -> code
        self.next_vip = 1
        self.stats_wanted = False  # set by SIGUSR1
        self.peak_clients = 0
        self.relay_packets = 0  # since the last stats line
        self.relay_bytes = 0
        self.relay_players: set[int] = set()

    # ---- helpers -----------------------------------------------------------

    def send(self, addr, data: bytes) -> None:
        try:
            self.sock.sendto(data, addr)
        except OSError as err:
            log.debug("send to %s failed: %s", addr, err)

    def alloc_vip(self) -> int:
        # 10.66.0.1 .. 10.66.255.254, skipping .0 and .255 host parts.
        for _ in range(65536):
            n = self.next_vip
            self.next_vip = self.next_vip % 65534 + 1
            if n & 0xFF in (0, 255):
                continue
            vip = (10 << 24) | (66 << 16) | n
            if vip not in self.by_vip:
                return vip
        raise RuntimeError("no free virtual address")

    def new_code(self) -> str:
        while True:
            code = "".join(random.choice(CODE_ALPHABET) for _ in range(CODE_LENGTH))
            if code not in self.rooms:
                return code

    def peer_message(self, about: Client) -> bytes:
        pub_ip, pub_port = about.public
        local = about.local[:8]
        body = struct.pack(">I", about.vip) + pack_str(about.name) + pack_ep(pub_ip, pub_port)
        body += bytes([len(local)]) + b"".join(pack_ep(ip, port) for ip, port in local)
        return header(PEER) + body

    def introduce(self, a: Client, b: Client) -> None:
        """Tell two clients about each other so they start punching."""
        self.send(a.addr, self.peer_message(b))
        self.send(b.addr, self.peer_message(a))

    def log_room_closed(self, room: Room, why: str) -> None:
        minutes = (time.monotonic() - room.opened_at) / 60.0
        log.info("room %s closed%s after %.0f min, %d joined, %s", room.code, why, minutes,
                 room.joined, "relay used" if room.relayed else "no relay")

    def drop_client(self, client: Client, why: str) -> None:
        relayed = f", relayed {client.relayed_bytes // 1024} KB" if client.relayed_bytes else ""
        log.info("client %s %s (%s) gone: %s%s", ip_str(client.vip), client.name, client.addr,
                 why, relayed)
        code = self.hosted.pop(client.vip, None)
        room = self.rooms.pop(code, None) if code else None
        if room:
            self.log_room_closed(room, " (host gone)")
        for code in list(client.rooms):
            room = self.rooms.get(code)
            if room:
                room.members.pop(client.vip, None)
        self.by_addr.pop(client.addr, None)
        self.by_token.pop(client.token, None)
        self.by_vip.pop(client.vip, None)

    def close_room(self, client: Client) -> None:
        code = self.hosted.pop(client.vip, None)
        room = self.rooms.pop(code, None) if code else None
        if room:
            self.log_room_closed(room, "")

    # ---- packets -----------------------------------------------------------

    def handle(self, data: bytes, addr) -> None:
        r = Reader(data)
        if len(data) < 6 or r.u32() != MAGIC:
            return
        version = r.u8()
        msg_type = r.u8()
        if version != VERSION:
            self.send(addr, header(ERROR) + pack_str(
                f"server speaks protocol {VERSION}, you {version}: update HarmonyRecomp"))
            return

        if msg_type == HELLO:
            self.on_hello(r, addr)
            return
        client = self.by_addr.get(addr)
        if client is None:
            self.send(addr, header(ERROR) + pack_str("unknown client, say HELLO"))
            return
        client.last_seen = time.monotonic()

        if msg_type == PING:
            self.send(addr, header(PONG))
        elif msg_type == ROOM_OPEN:
            self.on_room_open(client, r)
        elif msg_type == ROOM_CLOSE:
            self.close_room(client)
        elif msg_type == ROOM_LIST:
            self.on_room_list(client)
        elif msg_type == CONNECT:
            self.on_connect(client, r)
        elif msg_type == RELAY:
            self.on_relay(client, r)
        elif msg_type == BYE:
            self.drop_client(client, "said bye")

    def on_hello(self, r: Reader, addr) -> None:
        token = r.u64()
        build = r.string()
        name = r.string()
        local = [(r.u32(), r.u16()) for _ in range(min(r.u8(), 8))]
        client = self.by_token.get(token)
        if client:
            # Same player, maybe a new public port (NAT rebinding): keep the address.
            self.by_addr.pop(client.addr, None)
            client.addr, client.build, client.name, client.local = addr, build, name, local
        else:
            client = Client(token, addr, self.alloc_vip(), build, name, local)
            self.by_token[token] = client
            self.by_vip[client.vip] = client
            log.info("client %s %s from %s, game %s", ip_str(client.vip), name, addr, build)
            self.peak_clients = max(self.peak_clients, len(self.by_token))
        self.by_addr[addr] = client
        client.last_seen = time.monotonic()
        pub_ip, pub_port = client.public
        self.send(addr, header(WELCOME) + struct.pack(">I", client.vip) + pack_ep(pub_ip, pub_port))

    def on_room_open(self, client: Client, r: Reader) -> None:
        public = r.u8() != 0
        name = r.string()
        players = r.u8()
        maximum = r.u8()
        code = self.hosted.get(client.vip)
        room = self.rooms.get(code) if code else None
        if room:
            room.public, room.name, room.players, room.max = public, name, players, maximum
        else:
            code = self.new_code()
            room = Room(code, client, public, name, players, maximum)
            self.rooms[code] = room
            self.hosted[client.vip] = code
            client.rooms.add(code)
            log.info("room %s opened by %s (%s, %d/%d)", code, client.name,
                     "public" if public else "private", players, maximum)
        self.send(client.addr, header(ROOM_OPENED) + pack_str(code))

    def on_room_list(self, client: Client) -> None:
        rooms = [room for room in self.rooms.values()
                 if room.public and room.build == client.build and room.players < room.max
                 and room.host is not client][:MAX_LIST]
        body = bytes([len(rooms)])
        for room in rooms:
            body += pack_str(room.code) + pack_str(room.name) + bytes([room.players, room.max])
        self.send(client.addr, header(ROOM_LIST_RESULT) + body)

    def on_connect(self, client: Client, r: Reader) -> None:
        code = r.string().strip().upper().replace("-", "")
        room = self.rooms.get(code)

        def reply(ok: bool, message: str, host_vip: int = 0) -> None:
            self.send(client.addr, header(CONNECTED) + bytes([1 if ok else 0]) +
                      pack_str(message) + struct.pack(">I", host_vip))

        if room is None:
            reply(False, "no room with this code")
            return
        if room.build != client.build:
            reply(False, f"room uses game version {room.build}, you have {client.build}")
            return
        if client.vip not in room.members and len(room.members) >= MAX_ROOM_MEMBERS:
            reply(False, "room is full")
            return
        for member in list(room.members.values()):
            if member is not client:
                self.introduce(client, member)
        if client.vip not in room.members:
            room.joined += 1
        room.members[client.vip] = client
        client.rooms.add(code)
        log.info("%s connected to room %s (%d members)", client.name, code, len(room.members))
        reply(True, room.host.name, room.host.vip)

    def on_relay(self, client: Client, r: Reader) -> None:
        dst_vip = r.u32()
        payload = r.rest()
        dst = self.by_vip.get(dst_vip)
        if dst is None or len(payload) > MAX_RELAY or not (client.rooms & dst.rooms):
            return  # only between players of a common room
        self.send(dst.addr, header(RELAYED) + struct.pack(">I", client.vip) + payload)
        client.relayed_bytes += len(payload)
        self.relay_packets += 1
        self.relay_bytes += len(payload)
        self.relay_players.add(client.vip)
        for code in client.rooms & dst.rooms:
            self.rooms[code].relayed = True

    def expire(self) -> None:
        now = time.monotonic()
        for client in list(self.by_token.values()):
            if now - client.last_seen > CLIENT_TIMEOUT:
                self.drop_client(client, "timed out")

    def log_stats(self, seconds: float) -> None:
        """One "stats:" line with the current numbers."""
        public = sum(1 for room in self.rooms.values() if room.public)
        builds: dict[str, int] = {}
        for client in self.by_token.values():
            builds[client.build] = builds.get(client.build, 0) + 1
        versions = ", ".join(f"{b or '?'}={n}" for b, n in sorted(builds.items())) or "-"
        log.info("stats: %d clients (peak %d), %d rooms (%d public), versions %s, "
                 "relay %d players %d packets %d KB in %.0f s",
                 len(self.by_token), self.peak_clients, len(self.rooms), public, versions,
                 len(self.relay_players), self.relay_packets, self.relay_bytes // 1024, seconds)
        self.peak_clients = len(self.by_token)
        self.relay_packets = self.relay_bytes = 0
        self.relay_players.clear()

    def run(self) -> None:
        self.sock.settimeout(1.0)
        last_expire = time.monotonic()
        last_stats = time.monotonic()
        while True:
            try:
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                data = None
            except ConnectionResetError:
                continue  # Windows: ICMP port unreachable from an earlier send
            if data:
                try:
                    self.handle(data, addr)
                except (ValueError, struct.error) as err:
                    log.debug("bad packet from %s: %s", addr, err)
            now = time.monotonic()
            if now - last_expire > 5.0:
                last_expire = now
                self.expire()
            if self.stats_wanted or now - last_stats >= STATS_INTERVAL:
                self.stats_wanted = False
                self.log_stats(now - last_stats)
                last_stats = now


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=int(os.environ.get("HRNP_PORT", 47660)))
    parser.add_argument("--bind", default=os.environ.get("HRNP_BIND", "0.0.0.0"))
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    log.info("HarmonyRecomp online server on UDP %s:%d", args.bind, args.port)
    server = Server(sock)
    if hasattr(signal, "SIGUSR1"):
        # `systemctl kill -s USR1 hrnp`: log a stats line now.
        signal.signal(signal.SIGUSR1, lambda *_: setattr(server, "stats_wanted", True))
    server.run()


if __name__ == "__main__":
    main()
