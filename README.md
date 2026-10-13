# ESP32-S3 Tailscale Exit Node

An ESP32-S3 N16R8 that joins your [Tailscale](https://tailscale.com) tailnet and offers
itself as an **exit node**: a phone or laptop anywhere on the tailnet can send its internet
traffic out through this board's Wi-Fi, as if it were on your home network.

It's built on [MicroLink](https://github.com/CamM2325/microlink), an unofficial Tailscale
client for ESP-IDF, with a patch set that adds exit-node support and fixes several
data-path problems found while testing on real hardware (see
[MicroLink changes](#microlink-changes)).

Nothing user-specific is compiled in. Wi-Fi credentials, the Tailscale auth key and the
admin password are entered in a setup portal on first boot and stored in NVS, so one
`.bin` can be shared and flashed to any N16R8.

> **Unofficial, and modest.** MicroLink isn't made or supported by Tailscale. Measured
> throughput is about **1 Mbit/s** to a phone on cellular, which is fine for browsing,
> messaging and email but not video. For heavy use, a Raspberry Pi or PC running the official
> client is far faster. See [Performance](#performance).

## Contents

- [How it works](#how-it-works)
- [Hardware](#hardware)
- [Build and flash](#build-and-flash)
- [First boot](#first-boot)
- [Settings page](#settings-page)
- [ntfy: addresses and commands](#ntfy-addresses-and-commands)
- [Performance](#performance)
- [Troubleshooting](#troubleshooting)
- [Security notes](#security-notes)
- [Limitations](#limitations)
- [Project layout](#project-layout)
- [MicroLink changes](#microlink-changes)

## How it works

```
 phone (cellular)                          ESP32-S3                         internet
 ────────────────       WireGuard           ────────            NAT          ────────
 tailscale0  ── DERP relay (or direct) ──▶  wg netif ──▶ lwIP forward ──▶  Wi-Fi STA ──▶ router
 100.x.y.z     0.0.0.0/0 via exit node      100.a.b.c      + NAPT          192.168.x.y
```

1. **Advertise.** MicroLink registers the board and advertises `0.0.0.0/0` + `::/0`,
   which marks it as an exit node. You approve it once in the admin console.
2. **Forward.** Clients using it send all their traffic over WireGuard. Decrypted packets
   enter lwIP on the WireGuard interface and are forwarded out of Wi-Fi, with NAPT
   rewriting the source to the board's LAN address. Replies are translated back and
   re-encrypted to the client.
3. **DNS.** Clients using an exit node send every lookup to the exit node's *peer API*
   (`http://100.a.b.c:41112/dns-query`, DNS-over-HTTP). The board forwards each query to
   your LAN's DNS server (router, Pi-hole, … from DHCP; 1.1.1.1 as fallback), so remote
   clients resolve names exactly as devices at home do.
4. **Paths.** Peers start on a Tailscale DERP relay. A direct UDP path is used only once a
   DISCO probe has confirmed it. At start-up the board measures every DERP region and uses
   the closest.

## Hardware

| Board | Flash | PSRAM | Status |
|---|---|---|---|
| **ESP32-S3 N16R8** | 16 MB | 8 MB octal | tested on hardware, v1.3.2 |

- **PSRAM is required.** MicroLink keeps ~1 MB of buffers there, and queued packets and
  all TLS buffers (relay, ntfy, public-IP lookup) live there too, which keeps ~90 KB of
  internal RAM free under load.
- **2.4 GHz Wi-Fi only.** The ESP32-S3 has no 5 GHz radio.
- **Good signal matters more than anything else.** At −80 dBm the link lost 10% of
  packets and the relay connection reset every minute or two. At −54 dBm loss was 0% and
  throughput went up about 5×. Aim for **−67 dBm or better**; the boot log prints `rssi:`.
  Place the board near the router, or use a board with an external antenna.

## Build and flash

ESP-IDF **v5.5** (MicroLink upstream is tested on v5.3).

```bash
./build.sh
```

That builds and writes two images to `dist/`:

| File | Use | Flash at |
|---|---|---|
| `ts-exit-node-v<ver>-esp32s3-n16r8.bin` | **new board** (bootloader + partitions + app) | `0x0`, **wipes settings** |
| `ts-exit-node-v<ver>-esp32s3-n16r8-app.bin` | **upgrade** (app only) | `0x20000`, keeps settings |

New board (erases everything first):

```bash
./build.sh flash /dev/cu.usbserial-XXXX
```

Upgrade, keeping Wi-Fi settings and the Tailscale identity:

```bash
./build.sh upgrade /dev/cu.usbserial-XXXX
```

> **Never upgrade with the full image.** It starts at `0x0` and pads over the NVS partition
> at `0x9000`, so the board comes back unprovisioned, as a *new* Tailscale machine needing
> a new auth key.

If your ESP-IDF install has no Xtensa toolchain, `build.sh` uses the identical compiler
bundled with the Arduino ESP32 core (`~/Library/Arduino15/...`), so nothing extra needs
installing. Flashing without the source tree, offsets, and distributing binaries are covered
in [flash.md](flash.md).

## First boot

1. Flash it and power the board.
2. Join the open Wi-Fi network **`TS-Exit-Setup-XXXX`**. The suffix is the end of the
   board's MAC address.
3. The setup page opens by itself. If it doesn't, browse to **http://192.168.4.1**.
4. Fill in the form, then **Save & restart**:

   | Section | Fields |
   |---|---|
   | Wi-Fi | network (picked from a live scan) and password, **2.4 GHz only** |
   | Tailscale | auth key, device name (optional) |
   | Exit node | offer as exit node (on), block access to private networks (on), DERP relay region (blank = automatic) |
| ntfy (optional) | server, port, TLS, topic, login, command passphrase; see [ntfy](#ntfy-addresses-and-commands) |
   | Admin password | for the settings page later, username **`admin`** |

5. In the [Tailscale admin console](https://login.tailscale.com/admin/machines), find the
   device (default name `esp32-exit-xxxx`) and:
   - choose **⋯ → Edit route settings → Use as exit node**;
   - choose **⋯ → Disable key expiry**, so it doesn't drop off the tailnet in 180 days.
6. On a phone or laptop: Tailscale → **Exit node** → pick the device.

### Auth key

Generate one at
[login.tailscale.com/admin/settings/keys](https://login.tailscale.com/admin/settings/keys).
A **single-use** key is enough. After the first successful registration the board
re-registers on later boots using only its node key; if that is ever refused, it reboots
once and retries with the stored auth key.

To skip the manual approval step, add an auto-approver to your tailnet policy file:

```jsonc
"autoApprovers": { "exitNode": ["tag:exit-node"] }   // or a user, e.g. "you@example.com"
```

Clients also need ACL access to exit nodes, e.g.
`{"action": "accept", "src": ["autogroup:member"], "dst": ["autogroup:internet:*"]}`. The
default allow-all policy already grants it.

## Settings page

After setup the page is at the board's **LAN address** and its **Tailscale 100.x address**,
behind the browser's sign-in pop-up (user **`admin`**). `GET /status` returns the same data
as JSON.

### Status rows

| Row | Meaning |
|---|---|
| Wi-Fi | network, LAN IP, signal (dBm) |
| Tailscale | state, VPN IP, and the control-plane error if registration was refused |
| Peers | total, how many are on a direct path, how many go via a DERP relay |
| DERP relay | home relay region in use, and whether measured or fixed in settings |
| Exit node | advertised or off, LAN blocking |
| Forwarded | packets/bytes forwarded from clients to the internet (outbound direction only) |
| Dropped | LAN (blocked private addresses), IPv6, exit off, guard (frames trying to use the board as a router), shed under load, stack full |
| DNS for clients | DoH queries answered, failures, average upstream latency |
| Public IP | home WAN address (looked up every 15 min, needs ntfy on) |
| ntfy | off / connecting / subscribed / reconnecting / login refused |
| Memory free | internal heap now and lowest since boot, PSRAM |

### Changing settings

| Change | Effect |
|---|---|
| Wi-Fi or admin password | blank keeps the stored one |
| **new auth key** | the board forgets its node identity and registers as a *new* machine. Delete the old one in the admin console. |
| "Forget Tailscale identity" | same, with the stored key |
| device name | renames the machine on next boot (unless renamed in the admin console) |
| exit node off | stops advertising routes and the DNS endpoint; forwarded traffic is dropped |
| DERP relay region | blank = lowest latency at start-up; a number (e.g. `10` = Seattle) fixes it |

### Recovery

| Situation | What happens |
|---|---|
| Wi-Fi unreachable at boot, or lost for over 30 s | the `TS-Exit-Setup-XXXX` AP comes up (admin login required) while the station retries every 10 s; it leaves AP mode once the router is back |
| auth key rejected | **no reboot loop**; the error shows on the settings page and a new key can be entered there |
| forgot admin password / moved house | press **EN**, release, then **hold BOOT within 2 s**. The setup portal opens **without login**, pre-filled; nothing is erased. |
| wipe everything | `esptool erase-flash`, then flash the full image |

## ntfy: addresses and commands

Optional, as in WOL-NTFY: set a topic on the settings page and the board tells you where it
is, and takes commands from the [ntfy](https://ntfy.sh) app.

**Messages it sends** (title = device name + event):

- **online** after each boot, once Tailscale is connected (or after 60 s regardless);
- **address changed** when the LAN, Tailscale or public address changes (held for 10 s);
- **Tailscale refused** if registration fails, with the server's reason.

```
Wi-Fi     MyHomeWiFi, -54 dBm (excellent)
LAN       192.168.1.50
Tailscale 100.101.102.103  esp32-exit-c9ac, connected, exit node on
Public    203.0.113.5
Settings  http://192.168.1.50  http://100.101.102.103
fw 1.3.2, up 2h13m
```

Signal is rated excellent (≥ −55 dBm), good (≥ −67), fair (≥ −75) or weak (below that,
where packet loss starts hurting the relay). The public address comes from `https://api.ipify.org`, checked at start-up, every 15 minutes
and after Wi-Fi reconnects.

**Commands.** Send them to the same topic, **always prefixed with the command passphrase**.
Messages without it are ignored silently.

| Command | Effect |
|---|---|
| `<pass> status` (or `ip`) | addresses, plus peers, relay region, forwarded/shed traffic, DNS, memory |
| `<pass> reboot` | restart |
| `<pass> exit on` / `exit off` | offer or stop offering the exit node (saved; restarts) |
| `<pass> forget confirm` | new Tailscale identity on next boot (restarts); a used single-use key will be refused |
| `<pass> factoryreset confirm` | clear all settings and restart into the setup portal (identity kept) |
| `<pass> help` | the list |

Replies are tagged `tsexit`, so the board ignores its own messages coming back down the
topic. A restart waits for its reply to be sent first.

**Settings:** server (default `ntfy.sh`), port, TLS, topic (`[-_A-Za-z0-9]`, up to 64),
optional login (username + password, or a token), command passphrase (required with a
topic, must differ from the admin password).

> **Security.** On public ntfy.sh, anyone who knows the topic can read the messages,
> including your addresses and any passphrase you send. Use a long random topic, or a
> self-hosted / authenticated server. The passphrase is what protects the commands; it is
> never logged.

## Performance

Measured on hardware with a phone on cellular using the board as its exit node, via a DERP
relay:

| | Result |
|---|---|
| fast.com download | **~1 Mbit/s** |
| Relay round trip (phone ↔ board) | ~200–450 ms |
| DNS through the exit node | all queries answered |

Limits, in order:

1. **CPU.** WireGuard runs in software on 240 MHz Xtensa cores, plus TLS on the relay
   connection. Under load the decryption queue overflows (`drops wg_rx` in the log).
2. **Relay path.** A phone on cellular can rarely get a direct path through carrier NAT,
   and a home network behind two routers in a row makes it rarer still, so traffic goes via
   DERP and pays its round trip.
3. **Wi-Fi signal.** See [Hardware](#hardware). A weak signal brings everything else down
   with it.

When the internet side sends faster than the board can relay (any speed test), the board
**sheds** replies to forwarded flows at the door (`shed` counter). TCP sees the loss and
slows down, and the board's own connections (DERP, DNS, the portal) stay responsive.

## Troubleshooting

**Exit node selected, but nothing loads.** Check the status page:
- *DNS for clients* not rising → the client isn't sending DNS to the board. As a
  fallback that needs no firmware support: admin console → **DNS** → *Global nameservers*
  → add e.g. Cloudflare → tick **Use with exit node**.
- *Forwarded* not rising → the client isn't actually using the board. Check the phone's
  Tailscale app is connected (a phone often shows *offline* after sleeping) and
  re-select the exit node.

**Slow or stalling.** Check the Wi-Fi signal first (status page or boot log `rssi:`).
Then check the serial log's once-a-minute `diag:` line (below): frequent `relay resets`
point to the network path; high `drops wg_rx` means the board is at its CPU limit.

**LAN devices reachable even though "Block private networks" is on.** Another tailnet
machine is probably advertising your home subnet (a *Subnets* badge in the admin console,
e.g. Home Assistant advertising 192.168.0.0/24). Clients prefer that more specific route
over the exit node's `0.0.0.0/0`. If the board were the one forwarding, its *Dropped: LAN*
counter would rise instead.

### Serial log

115200 baud (`idf.py -p <port> monitor`). Per-packet logging is compiled out; once a
minute the board prints:

```
main: heap free 121911 (min 86140, largest block 48128), PSRAM free 8238136, tailscale connected
main: exit: fwd 4490 pkts / 1045 KB, shed 3586, stack full 0, dropped lan 0 ipv6 0; dns 263 queries (1 failed)
tailscale: diag: relay resets 1 | drops wg_rx 602 disco_rx 0 relay_tx 0 stale 14 | relay write max 23 ms (waits 57), read gap max 78 ms | queue max tx 22 wg_rx 60
```

| `diag:` field | Meaning |
|---|---|
| relay resets | DERP connection lost (any cause); each is followed by a `DERP reset context` line saying how long since the last receive/send |
| drops wg_rx / disco_rx | incoming packets dropped because the processing queue was full |
| drops relay_tx | outgoing relay packets dropped, queue full |
| stale | relay packets older than 3 s discarded (normal right after a reconnect) |
| relay write max / waits | slowest TLS write to the relay, and writes that had to wait (uplink congestion) |
| read gap max | longest time between checks for relay input |
| queue max | deepest relay TX and WireGuard RX queues since the last line |

Path changes appear as `path <host>: DERP -> direct a.b.c.d:p (rtt N ms)`. A warning
appears if internal RAM drops below 32 KB.

## Security notes

- **Block private networks** (default on) drops exit traffic to 10/8, 172.16/12,
  192.168/16, 169.254/16 and the board's own subnet. Without it, anyone allowed to use the
  exit node can reach devices on your home LAN.
- **Forwarding happens only from the WireGuard interface.** The Wi-Fi interfaces drop any
  frame not addressed to the board, so a LAN host or a setup-AP client can't route through
  it into the tailnet or the LAN.
- **The DNS endpoint answers tailnet peers only** (100.64.0.0/10); LAN requests get 403.
- **Secrets stay put.** The auth key, Wi-Fi password and admin password are never sent
  back to the browser or written to the log.
- **The settings page uses HTTP basic auth.** On the LAN that is plain HTTP, the same
  trade-off as most router UIs. Over the tailnet it is inside WireGuard.

## Limitations

- **IPv4 only.** `::/0` must be advertised for Tailscale to treat the node as an exit
  node, but IPv6 packets are dropped; clients fall back to IPv4.
- **Fragmented packets** crossing the NAT aren't reassembled, so non-first fragments
  can't be translated. TCP is unaffected.
- **NAT table:** 512 flows; idle UDP mappings expire after 30 s.
- **Direct paths** to cellular clients are rare (carrier NAT); expect relay latency.

## Project layout

```
main/
  main.c          boot flow; Setup / AP-fallback / Online state machine; per-minute stats
  settings.c      NVS config (namespace "tscfg")
  wifi.c          station / AP+STA, retry, Wi-Fi failure diagnosis
  webconfig.c     esp_http_server portal, basic auth, status page, /status JSON
  dns_captive.c   captive-portal DNS for the setup AP
  tailscale.c     MicroLink start-up, identity bookkeeping, diagnostics
  exitnode.c      forwarding policy, NAPT, input guard, load shedding, counters
  peerapi.c       DNS-over-HTTP endpoint for exit-node clients (port 41112)
  ntfy.c          ntfy subscribe stream + publish (one task owns all ntfy I/O)
  commands.c      passphrase-prefixed commands, status text
  wanip.c         public IP lookup (api.ipify.org)
  config.h        tunables (no credentials)
components/
  microlink/      MicroLink (MIT), vendored with the patches below
  wireguard_lwip/ WireGuard netif used by MicroLink
partitions.csv    16 MB: nvs 64K, otadata, 2 × 3 MB app slots, 9.9 MB spare
sdkconfig.defaults
build.sh          build, package, flash (new board) or upgrade (app only)
flash.md          flashing and distributing binaries
```

## MicroLink changes

Vendored from the upstream commit in `components/microlink/UPSTREAM_COMMIT`. Every change is
marked `[exit-node patch]` in the source.

| Area | Change |
|---|---|
| Exit node | `advertise_exit_node` config; `RoutableIPs: ["0.0.0.0/0", "::/0"]` in every Hostinfo |
| DNS | `peerapi_port` config; `Services: peerapi4 + peerapi-dns-proxy` in Hostinfo, without which clients' exit-node DNS URL is just `"/dns-query"` and every lookup fails |
| Registration | reads `Error` / `AuthURL` / `MachineAuthorized` from the RegisterResponse; on refusal parks in `ML_STATE_ERROR` instead of retrying forever; `microlink_get_last_error()` |
| Path selection | peers start on DERP; only a DISCO-confirmed address becomes the WireGuard endpoint (upstream used the unverified netmap address, so replies to cellular clients were lost). Sticky best path with RTT hysteresis, any direct pong keeps it trusted, no forced re-handshake on endpoint change. |
| DERP region | home region chosen by measured TCP-connect latency (parallel scan, then sequential re-test of the best 5) or fixed via `derp_region`, and reported as `PreferredDERP`. Upstream always used region 9 (Dallas). |
| DERP robustness | TX queue survives reconnects (64 deep, 3 s max age); resumable frame reader (a delayed segment no longer blocks output or kills the connection); liveness ping reconnects a silently dead connection in ~10 s instead of ~20; shorter handshake timeout |
| Data path | decrypted packets go through `tcpip_input` on the TCPIP thread instead of `ip_input()` from the WireGuard task; inbound filter hook; RX queues 8 → 64 and the WireGuard task wakes on arrival instead of polling every 10 ms; queued packets in PSRAM |
| Diagnostics | `microlink_get_diag()` counters, `microlink_relay_backlog()`, reset-context log line; per-packet logging demoted to debug (it was capping throughput at 115200 baud) |

Build-wide lwIP NAPT tuning is in the top-level `CMakeLists.txt`. The NAT port range moves
to 20000–39999, because lwIP's default range contains 51820, MicroLink's WireGuard port.
UDP mappings last 30 s instead of 2 s.

## License

MicroLink (MIT) and wireguard-lwip keep their own licences
(`components/microlink/LICENSE`, `components/wireguard_lwip/LICENSE`); keep those files in
anything you distribute. The application code in `main/` has no licence file yet.
