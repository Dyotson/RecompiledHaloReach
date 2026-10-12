# Hosting a Reach Live server

`server/` is a standard-library Python server: UDP rendezvous/relay plus HTTP title
services and a JSON status page. Players behind NAT need no port forwarding: they hole punch
to each other and fall back to the server relay.

| Port | Protocol | Use |
|------|----------|-----|
| 21100 | UDP | game, rendezvous, relay |
| 21101 | TCP | title services (HTTP), status page at `/` |

Forward both on the router to the host. Players set `live_server = "your.host:21100"`
(or `REACH_SERVER=your.host:21100`); the HTTP port is told to the client in the welcome
and always goes to the same IP as the UDP address, so LAN players can use the LAN IP and
everyone else the public name.

## Docker

    docker build -t reach-live server/
    docker run -d --name reach-live --restart unless-stopped --network host \
        -v /path/to/data:/data reach-live --host 0.0.0.0

`--network host` matters: with a bridge network Docker's UDP proxy hides the players' real
public ip:port, which hole punching needs. Extra flags go after the image name (`--motd`,
`--max-peers`, `--rate-limit`, `--http-port`). Data (rewards, shared files) is in `/data`.

Unraid: copy `server/` to `/mnt/user/appdata/reach-live/src`, build there with
`docker build -t reach-live .`, run as above with `-v /mnt/user/appdata/reach-live/data:/data`.

## Limits (all untrusted input)

- per address: 16 players, 1500 datagrams/s, 2 new registrations/s, 32 HTTP connections;
- per player: relay/broadcast budget 64 KB/s (`--rate-limit`), 24 shared files, 100 MB,
  8 unfinished uploads, one file at most 32 MB; HTTP bodies up to 16 MB;
- whole server: shared files at most `REACH_LIVE_DISK_CAP_MB` (default 4096) MB, at most
  `REACH_LIVE_MAX_PLAYERS` (20000) player records;
- the status page lists gamertags and XUIDs but no IP addresses.

There are no admin endpoints. Players are not authenticated (their XUID is whatever the
client claims); this is a trust-your-friends server.
