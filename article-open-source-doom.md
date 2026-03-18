# Open Source DOOM: How We Brought Real-Time Multiplayer to a 1993 Classic -No Servers Required

*By [JSKitty](https://jskitty.cat)*

---

In May 2021, Cloudflare did something wonderful. They took DOOM -the 1993 game that defined an entire genre -compiled it to WebAssembly, wired up WebSocket multiplayer through their Durable Objects edge platform, and [wrote a fantastic blog post about it](https://blog.cloudflare.com/doom-multiplayer-workers/). As Celso Martinho put it: *"Running Doom is effectively the new 'Hello, World' in computing."*

We loved it. And then we thought: *what if we could take it further?*

What if multiplayer DOOM didn't need Cloudflare's servers -or anyone's servers? What if it didn't need the internet at all? What if you could send a 4MB file to a friend in a chat message and be fragging each other within seconds, purely peer-to-peer, with the game feeling like a modern real-time shooter instead of a 1994 LAN party?

That's what we built. We call it **Open Source DOOM**.

---

## What Cloudflare Built (And Why It's Great)

Credit where it's due. Cloudflare's [doom-wasm](https://github.com/cloudflare/doom-wasm) project did the hard work of porting Chocolate Doom -the faithful open-source recreation of the original DOOM engine -to WebAssembly using Emscripten. That alone is a serious engineering effort. They then wrote `net_websockets.c`, a transport adapter that replaced DOOM's original IPX/UDP networking with WebSocket connections routed through Durable Objects on Cloudflare's edge network.

Their architecture looked like this:

```
Player A (Browser)  ←WebSocket→  Cloudflare Edge (Durable Object)  ←WebSocket→  Player B (Browser)
```

A Durable Object acted as the game room -maintaining a routing table of connected players and forwarding packets between them. Clean, elegant, and a great demo of edge computing.

But there was a catch.

### What they kept the same

Cloudflare didn't modify DOOM's actual netcode. The game still used its original **lockstep networking** model from 1993 -the same protocol designed for four PCs on a local area network connected by coaxial cable. Every player sends their inputs (which keys they pressed) to every other player, every single frame, and **the game freezes until everyone's inputs arrive**.

As their blog post acknowledged: *"The game only advances when everyone receives the commands from all the other players in the group."*

This worked in 1993 when your LAN had sub-millisecond latency. Over the internet, through WebSockets, through a routing layer? It meant choppy gameplay, freezes whenever anyone's connection hiccupped, and a gameplay experience that felt more like a slideshow than a shooter.

The Durable Object was also a single point of failure and a centralised dependency. No Cloudflare, no DOOM.

---

## What We Changed (Everything Except the Game Itself)

We forked Cloudflare's doom-wasm project and rebuilt the entire networking stack from scratch. Not just the transport layer -the fundamental model of how the game synchronises state between players.

Here's the before and after:

| | Cloudflare's DOOM | Open Source DOOM |
|---|---|---|
| **Transport** | WebSockets → Cloudflare Edge | P2P gossip via Iroh (QUIC) |
| **Server** | Durable Object (centralised) | Auto-elected from players (decentralised) |
| **Sync model** | Lockstep (1993 original) | Real-time hybrid (snapshots + interpolation) |
| **Damage** | Simulated locally by all clients | Host-authoritative events |
| **NPCs/Monsters** | Simulated locally by all clients | Host-authoritative snapshots |
| **Late join** | Not supported | Fully supported |
| **Internet required** | Yes (Cloudflare Workers) | No (works offline, P2P) |
| **Delivery** | Website (silentspacemarine.com) | 4.2MB file in a chat message |

Let's break down each piece.

---

## 1. No Servers, No Problem

### The Simple Version

Cloudflare's DOOM needed their servers to work. Ours doesn't need *any* servers. When you open the game, your device automatically figures out who should be the "host" -no configuration, no IP addresses, no port forwarding, no sign-ups. It just works.

The game runs inside a **.xdc file** -essentially a tiny 4.2MB zip archive containing the entire game. You literally send it as a file in a chat message. Your friend opens it. You're playing DOOM together. The data travels directly between your devices through the chat app's peer-to-peer channels.

### The Nerdy Version

We replaced `net_websockets.c` with `net_webxdc.c` -a transport module that speaks the [WebXDC](https://webxdc.org/) realtime channel protocol instead of WebSockets.

WebXDC is an open standard for sandboxed web apps distributed inside chat messages. The app has **zero internet access** -no fetch, no XMLHttpRequest, nothing. The only communication channel is `webxdc.joinRealtimeChannel()`, which gives you an unreliable broadcast pipe to other instances of the same .xdc file in the same chat.

Under the hood (in our primary platform, [Vector](https://github.com/nicholasopuni31/vector)), this channel is backed by [Iroh](https://iroh.computer/) -a QUIC-based peer-to-peer gossip protocol. Messages travel directly between devices, relayed through lightweight Iroh relay nodes only when direct connections aren't possible. There are no game servers, no routing tables, no Durable Objects.

The wire format is minimal:

```
[to: uint32 LE (4 bytes)][from: uint32 LE (4 bytes)][doom_payload]
```

JavaScript routes incoming packets by destination UID -only packets addressed to you (or broadcast address `0`) get delivered to the WASM engine. Everything else is silently dropped.

For performance, Vector provides a **WebSocket fast-path**: the app connects to `ws://127.0.0.1:{random_port}/{token}` -a localhost WebSocket server spun up by the Rust backend -giving near-zero-latency access to the Iroh gossip sender. Binary frames go straight to `sender.broadcast()` with zero copies. Fallback path uses Tauri invoke with base91 encoding.

---

## 2. Who's the Server? Magic.

### The Simple Version

In a normal online game, someone runs a server, and everyone connects to it. In Open Source DOOM, there *is* no predetermined server. When you open the game, all players silently negotiate who becomes the host. The person who opened the game first wins. This happens automatically in about three seconds, and you never even notice it.

### The Nerdy Version

Server election uses a dead-simple timestamp protocol over the broadcast channel:

1. Every instance broadcasts **4 magic bytes** (`[42, 42, 42, 42]`) every 300ms: *"I exist, who's the server?"*
2. Every instance responds to discovery requests with: `[43, 43, 43, 43][padding(4)][timestamp(8)]` -where timestamp is `Date.now()` from when the app first opened
3. The instance with the **earliest timestamp** wins
4. After 3 seconds with no earlier challenger, you declare yourself server
5. The elected server re-broadcasts its beacon every 3 seconds so late-joiners can discover it

The server gets `instanceUID = 1` (hardcoded). Clients get a random UID in `1–65534`. This UID becomes their network address for the entire session -no DNS, no IP addresses, no NAT traversal headaches.

The whole election algorithm is about 80 lines of JavaScript. It works over any broadcast transport. No configuration needed.

---

## 3. From Lockstep to Real-Time (The Big One)

### The Simple Version

Original DOOM multiplayer works like a group of people writing a letter round-robin. Nobody can write their next line until everyone has received and read the previous line. If one person is slow, *everyone* waits.

We changed it to work more like a live conversation. Everyone talks at their own pace. If you miss a word, you can still follow along because the speaker periodically summarises where things stand. The result feels like a modern shooter -smooth movement, responsive controls, no freezing.

### The Nerdy Version

This was the most fundamental change: replacing DOOM's pure lockstep synchronisation with a **hybrid real-time model**. The ticcmd backbone remains (it's too deeply embedded in Chocolate Doom's deterministic physics to remove), but we layered three correction systems on top:

**a) Position Snapshots + Exponential Smoothing**

Every 2 tics (~57ms at 35 FPS), each player broadcasts a snapshot of their state:

```c
int data[9] = {
    mo->x, mo->y, mo->z,        // World position (fixed-point)
    mo->angle,                    // Facing direction
    mo->momx, mo->momy, mo->momz, // Velocity
    attack_weapon,                // 0=idle, 1-9=attacking with weapon N
    latency                       // Round-trip time (ms)
};
```

Remote players don't run DOOM's physics engine at all. Instead, `D_TickPlayerInterp()` runs every tic:

1. **Extrapolate** the target forward using stored momentum: `target_x += momx`
2. **Smooth** toward the target, closing 60% of the remaining gap each tic:
   ```c
   #define INTERP_FRAC  39322  // 0.6 * 65536 (fixed-point)
   mo->x += FixedMul(target_x - mo->x, INTERP_FRAC);
   mo->y += FixedMul(target_y - mo->y, INTERP_FRAC);
   ```
3. **Teleport detection**: if the delta exceeds 128 map units, snap instantly (player respawned or hit a teleporter)

One subtle but critical detail: **angle is NOT interpolated**. The ticcmd's `angleturn` field is applied deterministically by `P_MovePlayer()` and stays in sync across machines. Interpolating angle toward a stale snapshot would *fight* the ticcmd, causing visible rotation jitter of up to 90 degrees. We learned this the hard way.

**b) Attack Animation Sync**

Remote players' attacks are driven by snapshot data, not ticcmds. When a snapshot reports `attack_weapon > 0`, we set the `S_PLAY_ATK1` animation state and play the weapon fire sound. An 8-tic animation lock prevents local state transitions from overriding it.

To prevent feedback loops, we track `last_received_attack[]` -the raw flag as received from the network -and broadcast *that*, not the lock-modified mobj state. Without this, attack animations would echo infinitely.

---

## 4. "I Shot You!" "No You Didn't!"

### The Simple Version

In the original DOOM, every computer runs its own copy of the game physics. When you shoot someone, *your* computer calculates the damage, and *their* computer calculates the damage, and because the game is in lockstep, they always agree.

With our real-time model, that guarantee vanishes -your screen and theirs might show slightly different positions. So we made one player (the host) the referee. When you shoot someone, you tell the host *"I hit Player 2 for 50 damage"*. The host checks the physics, applies the damage if it's valid, and announces the result to everyone. One truth, no arguments.

### The Nerdy Version

We implemented an **event-based host-authority model** for all game-changing state:

**Damage path:**
```
Client A fires weapon → bullet hits Player B locally
  → Client A sends DAMAGE_EVENT to host: {target: B, damage: 50, source: A}
  → Client A applies VISUAL FEEDBACK ONLY (screen flash, attacker tracking)
  → Client A does NOT reduce Player B's health

Host receives DAMAGE_EVENT:
  → Sets damage_from_event = true (bypasses remote-source skip)
  → Calls P_DamageMobj() with full physics simulation
  → New health broadcasted via HEALTH_AUTH packet next snapshot cycle

All clients receive HEALTH_AUTH:
  → Apply as ground truth
  → Host says dead, client says alive? → P_KillMobj()
  → Host says alive, client says dead? → Trigger respawn
  → Health decreased? → Play pain animation
```

The `damage_from_event` flag is critical. Without it, the host faces a double-damage problem: it receives both the ticcmd attack (from the lockstep backbone) AND the explicit damage event. The flag ensures only the event path applies.

**Other host-authoritative systems:**
- **USE events**: Client presses 'E' near a door → sends `USE_EVENT` → host calls `P_UseLines()` → door opens for everyone
- **Respawns**: Client dies → sends `RESPAWN_REQUEST` → host sets `playerstate = PST_REBORN`
- **Kill messages**: Host broadcasts `KILL_MSG` when a player dies → displayed on all screens

---

## 5. Making Monsters Agree

### The Simple Version

DOOM has dozens of monsters per level, each running their own AI -chasing you, shooting fireballs, infighting with each other. In the original game, every computer simulates every monster identically (because lockstep ensures they all see the same inputs). In our version, only the host simulates the monsters. Everyone else just sees the results -like watching a puppet show where only the puppeteer knows the script, but the audience sees the performance in real-time.

### The Nerdy Version

We built a full NPC synchronisation system (`p_netsync.c/h`):

**Registry**: Every monster and barrel gets a unique `net_id` via `P_NetAssignId()`:
```c
mobj_t *net_mobj_table[MAX_NET_MOBJS];  // 512 slots
unsigned short net_id_counter;            // Sequential allocator
```

**Host broadcasts** (every 2 tics) a compact binary snapshot:
```
[npc_count: u8][gametic: u16]
Per NPC (20 bytes): net_id(2) x(4) y(4) z(4) angle(1) statenum(2) health(2) flags(1)
[sector_count: u8]
Per changed sector (10 bytes): sector_id(2) ceiling(4) floor(4)
[missile_count: u8]
Per missile (32 bytes): source_net_id(2) type(2) x(4) y(4) z(4) momx(4) momy(4) momz(4) angle(4)
```

**Client-side**: In `P_MobjThinker()`, clients skip physics entirely for any mobj with `net_id > 0`. Only the animation tic counter runs. State changes use `P_SetMobjStateNoAction()` -which applies the visual state WITHOUT executing action functions. This prevents clients from independently spawning projectiles, running AI decisions, or playing duplicate sounds.

The sector data keeps doors and lifts in sync. The missile data ensures fireballs and rockets appear on all screens.

---

## 6. "Room for One More?" -Mid-Game Joining

### The Simple Version

Original DOOM didn't let you join a game already in progress. Everyone had to be there at the start, or too bad. Our version lets players drop in mid-game. You open the .xdc, the game finds the server, and you spawn in -even if everyone else is already knee-deep in the dead.

### The Nerdy Version

Late joining required solving several hairy problems:

**Tic synchronisation**: The late joiner's tic counter is meaningless -they weren't there for tics 0 through N. When the server accepts a late join, it sends `settings->start_tic` set to the current server tic. The client aligns its `recvtic`, `maketic`, and `gametic` to this value, syncing the 8-bit tic sequence numbers with the server's counter.

**Double-slot bug**: We found that `NET_SV_AssignPlayers()` could assign the new client to a slot that was already used, creating a ghost duplicate. The fix: `HandleLateJoin()` explicitly clears any pre-existing slots for the new client before assigning a fresh one.

**Ghost body cleanup**: When a player disconnects and reconnects (or the quit/rejoin cycle fires), their old mobj lingers in the world as an invisible collision obstacle. `P_SpawnPlayer()` now checks for and removes any existing mobj before spawning:
```c
if (p->mo) {
    p->mo->player = NULL;
    P_RemoveMobj(p->mo);
    p->mo = NULL;
}
```

**Name propagation**: When a new player joins, every existing player re-broadcasts their name, so the joiner's HUD displays the correct names instead of the defaults ("Green", "Indigo", "Brown", "Red").

---

## 7. Fits in a Chat Message

### The Simple Version

The entire game -engine, levels, monsters, weapons, networking, touch controls, gamepad support, all of it -fits in a **4.2 megabyte file**. That's smaller than most photos your phone takes. You send it in a chat message like you'd send a meme. Your friend taps it, and they're in the game. No app store, no downloads, no accounts, no updates.

### The Nerdy Version

The .xdc format is just a ZIP archive with a different extension. Ours contains:

| File | Size (compressed) | Purpose |
|---|---|---|
| `vector-doom.wasm` | ~2.7 MB | Chocolate Doom engine (Emscripten, -O3) |
| `doom1.wad` | ~1.7 MB | DOOM shareware levels (freely distributable) |
| `vector-doom.js` | ~160 KB | Emscripten runtime (terser-minified, 58% reduction) |
| `index.html` | ~12 KB | UI, touch controls, gamepad support, CRT visual theme |
| `webxdc-net.js` | ~3 KB | Server election + packet routing |
| `icon.png` | ~90 KB | App icon |
| `bg.jpg` | ~100 KB | Background artwork |
| `default.cfg` | ~2 KB | Default key bindings |
| `manifest.toml` | ~100 B | WebXDC metadata |

The WASM binary and WAD compress extremely well in ZIP (65% and 58% respectively), bringing the total to 4.2MB.

The build pipeline:
1. Emscripten compiles Chocolate Doom + our modifications to WASM (`emmake make -j4`)
2. `webxdc-net.js` is injected as `--pre-js` (runs before WASM initialises)
3. JS is minified with terser in parallel
4. HTML is minified (strip comments, collapse whitespace)
5. Everything zipped at maximum compression (`zip -9`)

The game runs on any WebXDC-compatible messenger. We built it for [Vector](https://vector.im) (a Nostr-based messenger), but it works in [Delta Chat](https://delta.chat/) and potentially any app that implements the WebXDC standard.

---

## The Full Stack, From Keypress to Frag

Here's what happens when you press the fire button on your phone:

```
1. Your finger hits the Fire button (HTML touch event)
2. JavaScript calls Module._inject_key_event(0, 32)     // keydown, spacebar
3. DOOM's event queue receives ev_keydown
4. D_ProcessEvents() → G_Responder() builds ticcmd with BT_ATTACK
5. TryRunTics() executes the tic -your weapon fires, P_LineAttack() traces a hitscan
6. Bullet hits Player 2 → P_DamageMobj() → instead of applying damage:
   → NET_CL_SendDamageEvent(target=2, damage=50, source=you)
7. Every 2 tics, your position snapshot broadcasts via realtimeChannel:
   → JS encodes [to(4)][from(4)][snapshot] → WebSocket → Iroh gossip → peer devices
8. Host receives damage event → validates → applies → broadcasts HEALTH_AUTH
9. Player 2's screen: health drops, pain flash plays, kill message appears
10. Your screen: the host's HEALTH_AUTH confirms the kill
```

Total time from keypress to kill confirmation: roughly 100-200ms depending on network conditions. No servers touched. No corporation involved. Just two chat apps talking directly to each other.

---

## Standing on the Shoulders of Giants

None of this would exist without:

- **[id Software](https://www.idsoftware.com/)** for open-sourcing the DOOM engine in 1997 -a decision that created an entire modding ecosystem and directly enabled everything described here
- **[Chocolate Doom](https://www.chocolate-doom.org/)** for faithfully recreating the original engine in portable, hackable C
- **[Cloudflare](https://blog.cloudflare.com/doom-multiplayer-workers/)** for proving DOOM-in-WebAssembly was viable and open-sourcing their [doom-wasm](https://github.com/cloudflare/doom-wasm) port -the foundation we forked
- **[Emscripten](https://emscripten.org/)** for making C-to-WASM compilation actually work
- **[Iroh](https://iroh.computer/)** (by n0.computer) for the QUIC-based P2P gossip protocol that makes serverless real-time gaming possible
- **The [WebXDC](https://webxdc.org/) community** for defining an open standard for sandboxed web apps in chat messages

And a special note: the networking architecture of Open Source DOOM was designed and implemented as a collaboration between a human developer and an AI ([Claude](https://claude.ai), by Anthropic). Not generated and pasted -*collaborated on.* Hundreds of iterations, debugging sessions at 3am with hex dumps of gossip packets, heated debates about whether to interpolate angles (don't), and moments of genuine surprise when things just... worked.

---

## Try It

Open Source DOOM is free, open-source, and available today.

**Play it**: Download [Vector](https://vectorapp.io), open **Vector Nexus** (our decentralised in-app Mini App store), and find **DOOM** in the Multiplayer category. Send it to a friend or group chat and start fragging - no manual file management needed.

**Read the code**: The full source is available at [github.com/nicholasopuni31/doom-wasm](https://github.com/VectorPrivacy/DOOM).

**Build on it**: The WebXDC realtime channel pattern we developed here works for any real-time multiplayer game.

*If DOOM is "Hello, World" for computing, then Open Source DOOM is "Hello, World" for decentralised gaming.*

---

*Published March 2026. Written by JSKitty.*
