# HarmonyRecomp online server

Lets players host and join co-op games without a VPN:

- every player gets a virtual address (10.66.x.y) that the game uses,
- rooms get a short code (e.g. `K7Q2XM`) to join with; public rooms are listed for
  everyone with the same game version and are found by the game's own search,
- players are introduced to each other and connect **directly** (UDP hole punching),
- if a direct connection is impossible (strict routers, CGNAT), their game packets are
  **relayed** through the server.

The server only forwards packets; it stores nothing on disk. Traffic is tiny: a few KB/s
per relayed player.

## Running it

Python 3.8+, no extra packages, one UDP port (default `47660`):

```sh
python3 hrnp_server.py            # --port 47660 --verbose
```

Open the port in the firewall (`ufw allow 47660/udp`).

### As a service (Debian/Ubuntu)

```sh
sudo useradd --system --no-create-home hrnp
sudo mkdir -p /opt/hrnp && sudo cp hrnp_server.py /opt/hrnp/
sudo tee /etc/systemd/system/hrnp.service >/dev/null <<'EOF'
[Unit]
Description=HarmonyRecomp online server
After=network-online.target

[Service]
User=hrnp
ExecStart=/usr/bin/python3 /opt/hrnp/hrnp_server.py --port 47660
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl enable --now hrnp
journalctl -u hrnp -f              # log: clients, rooms, a stats line every 5 min
```

## Using it in the game

F6 > Online: switch on online co-op, enter the server as **Online server**
(`host` or `host:port`), Save, restart. The Online tab then shows your room code while
you host, lets you join a room by code, and lists public rooms. Everyone in a game must
use the same server; players on it cannot see LAN/VPN-only players and vice versa.

## Protocol

See the comment at the top of `hrnp_server.py` and `src/netplay/online.cpp`.
