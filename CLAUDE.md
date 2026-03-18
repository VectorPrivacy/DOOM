# VectorDoom — Technical Reference

VectorDoom is a fork of [cloudflare/doom-wasm](https://github.com/nicholasopuni31/doom-wasm) (itself based on Chocolate Doom) modified to run as a **WebXDC** mini-app — a sandboxed HTML/JS/WASM bundle distributed inside chat apps (Delta Chat, etc.) with **zero internet access**. All networking uses `webxdc.joinRealtimeChannel()` for peer-to-peer messaging.

This document covers every significant modification from the original Chocolate Doom codebase.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────┐
│  WebXDC Chat App (Delta Chat, etc.)                 │
│  ┌───────────────────────────────────────────────┐  │
│  │  vectordoom.xdc (~4.2MB)                      │  │
│  │  ┌─────────────┐  ┌────────────────────────┐  │  │
│  │  │ index.html   │  │ webxdc-net.js (pre-js) │  │  │
│  │  │ UI + Touch + │  │ Server election +      │  │  │
│  │  │ Gamepad      │  │ Packet routing         │  │  │
│  │  └──────┬───────┘  └───────────┬────────────┘  │  │
│  │         │  inject_key_event()  │               │  │
│  │         │  inject_mouse_motion()               │  │
│  │         ▼                      ▼               │  │
│  │  ┌─────────────────────────────────────────┐   │  │
│  │  │  vector-doom.wasm (Chocolate Doom)      │   │  │
│  │  │  ┌──────────┐  ┌─────────────────────┐  │   │  │
│  │  │  │ Game     │  │ Net Layer           │  │   │  │
│  │  │  │ d_net.c  │  │ net_webxdc.c        │  │   │  │
│  │  │  │ g_game.c │  │ net_client.c        │  │   │  │
│  │  │  │ p_*.c    │  │ net_server.c        │  │   │  │
│  │  │  └──────────┘  └─────────────────────┘  │   │  │
│  │  └─────────────────────────────────────────┘   │  │
│  └───────────────────────────────────────────────┘  │
│                                                     │
│  webxdc.joinRealtimeChannel() ←→ other peers        │
└─────────────────────────────────────────────────────┘
```

**Authority model**: Host is authoritative for health, damage, NPC state, USE events, and respawns. Clients report actions; host approves and broadcasts results.

---

## 1. Network Transport — WebXDC RealtimeChannel

**Replaces**: `net_websockets.c` (WebSocket transport)
**New files**: `src/net_webxdc.c`, `src/net_webxdc.h`

### Packet Format

All packets sent through the realtimeChannel use this wire format:

```
[to: uint32 LE][from: uint32 LE][doom_payload: bytes]
```

The JavaScript layer (`webxdc-net.js`) strips the `to` field before delivery to C, so the C code receives `[from(4)][doom_payload]`.

### instanceUID System

Every instance gets a unique 32-bit identifier:
- **Server**: `instanceUID = 1` (hardcoded)
- **Clients**: `instanceUID = rand() % 0xfffe` (1–65534)

Defined as `uint32_t instanceUID` in `d_loop.c:46`. Used as the sender/receiver address in all packets.

### Server Election (`webxdc-net.js`)

Since WebXDC has no central server or DNS, the first instance to open becomes the server via a timestamp-based election:

1. Each instance broadcasts a **request** every 300ms: `[42, 42, 42, 42]` (4 magic bytes)
2. Each instance responds with: `[43, 43, 43, 43][unused(4)][timestamp(8)]` — where timestamp is `thisAppStartedAt`
3. The instance with the **earliest** timestamp wins and becomes server
4. 3-second timeout: if no earlier server responds, self-elect
5. Server re-announces every 3 seconds so late-opening instances discover it

The election result is exposed as `globalThis._serverElectionP` (a Promise resolving to `{ isServer: boolean }`).

### Packet Routing

JavaScript filters incoming packets by destination UID. Only packets addressed to `_doomInstanceUID` (or broadcast address `0`) are delivered to the C receive queue (`_webxdcRecvQueue`).

### Transport Module (`net_webxdc.c`)

Implements the standard `net_module_t` interface:
- `NET_WebXDC_SendPacket()` — builds `[to(4)][from(4)][payload]` and calls `js_webxdc_send()`
- `NET_WebXDC_RecvPacket()` — polls `js_webxdc_recv()` for queued packets
- `NET_WebXDC_ResolveAddress()` — maps string address (e.g. "1") to `net_addr_t`
- Address table: static array of `net_addr_t` entries keyed by IP (instanceUID)

---

## 2. Custom Packet Types

Added to `net_defs.h` beyond standard Chocolate Doom:

| Packet Type | Direction | Purpose |
|---|---|---|
| `PLAYER_STATE` | All → All | Position/angle/momentum snapshot (desync correction) |
| `HEALTH_AUTH` | Host → Clients | Authoritative health, playerstate, scores |
| `RESPAWN_REQUEST` | Client → Host | Request respawn after death |
| `DAMAGE_EVENT` | Client → Host | "I hit player X for N damage" |
| `NPC_STATE` | Host → Clients | NPC positions, sector heights, missiles |
| `NPC_DAMAGE` | Client → Host | "I hit NPC X for N damage" |
| `USE_EVENT` | Client → Host | "I pressed USE" (doors/switches) |
| `CHAT_MSG` | Any → All (via server) | Full chat message (bypasses ticcmd chatchar) |
| `PLAYER_NAME` | Any → All (via server) | Player name broadcast |
| `KILL_MSG` | Host → Clients | Kill notification |
| `PING` / `PONG` | Bidirectional | Latency measurement |
| `PLAYER_JOINED` | Host → Clients | Mid-game join announcement |

### Sending (net_client.c)

Each custom packet type has a corresponding `NET_CL_Send*()` function. The server forwards most of these to all other clients (relay pattern).

### Server Handling (net_server.c)

The server's packet dispatch (`NET_SV_Packet()`) routes custom packets to handlers that either:
- Forward to all other clients (chat, names, kills, state)
- Forward to host's game logic (damage, NPC damage, USE, respawn)
- Forward to specific recipients (pong)

---

## 3. Snapshot & Interpolation System

**Problem**: Standard Chocolate Doom uses lockstep networking — both sides must execute identical ticcmds. Over WebXDC's unreliable broadcast channel, ticcmds can arrive late or out of order, causing desync.

**Solution**: Periodic position snapshots with exponential smoothing interpolation.

### Snapshot Transmission (`d_loop.c`)

Every 2 tics (`SNAPSHOT_INTERVAL`), each instance sends its local player's state:

```c
// In TryRunTics(), after running each tic:
if (gametic % SNAPSHOT_INTERVAL == 0 && gametic > BACKUPTICS) {
    D_BuildAndSendSnapshot();      // All players send position
    if (is_host) {
        D_BuildAndSendHealthAuth(); // Host sends health authority
        D_BuildAndSendNPCState();   // Host sends NPC state
    }
}
```

### Player Snapshot Format (`doom/d_net.c`)

9 ints: `[x, y, z, angle, momx, momy, momz, attack_weapon, latency]`

- `attack_weapon`: 0 = idle, 1–9 = attacking with weapon (readyweapon + 1)
- `latency`: round-trip time from `NET_CL_GetLatency()`

### Player Interpolation (`D_TickPlayerInterp` in `doom/d_net.c`)

Remote players skip DOOM's physics engine entirely. Instead, `P_MobjThinker()` calls `D_TickPlayerInterp()` which:

1. **Extrapolates** the target position forward using stored momentum (`target_x += momx` each tic)
2. **Smooths** toward the target: each tic, close 60% of the remaining gap (`INTERP_FRAC = 0.6 * FRACUNIT`)
3. **Teleport detection**: if position delta > 128 map units, snap instantly (respawn/teleport)
4. **Angle is NOT interpolated** — `P_MovePlayer()` applies ticcmd `angleturn` deterministically, which stays in sync across machines. (Interpolating angle toward a stale snapshot fights the ticcmd and causes visible rotation errors.)

### Attack Animation Sync

Remote players' attack animations are driven by snapshots, not ticcmds:
- When a snapshot reports `attack_weapon > 0`, set `S_PLAY_ATK1` state and play the weapon fire sound
- **Animation lock** (`ATTACK_LOCK_TICS = 8`): prevents local state machine from overriding the attack animation for ~228ms
- `last_received_attack[]` array breaks feedback loops — stores the raw received flag, not the lock-modified state

---

## 4. Health & Damage Authority

### Host-Authoritative Health (`doom/d_net.c`)

The host broadcasts every 2 tics:
```
Per player (5 ints): [health, playerstate, killcount, itemcount, secretcount]
```

Clients apply this as ground truth:
- Host says dead, client says alive → `P_KillMobj()`
- Host says alive, client says dead → `PST_REBORN` for respawn
- Health decreased → play pain animation

### Event-Based Damage (`doom/p_inter.c`)

The original DOOM applies damage locally via `P_DamageMobj()`. VectorDoom changes this to an event-based authority model:

**Client side** (in `P_DamageMobj`):
- If client hits another player: send `DAMAGE_EVENT` to host, apply visual feedback only (screen flash, attacker tracking), don't reduce health locally
- If client hits an NPC (has `net_id`): send `NPC_DAMAGE` to host, don't apply locally

**Host side**:
- `HandleDamageEvent()`: receives client's damage report, sets `damage_from_event = true`, calls `P_DamageMobj()` to apply real damage
- The `damage_from_event` flag bypasses the "remote source skip" — without it, the host would ignore damage from remote players' ticcmds AND events

**Double-damage prevention**: The host receives both the ticcmd attack AND the damage event. The remote-source skip blocks the ticcmd path; only the event path applies damage.

### USE Event Authority

Clients send `USE_EVENT` when pressing the use key. Host calls `P_UseLines()` for the requesting player. This ensures doors/switches are only activated with host validation.

### Respawn Authority

Clients send `RESPAWN_REQUEST` after death. Host sets `players[player].playerstate = PST_REBORN`.

---

## 5. NPC Network Synchronization

**New files**: `src/doom/p_netsync.c`, `src/doom/p_netsync.h`

### Net ID Registry

Every `MF_COUNTKILL` mobj (monsters) and barrels get a unique `net_id` via `P_NetAssignId()`. This allows cross-machine references:

```c
mobj_t *net_mobj_table[MAX_NET_MOBJS];  // Lookup by net_id
unsigned short net_id_counter;           // Sequential allocator
```

### NPC Snapshot Format

Host broadcasts NPC state in a compact binary format:

```
[npc_count: u8][gametic: u16]
Per NPC (20 bytes): net_id(2) x(4) y(4) z(4) angle(1) statenum(2) health(2) flags(1)
[sector_count: u8]
Per changed sector (10 bytes): sector_id(2) ceiling(4) floor(4)
[missile_count: u8]
Per missile (32 bytes): source_net_id(2) type(2) x(4) y(4) z(4) momx(4) momy(4) momz(4) angle(4)
```

### Client-Side NPC Handling (`p_mobj.c`)

For clients (`net_client_player`), `P_MobjThinker()` skips physics for all NPCs with `net_id > 0` — only animation tic countdown runs. Host snapshots drive their positions.

---

## 6. Mid-Game Join

### Server Side (`net_server.c: HandleLateJoin`)

When the server (already in-game) receives a new client SYN:

1. Clear any slots the new client was assigned by `NET_SV_AssignPlayers()` (prevents double-slot bug)
2. Find a free `sv_players[]` slot
3. Broadcast `NET_PACKET_TYPE_PLAYER_JOINED` to all existing clients
4. Send all known player names to the new client

### Client Side (`d_loop.c: D_PlayerJoinedMidGame`)

When any instance receives the join announcement:
1. Set `local_playeringame[player] = true`
2. Call `loop_interface->PlayerJoined(player)` — which re-broadcasts the local player's name so the joiner learns it

### Tic Synchronization

Late joiners receive `settings->start_tic > 0`, which syncs their `recvtic`, `maketic`, and `gametic` to the server's current tic counter. This aligns the 8-bit tic sequence numbers.

### Ghost Body Prevention (`p_mobj.c: P_SpawnPlayer`)

When a player spawns, any existing mobj is cleaned up:
```c
if (p->mo) {
    p->mo->player = NULL;
    P_RemoveMobj(p->mo);
    p->mo = NULL;
}
```

### Player Quit Cleanup (`doom/d_net.c: PlayerQuitGame`)

When a player disconnects, their mobj is removed from the world:
```c
if (player->mo) {
    player->mo->player = NULL;
    P_RemoveMobj(player->mo);
    player->mo = NULL;
}
```

---

## 7. Player Names & Chat

### Name System (`doom/hu_stuff.c`)

Player names are statically initialized to color defaults (`"Green"`, `"Indigo"`, `"Brown"`, `"Red"`) and updated via `HU_SetPlayerName()` when `PLAYER_NAME` packets arrive. Names are stored in `net_player_names[4][]`.

**Key fix**: `HU_Init()` no longer calls `HU_InitPlayerNames()` — the old code reset names to defaults *after* `D_CheckNetGame()` had set the real names from the network.

### Chat Messages

Full messages are sent out-of-band via `CHAT_MSG` packets (bypassing the 1-char-per-tic `ticcmd.chatchar` limitation). Displayed via `HU_DisplayNetMessage()` with player color prefixes.

### Kill Messages

Host broadcasts kill notifications via `KILL_MSG` packets when a player dies in a netgame. Displayed via `HU_DisplayKillMessage()`.

---

## 8. Input System — Emscripten Exports

**File**: `src/i_input.c`

Two C functions exported to JavaScript via `EMSCRIPTEN_KEEPALIVE`:

```c
void inject_key_event(int type, int key);    // type: 0=down, 1=up
void inject_mouse_motion(int dx, int dy);    // dx=turn, dy=forward
```

These bypass SDL entirely and inject events directly into DOOM's `D_PostEvent()` queue. Used by:
- **Mobile touch controls**: Virtual joysticks + action buttons
- **Gamepad support**: Web Gamepad API polling

### Key Codes Used

| Code | Key | Action |
|---|---|---|
| `0xad` | `K_UP` | Move forward |
| `0xaf` | `K_DOWN` | Move backward |
| `97` ('a') | `K_STRAFE_L` | Strafe left |
| `100` ('d') | `K_STRAFE_R` | Strafe right |
| `32` (Space) | `K_FIRE` | Fire weapon |
| `101` ('e') | `K_USE` | Use/interact |
| `0xb6` | `K_RSHIFT` | Run |
| `9` (Tab) | `K_TAB` | Toggle map |
| `49`–`55` ('1'–'7') | Weapon slots | Select weapon |

---

## 9. Frontend (`src/index.html`)

### Game Flow

1. **Loading**: WASM loads + server election runs in parallel
2. **Menu**: Host sees name input + game mode + "Start Game"; client sees name input + "Waiting for host..."
3. **Play**: Menu hides, canvas shows, `callMain(args)` launches DOOM engine

### Launch Arguments

- Common: `-iwad doom1.wad -window -nogui -nomusic -nosound -config default.cfg -servername vectordoom -pet <name>`
- Host adds: `-server -privateserver [-deathmatch]`
- Client adds: `-connect 1`

### Mobile Touch Controls (`setupMobileControls()`)

- Left zone: floating joystick for movement (deadzone 0.15, state-change key injection)
- Right zone: floating joystick for look/turn (continuous `inject_mouse_motion()` at 30fps)
- Buttons: Fire, Use, Map toggle, Weapon prev/next (cycle slots 1–7)
- Canvas touch events blocked to prevent SDL mouse interpretation

### Gamepad Controller Support (`setupGamepad()`)

Web Gamepad API with `requestAnimationFrame` polling:

| Input | Action |
|---|---|
| Left Stick | Movement (deadzone 0.25, auto-run > 0.7) |
| Right Stick | Turn via `inject_mouse_motion()` (deadzone 0.15, 75px sensitivity) |
| D-Pad | Alternative movement (only when left stick idle) |
| R2 Trigger | Fire (threshold > 0.3) |
| A / Cross | Use/interact |
| R1 / L1 Bumpers | Weapon cycle next/prev (250ms debounce) |
| Start | Map toggle |

Auto-detects on `gamepadconnected` event or pre-connected gamepads.

### CRT Monitor UI

- 4:3 aspect ratio frame with `#59fcb3` border and glow
- Scanline overlay effect
- Orange power LED with pulse animation
- DOOM artwork background (`bg.jpg`) with massive dark vignette shadow
- Portrait mobile: shows rotate-to-landscape prompt
- Landscape mobile: CRT frame hidden, fullscreen canvas

---

## 10. Build System

### Compilation

```bash
export PATH="/tmp/emsdk:/tmp/emsdk/upstream/emscripten:/tmp/emsdk/node/22.6.0_64bit/bin:/tmp/emsdk/python/3.13.3_64bit/bin:$PATH"
emmake make -j4
```

**Key Makefile note**: `webxdc-net.js` is passed as `--pre-js` in LDFLAGS only (not CFLAGS). It runs before the WASM module initializes, setting up the realtimeChannel and server election.

### Packaging (`build-xdc.sh`)

1. Copy compiled assets to `_xdc_staging/`
2. Minify JS with terser (parallel)
3. Minify HTML (strip comments/whitespace)
4. Package as `.xdc` (renamed `.zip`) with max compression

### .xdc Contents

| File | Size (compressed) | Purpose |
|---|---|---|
| `vector-doom.wasm` | ~2.7MB | DOOM engine (Emscripten) |
| `doom1.wad` | ~1.7MB | Shareware game data |
| `vector-doom.js` | ~160KB min | Emscripten runtime |
| `index.html` | ~12KB | UI + controls |
| `webxdc-net.js` | ~3KB | Server election + routing |
| `bg.jpg` | ~100KB | Background artwork |
| `icon.png` | ~14KB | App icon (256x256) |
| `default.cfg` | ~2KB | Default config |
| `manifest.toml` | ~100B | WebXDC metadata |

**Total**: ~4.2MB

---

## 11. Key Differences from Original Chocolate Doom

| Feature | Chocolate Doom | VectorDoom |
|---|---|---|
| Transport | TCP/WebSocket | WebXDC realtimeChannel (UDP-like broadcast) |
| Server discovery | IP address / DNS | Magic byte election + timestamp |
| Addressing | IP:port | instanceUID (uint32) |
| Sync model | Pure lockstep | Lockstep + snapshot correction |
| Position sync | Lockstep only | Snapshots every 2 tics + interpolation |
| Health authority | Both sides (lockstep) | Host-authoritative broadcast |
| Damage authority | Local physics | Event-based: client reports, host approves |
| NPC authority | Lockstep | Host broadcasts snapshots |
| Mid-game join | Not supported | Supported with tic sync + join announcement |
| Player names | Not transmitted | Broadcast at connect + on join |
| Chat | 1 char/tic via ticcmd | Full messages, out-of-band packets |
| Kill notifications | Local console only | Broadcast to all players |
| Input | SDL keyboard/mouse | SDL + JS inject (touch, gamepad) |
| Platform | Native / Emscripten | Emscripten inside WebXDC sandbox |

---

## 12. Known Issues

### Mid-Game Join Sprite Crash (Deferred)

Late joiners can crash with `R_ProjectSprite: invalid sprite frame 25 : 32769`:
- Sprite 25 = `SPR_BFE2` (BFG explosion, not in shareware WAD)
- Frame 32769 = `0x8001` (frame 1 + fullbright flag)
- Root cause: `ingame[consoleplayer]` flips false on some tics, triggering a PlayerQuitGame → rejoin cycle that corrupts psprite state
- The double-slot fix is in place but the psprite corruption during the quit/rejoin cycle remains unresolved
