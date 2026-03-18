# VectorDoom Netcode Architecture

A comprehensive map of the Chocolate Doom networking stack as modified for
VectorDoom (WebXDC realtimeChannel transport).

---

## Table of Contents

1. [High-Level Overview](#1-high-level-overview)
2. [Layer Diagram](#2-layer-diagram)
3. [Key Data Structures](#3-key-data-structures)
4. [File-by-File Reference](#4-file-by-file-reference)
5. [The Game Loop (Execution Order)](#5-the-game-loop-execution-order)
6. [Ticcmd Flow: Keyboard to Simulation](#6-ticcmd-flow-keyboard-to-simulation)
7. [The Loopback Mechanism (Host = Server + Client)](#7-the-loopback-mechanism-host--server--client)
8. [Server Receive Window and PumpSendQueue](#8-server-receive-window-and-pumpsendqueue)
9. [Client Receive Window](#9-client-receive-window)
10. [The Diff/Patch Mechanism](#10-the-diffpatch-mechanism)
11. [Non-Blocking Server (VectorDoom Modification)](#11-non-blocking-server-vectordoom-modification)
12. [Consistency Check](#12-consistency-check)
13. [WebXDC Transport Specifics](#13-webxdc-transport-specifics)
14. [Connection Lifecycle](#14-connection-lifecycle)
15. [Disconnect Detection ("Player X left the game")](#15-disconnect-detection-player-x-left-the-game)
16. [Timeout and Keepalive Mechanisms](#16-timeout-and-keepalive-mechanisms)
17. [Resend / Retransmission Logic](#17-resend--retransmission-logic)
18. [Clock Synchronization (PID Filter)](#18-clock-synchronization-pid-filter)
19. [Packet Type Reference](#19-packet-type-reference)

---

## 1. High-Level Overview

VectorDoom uses a **client-server lockstep** architecture derived from
Chocolate Doom. All instances run the same deterministic game simulation.
The server collects per-tic input commands (`ticcmd_t`) from every player,
assembles them into a `net_full_ticcmd_t` containing all players' inputs for
that tic, and distributes them to all clients. Every client then executes the
same sequence of tics with the same inputs, producing identical game states.

**VectorDoom's key modification**: the transport layer (`net_websockets.c`)
was replaced with `net_webxdc.c`, which routes all packets through
`webxdc.joinRealtimeChannel()` -- a peer-to-peer broadcast channel available
inside WebXDC sandboxed apps. A JavaScript pre-loaded layer (`webxdc-net.js`)
handles server election and packet routing.

The **host** instance runs both the server and a client simultaneously,
connected via an in-process loopback (`net_loop.c`). Remote clients connect
to the server over the WebXDC transport.

---

## 2. Layer Diagram

```
+------------------------------------------------------------------+
|                          GAME LAYER                              |
|  g_game.c          G_Ticker, G_BuildTiccmd, consistency check    |
|  doom/d_net.c      RunTic, PlayerQuitGame, loop_interface        |
+------------------------------------------------------------------+
|                         LOOP LAYER                               |
|  d_loop.c          TryRunTics, NetUpdate, BuildNewTic,           |
|                    D_ReceiveTic, D_InitNetGame, D_StartNetGame    |
+------------------------------------------------------------------+
|                       CLIENT / SERVER                             |
|  net_client.c      Client state machine, send/recv windows,      |
|                    clock sync, ticcmd sending                     |
|  net_server.c      Server state machine, recv window,            |
|                    PumpSendQueue, player management               |
+------------------------------------------------------------------+
|                      CONNECTION LAYER                             |
|  net_common.c      Connection state, keepalive, reliable packets, |
|                    disconnect handshake, timeout detection         |
+------------------------------------------------------------------+
|                      SERIALIZATION                                |
|  net_structrw.c    Diff/Patch, Read/Write ticcmds, settings,     |
|                    full ticcmds, wait data, protocol negotiation   |
+------------------------------------------------------------------+
|                        I/O LAYER                                  |
|  net_io.c          Context management, module dispatch,           |
|                    SendPacket, RecvPacket, address refcounting     |
|  net_packet.c      Packet buffer allocation, read/write integers, |
|                    strings                                        |
+------------------------------------------------------------------+
|                     TRANSPORT MODULES                             |
|  net_loop.c        In-process loopback (host server <-> client)   |
|  net_webxdc.c      WebXDC realtimeChannel transport (C side)      |
|  webxdc-net.js     JS layer: channel setup, server election,      |
|                    packet routing/filtering                        |
+------------------------------------------------------------------+
```

---

## 3. Key Data Structures

### `ticcmd_t` (d_ticcmd.h)

The fundamental unit of player input for one game tic:

```c
typedef struct {
    signed char forwardmove;   // forward/backward (-127..127)
    signed char sidemove;      // strafe left/right (-127..127)
    short       angleturn;     // turning (<<16 for angle delta)
    byte        chatchar;      // chat character
    byte        buttons;       // fire, use, weapon switch
    byte        consistancy;   // game state hash for sync check
    byte        buttons2;      // (Strife)
    int         inventory;     // (Strife)
    byte        lookfly;       // (Heretic/Hexen)
    byte        arti;          // (Heretic/Hexen)
} ticcmd_t;
```

### `net_ticdiff_t` (net_defs.h)

A delta-encoded ticcmd. Only changed fields are transmitted:

```c
typedef struct {
    unsigned int diff;   // bitmask of which fields changed
    ticcmd_t cmd;        // the new values for changed fields
} net_ticdiff_t;
```

Diff flags: `NET_TICDIFF_FORWARD`, `NET_TICDIFF_SIDE`, `NET_TICDIFF_TURN`,
`NET_TICDIFF_BUTTONS`, `NET_TICDIFF_CONSISTANCY`, `NET_TICDIFF_CHATCHAR`,
`NET_TICDIFF_RAVEN`, `NET_TICDIFF_STRIFE`.

### `net_full_ticcmd_t` (net_defs.h)

A complete set of all players' inputs for one tic, sent from server to client:

```c
typedef struct {
    signed int  latency;                       // worst-case latency
    unsigned int seq;                          // tic sequence number
    boolean     playeringame[NET_MAXPLAYERS];  // who is playing
    net_ticdiff_t cmds[NET_MAXPLAYERS];        // per-player diffs
} net_full_ticcmd_t;
```

### `ticcmd_set_t` (d_loop.c, local)

The game loop's local storage for a tic's commands:

```c
typedef struct {
    ticcmd_t cmds[NET_MAXPLAYERS];
    boolean  ingame[NET_MAXPLAYERS];
} ticcmd_set_t;
```

Stored in circular buffer: `ticcmd_set_t ticdata[BACKUPTICS]` (BACKUPTICS=128).

### `net_packet_t` (net_defs.h)

A network packet buffer:

```c
struct _net_packet_s {
    byte *data;
    size_t len;       // current data length
    size_t alloced;   // allocated size
    unsigned int pos; // read cursor
};
```

### `net_addr_t` (net_defs.h)

A network address with module dispatch:

```c
struct _net_addr_s {
    net_module_t *module;  // which transport module owns this
    int refcount;          // reference counting
    void *handle;          // module-specific data (e.g., uint32_t* UID)
};
```

### `net_connection_t` (net_common.h)

Connection state tracked per peer:

```c
typedef struct {
    net_connstate_t state;
    net_disconnect_reason_t disconnect_reason;
    net_addr_t *addr;
    net_protocol_t protocol;
    int last_send_time;
    int num_retries;
    int keepalive_send_time;
    int keepalive_recv_time;
    net_reliable_packet_t *reliable_packets;
    int reliable_send_seq;
    int reliable_recv_seq;
} net_connection_t;
```

States: `CONNECTING` -> `CONNECTED` -> `DISCONNECTING` -> `DISCONNECTED`
(also `DISCONNECTED_SLEEP` for lingering ACK window).

### `net_client_t` (net_server.c, local)

Server's per-client state:

```c
typedef struct {
    boolean active;
    int player_number;
    net_addr_t *addr;
    net_connection_t connection;
    int last_send_time;
    char *name;
    boolean ready;
    unsigned int connect_time;
    int last_gamedata_time;
    boolean recording_lowres;
    int sendseq;
    net_full_ticcmd_t sendqueue[BACKUPTICS];
    unsigned int acknowledged;
    int max_players;
    boolean drone;
    sha1_digest_t wad_sha1sum, deh_sha1sum;
    unsigned int is_freedoom;
    int player_class;
} net_client_t;
```

### Server Receive Window (net_server.c)

```c
typedef struct {
    boolean active;
    signed int latency;
    unsigned int resend_time;
    net_ticdiff_t diff;
} net_client_recv_t;

static net_client_recv_t recvwindow[BACKUPTICS][NET_MAXPLAYERS];
static unsigned int recvwindow_start;  // first tic in the window
```

Two-dimensional: `recvwindow[tic_offset][player]`.

### Client Receive Window (net_client.c)

```c
typedef struct {
    boolean active;
    unsigned int resend_time;
    net_full_ticcmd_t cmd;
} net_server_recv_t;

static net_server_recv_t recvwindow[BACKUPTICS];
static int recvwindow_start;
static ticcmd_t recvwindow_cmd_base[NET_MAXPLAYERS];  // diff baseline
```

One-dimensional: `recvwindow[tic_offset]`, contains full ticcmds for all
players.

---

## 4. File-by-File Reference

### `d_loop.c` -- The Game Loop

**Purpose**: The central game loop that ties together input generation,
networking, and game simulation.

**Key globals**:
- `gametic` -- the tic about to be (or currently being) simulated
- `maketic` -- the next tic to have input generated for
- `recvtic` -- the latest tic fully received from the server
- `ticdata[BACKUPTICS]` -- circular buffer of `ticcmd_set_t`
- `localplayer` -- this instance's player index
- `instanceUID` -- WebXDC instance identifier

**Key functions**:

| Function | Role |
|----------|------|
| `NetUpdate()` | Called every frame. Runs `NET_CL_Run()` + `NET_SV_Run()`, then builds new ticcmds based on elapsed time |
| `BuildNewTic()` | Reads input, calls `BuildTiccmd()`, sends to server via `NET_CL_SendTiccmd()`, stores locally |
| `D_ReceiveTic()` | Callback from `net_client.c`. Stores received ticcmds into `ticdata[]`, increments `recvtic` |
| `TryRunTics()` | Main entry point per frame. Calls `NetUpdate()`, waits for enough tics, then runs them via `loop_interface->RunTic()` |
| `GetLowTic()` | Returns `min(maketic, recvtic)` -- the highest tic we can safely simulate |
| `D_InitNetGame()` | Sets up networking: server gets `instanceUID=1`, clients get random UID. Connects to server |
| `D_StartNetGame()` | Sends game settings, blocks until server starts the game |
| `D_QuitNetGame()` | Clean shutdown of server and client |

### `d_loop.h` -- Loop Interface

**Purpose**: Declares the `loop_interface_t` callback structure and public API.

```c
typedef struct {
    void (*ProcessEvents)();
    void (*BuildTiccmd)(ticcmd_t *cmd, int maketic);
    void (*RunTic)(ticcmd_t *cmds, boolean *ingame);
    void (*RunMenu)();
} loop_interface_t;
```

### `net_server.c` -- Server-Side Netcode

**Purpose**: Manages all connected clients, collects their ticcmds,
assembles complete tic data, and distributes it.

**Server states**: `SERVER_WAITING_LAUNCH` -> `SERVER_WAITING_START` ->
`SERVER_IN_GAME`

**Key functions**:

| Function | Role |
|----------|------|
| `NET_SV_Init()` | Creates server context, initializes client slots |
| `NET_SV_Run()` | Main server tick: receive packets, run each client, advance window, check resends |
| `NET_SV_Packet()` | Dispatches incoming packets by type (SYN, GAMEDATA, ACK, etc.) |
| `NET_SV_ParseSYN()` | Handles connection requests: validates magic, protocol, game mode. Allocates client slot |
| `NET_SV_ParseGameData()` | Receives ticcmd diffs from clients, stores in `recvwindow[][]`, tracks `sv_last_ticcmd[]` |
| `NET_SV_PumpSendQueue()` | **Core function**: For each client, assembles `net_full_ticcmd_t` from all other players' data, enqueues and sends |
| `NET_SV_AdvanceWindow()` | Slides `recvwindow_start` forward based on lowest acknowledged tic |
| `NET_SV_RunClient()` | Runs connection state machine, calls PumpSendQueue and CheckDeadlock |
| `NET_SV_CheckDeadlock()` | If no data from a client for 1s, sends resend request to break deadlock |
| `NET_SV_SendResendRequest()` | Asks a client to retransmit specific tics |
| `NET_SV_CheckResends()` | Re-sends resend requests that timed out (300ms) |
| `NET_SV_SendTics()` | Serializes and sends a range of tics from a client's sendqueue |
| `NET_SV_ParseGameDataACK()` | Updates client's acknowledged tic number |
| `StartGame()` | Sends GAMESTART to all clients, transitions to IN_GAME |
| `NET_SV_GameEnded()` | Resets to WAITING_LAUNCH when all players disconnect |

**Non-blocking modification** (VectorDoom-specific):
- `sv_last_ticcmd[NET_MAXPLAYERS]` -- reconstructed full ticcmd per player
- `sv_player_has_sent[NET_MAXPLAYERS]` -- tracks if player has ever sent data
- When `PumpSendQueue()` encounters a missing tic, it fabricates from
  `sv_last_ticcmd[]` with all diff flags set, rather than blocking

### `net_client.c` -- Client-Side Netcode

**Purpose**: Manages the connection to the server, sends local ticcmds,
receives and buffers complete tic data from the server.

**Client states**: `CLIENT_STATE_WAITING_LAUNCH` ->
`CLIENT_STATE_WAITING_START` -> `CLIENT_STATE_IN_GAME`

**Key functions**:

| Function | Role |
|----------|------|
| `NET_CL_Run()` | Main client tick: receive packets, run connection, advance receive window, check resends |
| `NET_CL_Connect()` | Sends SYN packets every 1s, waits up to 120s for connection |
| `NET_CL_SendTiccmd()` | Diffs current ticcmd against `last_ticcmd`, stores in `send_queue[]`, transmits |
| `NET_CL_SendTics()` | Serializes and sends a range of ticcmd diffs to the server |
| `NET_CL_ParseGameData()` | Receives `net_full_ticcmd_t` from server, stores in `recvwindow[]`, triggers clock sync |
| `NET_CL_AdvanceWindow()` | Pops tics from front of recvwindow, calls `ExpandFullTiccmd()` then `D_ReceiveTic()` |
| `NET_CL_ExpandFullTiccmd()` | Applies diff patches against `recvwindow_cmd_base[]` to reconstruct full ticcmds |
| `NET_CL_CheckResends()` | Sends resend requests for timed-out tics (300ms); deadlock detection at 1000ms |
| `NET_CL_SendGameDataACK()` | Sends current `recvwindow_start` as acknowledgment to server |
| `NET_CL_Disconnect()` | Sends DISCONNECT, waits up to 5s for ACK |
| `UpdateClockSync()` | PID controller adjusting `offsetms` to synchronize client clocks |
| `NET_CL_ParseSYN()` | Handles SYN response, sets state to CONNECTED |

**Client send window**: `send_queue[BACKUPTICS]` of `net_server_send_t`
(stores seq, timestamp, ticdiff for each sent tic).

### `net_defs.h` -- Network Type Definitions

**Purpose**: Central header defining all network types and constants.

Key constants:
- `MAXNETNODES 16` -- max connected nodes (including observers)
- `NET_MAXPLAYERS 8` -- max actual players
- `BACKUPTICS 128` -- circular buffer size for tic data
- `NET_MAGIC_NUMBER 1454104972U` -- connection handshake magic
- `NET_RELIABLE_PACKET (1 << 15)` -- flag in packet type for reliable delivery

Defines all packet types (`NET_PACKET_TYPE_SYN`, `_GAMEDATA`, `_GAMEDATA_ACK`,
`_DISCONNECT`, etc.), game settings structures, and the `net_module_t` interface.

### `net_structrw.c` / `net_structrw.h` -- Serialization

**Purpose**: Reads and writes all network structures to/from packet buffers.

**Key functions**:

| Function | Role |
|----------|------|
| `NET_TiccmdDiff()` | Computes diff between two ticcmds, producing `net_ticdiff_t` |
| `NET_TiccmdPatch()` | Applies a `net_ticdiff_t` to a baseline `ticcmd_t` to reconstruct the new one |
| `NET_WriteTiccmdDiff()` / `NET_ReadTiccmdDiff()` | Serialize/deserialize a single ticcmd diff |
| `NET_WriteFullTiccmd()` / `NET_ReadFullTiccmd()` | Serialize/deserialize a complete tic (latency + playeringame bitfield + per-player diffs) |
| `NET_WriteSettings()` / `NET_ReadSettings()` | Serialize/deserialize game settings |
| `NET_WriteConnectData()` / `NET_ReadConnectData()` | Serialize/deserialize connection data |
| `NET_WriteProtocolList()` / `NET_ReadProtocolList()` | Protocol negotiation |

### `net_common.c` / `net_common.h` -- Connection Management

**Purpose**: Shared connection state machine used by both client and server
connections.

**Key constants**:
- `CONNECTION_TIMEOUT_LEN 4` (seconds) -- modified from original 30s for WebXDC
- `KEEPALIVE_PERIOD 2` (seconds)
- `MAX_RETRIES 5` -- disconnect retry limit

**Key functions**:

| Function | Role |
|----------|------|
| `NET_Conn_InitClient()` | Initializes connection in CONNECTING state |
| `NET_Conn_InitServer()` | Initializes connection in CONNECTED state |
| `NET_Conn_Run()` | Runs the connection state machine: timeout detection, keepalive sending, reliable packet retransmit |
| `NET_Conn_Packet()` | Processes common packet types (DISCONNECT, DISCONNECT_ACK, KEEPALIVE, RELIABLE_ACK). Returns true if consumed |
| `NET_Conn_Disconnect()` | Transitions to DISCONNECTING state |
| `NET_Conn_NewReliable()` | Creates a reliable packet (auto-retransmitted until ACKed) |
| `NET_Conn_SendPacket()` | Sends packet and updates keepalive_send_time |
| `NET_ExpandTicNum()` | Expands a 1-byte tic number to full 32-bit using relative window position |

**Reliable packet mechanism**: Reliable packets get a sequence number and
are stored in a linked list (`reliable_packets`). They are retransmitted
every 1 second until the other side sends a `RELIABLE_ACK` with the next
expected sequence number. Used for SYN, LAUNCH, GAMESTART, CONSOLE_MESSAGE,
DISCONNECT.

### `net_loop.c` -- Loopback Transport

**Purpose**: In-process loopback so the host can be both server and client.
Two packet queues connect the two sides without any real networking.

**Architecture**: Two `net_module_t` implementations:
- `net_loop_client_module` -- used by the client side
- `net_loop_server_module` -- used by the server side

When the client sends a packet, it is duplicated and placed in `server_queue`.
When the server sends a packet, it is duplicated and placed in `client_queue`.
Each side pops from its own queue when receiving.

Queue size: 16 packets (circular buffer). Packets are deep-copied
(`NET_PacketDup`) to avoid aliasing.

### `net_io.c` -- Network I/O Abstraction

**Purpose**: Manages `net_context_t` objects that hold a list of transport
modules. Provides unified Send/Recv that dispatches through modules.

**Key functions**:
- `NET_NewContext()` -- allocates a context
- `NET_AddModule()` -- registers a transport module
- `NET_SendPacket()` -- dispatches to `addr->module->SendPacket()`
- `NET_RecvPacket()` -- polls all modules in order, returns first available packet
- `NET_ReferenceAddress()` / `NET_ReleaseAddress()` -- reference counting for `net_addr_t`

**Server context**: has both `net_loop_server_module` and `net_webxdc_module`.
**Client context (host)**: has only `net_loop_client_module`.
**Client context (remote)**: has only `net_webxdc_module`.

### `net_packet.c` / `net_packet.h` -- Packet Buffer Management

**Purpose**: Low-level packet allocation and read/write primitives.

- `NET_NewPacket()` -- allocates packet with initial size
- `NET_PacketDup()` -- deep copy
- `NET_FreePacket()` -- free
- `NET_ReadInt8/16/32()`, `NET_ReadSInt8/16/32()` -- read with bounds check
- `NET_WriteInt8/16/32()` -- write with auto-grow
- `NET_ReadString()` / `NET_WriteString()` -- NUL-terminated strings
- Big-endian wire format for multi-byte integers

### `net_webxdc.c` -- WebXDC Transport Layer (C Side)

**Purpose**: Implements `net_module_t` for the WebXDC realtimeChannel,
replacing the original WebSocket transport.

**Wire format**: `[to(4 bytes LE)][from(4 bytes LE)][doom_payload]`

**Key functions**:

| Function | Role |
|----------|------|
| `InitWebXDC()` | Checks JS channel is ready, registers instanceUID |
| `NET_WebXDC_SendPacket()` | Prepends to/from headers, calls `js_webxdc_send()` |
| `NET_WebXDC_RecvPacket()` | Calls `PollReceivedPackets()`, pops from C queue |
| `PollReceivedPackets()` | Calls `js_webxdc_recv()` in a loop, extracts from-address, creates `net_packet_t` |
| `FindAddressByIp()` | Maintains a static table of `net_addr_t` keyed by uint32 UID |

**EM_JS bindings**:
- `js_webxdc_send(data, len)` -- copies from WASM heap, sends via `globalThis._webxdcChannel.send()`
- `js_webxdc_recv(buf, maxlen)` -- shifts from `globalThis._webxdcRecvQueue`, copies to WASM heap

Address table is static (64 slots), never freed. Each UID maps to a
`net_addr_t` whose `handle` points to a `uint32_t` in the `ips[]` array.

### `webxdc-net.js` -- JavaScript Networking Layer

**Purpose**: Pre-loaded before WASM. Sets up the realtimeChannel, handles
server election, and filters incoming packets.

**Server election protocol**:
1. On startup, broadcast `[42, 42, 42, 42]` every 300ms
2. Any existing server responds with `[43, 43, 43, 43][unused(4)][timestamp(8)]`
3. If response timestamp < own timestamp, they are server, we are client
4. If no response within 3 seconds, we become the server
5. Server continues broadcasting `[42, 42, 42, 42]` every 3s for late joiners

**Packet routing**:
- All game packets are broadcast to all peers via realtimeChannel
- Format: `[to(4)][from(4)][doom_payload]`
- JS reads the `to` field (LE uint32) and compares to `globalThis._doomInstanceUID`
- Only packets addressed to this instance (or to address 0) are delivered
- Delivered to C as `[from(4)][doom_payload]` (to field stripped)

**Fallback**: If `webxdc` global is absent, uses `BroadcastChannel('vectordoom-net')`
for local testing.

**Globals exposed**:
- `_webxdcRecvQueue` -- array of Uint8Array, polled by C
- `_doomInstanceUID` -- set by C when ready
- `_webxdcChannel` -- the realtimeChannel object
- `_serverElectionP` -- Promise<boolean>

### `doom/d_net.c` -- Game-Level Network Interface

**Purpose**: Doom-specific glue between the generic loop layer and the game.

**Key functions**:

| Function | Role |
|----------|------|
| `RunTic()` | Called by d_loop.c for each tic. Checks for player quits, sets `netcmds`, calls `G_Ticker()` |
| `PlayerQuitGame()` | Sets `playeringame[i] = false`, displays "Player N left the game" |
| `D_ConnectNetGame()` | Initializes connect data, calls `D_InitNetGame()` |
| `D_CheckNetGame()` | Registers loop callbacks, starts net game, loads settings |
| `LoadGameSettings()` | Copies `net_gamesettings_t` into global game variables |
| `SaveGameSettings()` | Copies global game variables into `net_gamesettings_t` |

**Loop interface registration**:
```c
static loop_interface_t doom_loop_interface = {
    D_ProcessEvents,  // ProcessEvents
    G_BuildTiccmd,    // BuildTiccmd
    RunTic,           // RunTic
    M_Ticker          // RunMenu
};
```

### `doom/g_game.c` -- Game Ticker and Consistency

**Purpose**: The actual game simulation tick, input building, and
consistency checking.

**Key functions**:

| Function | Role |
|----------|------|
| `G_BuildTiccmd()` | Reads keyboard/mouse/joy state, fills `ticcmd_t`. Sets `cmd->consistancy` from local table |
| `G_Ticker()` | Main game tick: handles reborns, game actions, copies netcmds to player structs, runs consistency check, then advances game state |

**Consistency array**: `byte consistancy[MAXPLAYERS][BACKUPTICS]`

---

## 5. The Game Loop (Execution Order)

Each frame, the Emscripten main loop callback runs `TryRunTics()`:

```
TryRunTics()
  |
  +-- if singletics: BuildNewTic()
  |   else: NetUpdate()
  |           |
  |           +-- NET_CL_Run()         // Client: recv packets, advance window
  |           |     +-- NET_RecvPacket()  // poll transport modules
  |           |     +-- NET_CL_ParsePacket()
  |           |     |     +-- NET_CL_ParseGameData()  // store in recvwindow
  |           |     +-- NET_Conn_Run()  // keepalive, reliable retransmit
  |           |     +-- NET_CL_AdvanceWindow()
  |           |     |     +-- NET_CL_ExpandFullTiccmd()  // diff -> full ticcmd
  |           |     |     +-- D_ReceiveTic()  // -> ticdata[], recvtic++
  |           |     +-- NET_CL_CheckResends()  // retransmit expired requests
  |           |
  |           +-- NET_SV_Run()         // Server: recv packets, run clients
  |           |     +-- NET_RecvPacket()  // poll server context modules
  |           |     +-- NET_SV_Packet()   // dispatch by type
  |           |     +-- for each client: NET_SV_RunClient()
  |           |     |     +-- NET_Conn_Run()  // keepalive, reliable retransmit
  |           |     |     +-- NET_SV_PumpSendQueue()  // assemble & send tics
  |           |     |     +-- NET_SV_CheckDeadlock()
  |           |     +-- NET_SV_AdvanceWindow()  // slide recvwindow_start
  |           |     +-- NET_SV_CheckResends()   // for each player
  |           |
  |           +-- Build new ticcmds based on elapsed time
  |                 for each new tic:
  |                   BuildNewTic()
  |                     +-- I_StartTic()              // pump OS events
  |                     +-- ProcessEvents()           // D_ProcessEvents
  |                     +-- RunMenu()                 // M_Ticker
  |                     +-- BuildTiccmd(&cmd)         // G_BuildTiccmd
  |                     +-- NET_CL_SendTiccmd(&cmd)   // send to server
  |                     +-- store in ticdata[maketic]  // local copy
  |                     +-- maketic++
  |
  +-- GetLowTic()  -> lowtic = min(maketic, recvtic)
  |
  +-- Wait loop: while lowtic < gametic/ticdup + counts
  |     NetUpdate(), sleep 1ms, re-check
  |     Bail after MAX_NETGAME_STALL_TICS (10 tics)
  |
  +-- Run tics: while counts--
        set = ticdata[(gametic/ticdup) % BACKUPTICS]
        for i in 0..ticdup:
          copy ingame to local_playeringame
          loop_interface->RunTic(set->cmds, set->ingame)
            -> d_net.c RunTic()
              -> check player quits (ingame transitions false->true)
              -> set netcmds = cmds
              -> G_Ticker()
          gametic++
        NetUpdate()  // check for new commands after each tic group
```

---

## 6. Ticcmd Flow: Keyboard to Simulation

### Step 1: Input Generation (local player)

```
BuildNewTic() in d_loop.c
  -> G_BuildTiccmd(&cmd, maketic) in g_game.c
     - Reads keyboard, mouse, joystick state
     - Fills forwardmove, sidemove, angleturn, buttons
     - Sets cmd->consistancy = consistancy[consoleplayer][maketic % BACKUPTICS]
```

### Step 2: Send to Server

```
BuildNewTic() continues:
  -> NET_CL_SendTiccmd(&cmd, maketic) in net_client.c
     - NET_TiccmdDiff(&last_ticcmd, &cmd, &diff)  // delta encode
     - Store in send_queue[maketic % BACKUPTICS]
     - last_ticcmd = cmd
     - NET_CL_SendTics(maketic - extratics, maketic)
       - Writes: [GAMEDATA type][recvwindow_start & 0xff][start & 0xff][count]
       - For each tic: [latency(16)][ticdiff...]
       - NET_Conn_SendPacket() -> transport module
```

### Step 3: Server Receives

```
NET_SV_Run() -> NET_SV_Packet() -> NET_SV_ParseGameData()
  - Reads: ackseq, seq, num_tics
  - Expands 8-bit seq to full via NET_SV_ExpandTicNum()
  - For each tic in packet:
    - NET_ReadSInt16() -> latency
    - NET_ReadTiccmdDiff() -> diff
    - Store in recvwindow[index][player]
    - NET_TiccmdPatch(&sv_last_ticcmd[player], &diff, &sv_last_ticcmd[player])
    - sv_player_has_sent[player] = true
  - Update client->acknowledged from ackseq
  - If gaps detected before seq, send resend request
```

### Step 4: Server Assembles and Distributes

```
NET_SV_RunClient() -> NET_SV_PumpSendQueue(client)
  - For the client's current sendseq:
    - recv_index = sendseq - recvwindow_start
    - For each OTHER player i:
      - If recvwindow[recv_index][i].active: use real diff
      - Else if recvwindow[recv_index-1][i].active: use off-by-one (loopback fix)
      - Else: fabricate from sv_last_ticcmd[i] with all diff flags
    - Assemble net_full_ticcmd_t with seq, latency, playeringame[], cmds[]
    - Store in client->sendqueue[sendseq % BACKUPTICS]
    - NET_SV_SendTics(starttic, endtic)
      - Writes: [GAMEDATA type][start & 0xff][count]
      - For each tic: NET_WriteFullTiccmd()
        - [latency(16)][playeringame bitfield(8)]
        - For each active player: [ticdiff...]
    - sendseq++
```

### Step 5: Client Receives from Server

```
NET_CL_Run() -> NET_CL_ParseGameData()
  - Reads: seq, num_tics
  - Expands seq to full tic number
  - For each tic: NET_ReadFullTiccmd() -> store in recvwindow[index]
  - UpdateClockSync() for last tic in packet
  - If gaps before seq, send resend request
```

### Step 6: Client Advances Window

```
NET_CL_Run() -> NET_CL_AdvanceWindow()
  - While recvwindow[0].active:
    - NET_CL_ExpandFullTiccmd(&recvwindow[0].cmd, recvwindow_start, ticcmds)
      - For each player i (except self):
        - NET_TiccmdPatch(&recvwindow_cmd_base[i], &diff, &ticcmds[i])
        - recvwindow_cmd_base[i] = ticcmds[i]
    - D_ReceiveTic(ticcmds, playeringame)
      -> d_loop.c: stores into ticdata[recvtic % BACKUPTICS], recvtic++
    - memmove window forward, recvwindow_start++
```

### Step 7: Game Simulation

```
TryRunTics() -> loop_interface->RunTic(set->cmds, set->ingame)
  -> d_net.c RunTic()
    - Check for player quits (ingame changed from true to false)
    - netcmds = cmds (global pointer)
    - G_Ticker()
      - For each player: memcpy(player.cmd, &netcmds[i])
      - Consistency check (see section 12)
      - Run game logic (P_Ticker -> thinkers, physics, etc.)
```

---

## 7. The Loopback Mechanism (Host = Server + Client)

When a player starts as the server (`-server` flag):

```
D_InitNetGame():
  instanceUID = 1
  NET_SV_Init()                        // create server context
  NET_SV_AddModule(&net_loop_server_module)  // loopback server endpoint
  NET_SV_AddModule(&net_webxdc_module)       // WebXDC for remote clients
  net_loop_client_module.InitClient()        // loopback client endpoint
  addr = net_loop_client_module.ResolveAddress(NULL)  // -> &client_addr
  NET_CL_Connect(addr, ...)            // connect client to server via loopback
```

The server context receives packets from two sources:
1. `net_loop_server_module` -- packets from the local client
2. `net_webxdc_module` -- packets from remote clients

The local client context only receives from `net_loop_client_module`.

**Data flow for loopback**:
```
Client SendPacket() -> QueuePush(server_queue, dup)
Server RecvPacket() -> QueuePop(server_queue)
Server SendPacket() -> QueuePush(client_queue, dup)
Client RecvPacket() -> QueuePop(client_queue)
```

Both `NET_CL_Run()` and `NET_SV_Run()` are called every frame in
`NetUpdate()`, so the loopback host processes both sides each frame.

---

## 8. Server Receive Window and PumpSendQueue

### Receive Window Structure

```
recvwindow[BACKUPTICS][NET_MAXPLAYERS]
          ^                    ^
          |                    |
     tic offset from       player index
     recvwindow_start
```

`recvwindow_start` is the sequence number of the first slot. Slot `i`
corresponds to tic `recvwindow_start + i`.

### How Data Enters the Window

`NET_SV_ParseGameData()`:
- Receives ticcmd diffs from a single client
- For each tic in the packet, computes `index = seq + i - recvwindow_start`
- If in range [0, BACKUPTICS), sets `recvwindow[index][player].active = true`
  and stores the diff
- Also patches `sv_last_ticcmd[player]` to maintain the reconstructed
  full ticcmd for that player

### PumpSendQueue Assembly

`NET_SV_PumpSendQueue(client)` runs for each client every server tick:

1. **Throttle check**: If `sendseq - LatestAcknowledged() > 40`, skip (backpressure)
2. **Compute recv_index**: `sendseq - recvwindow_start`
3. **For each other player**: Check if their tic data is available:
   - `recvwindow[recv_index][i].active` -- ideal case, use real diff
   - `recvwindow[recv_index - 1][i].active` -- off-by-one fix for loopback timing
   - Neither -- **fabricate** from `sv_last_ticcmd[i]` (non-blocking)
4. **Build `net_full_ticcmd_t`**: set seq, latency, playeringame, cmds
5. **Store** in `client->sendqueue[sendseq % BACKUPTICS]`
6. **Send** tics from `sendseq - extratics` to `sendseq` (redundancy)
7. **Increment** `client->sendseq`

### AdvanceWindow

`NET_SV_AdvanceWindow()`:
- Computes `lowtic = NET_SV_LatestAcknowledged()` (minimum across all clients)
- While `recvwindow_start < lowtic`:
  - `memmove(recvwindow, recvwindow + 1, ...)` -- shift window left
  - Clear last slot
  - `recvwindow_start++`

This discards old tic data that all clients have acknowledged receiving.

---

## 9. Client Receive Window

### Structure

```
recvwindow[BACKUPTICS] of net_server_recv_t
recvwindow_start = first tic in window
recvwindow_cmd_base[NET_MAXPLAYERS] = diff baselines
```

### How Data Enters

`NET_CL_ParseGameData()`:
- Receives complete tics (all players' diffs) from server
- For each tic, computes `index = seq - recvwindow_start + i`
- Stores `net_full_ticcmd_t` in `recvwindow[index]`

### AdvanceWindow

`NET_CL_AdvanceWindow()`:
- While `recvwindow[0].active`:
  - `NET_CL_ExpandFullTiccmd()`: for each player (except self), applies diff
    against `recvwindow_cmd_base[i]` to get full `ticcmd_t`
  - `D_ReceiveTic(ticcmds, playeringame)` -- delivers to game loop
  - Shift window: `memmove`, clear last, `recvwindow_start++`

**Important**: The local player's ticcmd is NOT overwritten by the server data.
In `D_ReceiveTic()`, `if (!drone && i == localplayer)` skips the copy. This
prevents round-trip latency from affecting local input -- the local copy was
already stored directly in `ticdata[]` by `BuildNewTic()`.

---

## 10. The Diff/Patch Mechanism

Ticcmds are delta-encoded to save bandwidth. Only fields that changed from
the previous tic are transmitted.

### Creating a Diff

`NET_TiccmdDiff(ticcmd_t *old, ticcmd_t *new, net_ticdiff_t *diff)`:
- Compares each field of old vs new
- Sets bits in `diff->diff` for changed fields
- Copies the full new ticcmd into `diff->cmd`

### Applying a Diff (Patch)

`NET_TiccmdPatch(ticcmd_t *base, net_ticdiff_t *diff, ticcmd_t *result)`:
- Starts with a copy of `base`
- For each set bit in `diff->diff`, overwrites corresponding field from `diff->cmd`
- Fields without the bit set retain their value from `base`
- Special: `chatchar` and `arti` and `inventory` are reset to 0 if their
  flag is not set (they are event-like, not stateful)

### Serialization

`NET_WriteTiccmdDiff()`: Writes the diff byte, then only the fields whose
bits are set. Angleturn is 2 bytes normally, 1 byte if `lowres_turn`.

`NET_ReadTiccmdDiff()`: Reads the diff byte, then conditionally reads each
field.

### Chain of Baselines

**Client -> Server**: Each client maintains `last_ticcmd` as the baseline.
Each sent diff is relative to the previously sent ticcmd.

**Server side**: The server patches diffs onto `sv_last_ticcmd[player]` to
maintain the running full state. The recvwindow stores the raw diffs.

**Server -> Client**: The server sends diffs in `net_full_ticcmd_t`. Each
client maintains `recvwindow_cmd_base[player]` as the baseline for each
remote player. `NET_CL_ExpandFullTiccmd()` patches each received diff onto
this baseline.

---

## 11. Non-Blocking Server (VectorDoom Modification)

Original Chocolate Doom's `PumpSendQueue()` would block (return early) if
any player's tic data was missing. This caused the game to freeze when any
single player had packet loss.

VectorDoom modifications:

### 1. `sv_last_ticcmd[NET_MAXPLAYERS]` and `sv_player_has_sent[NET_MAXPLAYERS]`

Tracks the last fully-reconstructed ticcmd for each player. Updated in
`NET_SV_ParseGameData()` via `NET_TiccmdPatch()`.

### 2. Non-blocking PumpSendQueue

In the player-scanning loop:
```c
if (recvwindow[recv_index][i].active) {
    // Use real data
} else if (recv_index > 0 && recvwindow[recv_index - 1][i].active) {
    // Loopback off-by-one fix: use data from one position back
} else {
    // Fabricate: use sv_last_ticcmd[i] with ALL diff flags set
    if (sv_player_has_sent[i]) {
        cmd.cmds[i].diff = NET_TICDIFF_FORWARD | NET_TICDIFF_SIDE
                         | NET_TICDIFF_TURN | NET_TICDIFF_BUTTONS
                         | NET_TICDIFF_CONSISTANCY;
        cmd.cmds[i].cmd = sv_last_ticcmd[i];
    } else {
        memset(&cmd.cmds[i], 0, sizeof(net_ticdiff_t));
    }
}
```

The fabricated diff has ALL relevant flags set so the receiver gets absolute
values. This prevents baseline drift on the client side (since the client
applies patches sequentially, a fabricated diff with full absolute values
resets the baseline correctly).

### 3. Non-blocking AdvanceWindow

```c
while (recvwindow_start < lowtic) {
    // Advance unconditionally -- PumpSendQueue handles missing data
    memmove(...);
    recvwindow_start++;
}
```

No check for "all players have data" before advancing -- the window advances
purely based on client acknowledgments.

---

## 12. Consistency Check

The consistency check detects desynchronization between clients.

### How It Works

**Building**: In `G_BuildTiccmd()`:
```c
cmd->consistancy = consistancy[consoleplayer][maketic % BACKUPTICS];
```

**Checking**: In `G_Ticker()`:
```c
if (netgame && !netdemo && !(gametic % ticdup)) {
    if (gametic > BACKUPTICS && consistancy[i][buf] != cmd->consistancy) {
        // Consistency failure! (currently just prints warning)
    }
    if (players[i].mo)
        consistancy[i][buf] = players[i].mo->x;  // player X position
    else
        consistancy[i][buf] = rndindex;           // random state
}
```

**Mechanism**:
1. Each tic, the local player embeds `consistancy[consoleplayer][tic % BACKUPTICS]`
   into their ticcmd
2. This value was computed from `players[i].mo->x` (player's X position)
   during the previous processing of that buffer slot
3. When another client receives this ticcmd, it compares the received
   `consistancy` byte against its own locally-computed value
4. If they differ, the game states have diverged

**VectorDoom note**: The check currently only prints a warning (not a fatal
error), which is important given the non-blocking server can send fabricated
inputs that may cause temporary inconsistencies.

---

## 13. WebXDC Transport Specifics

### Architecture

```
 Instance A (Server)              Instance B (Client)
 ==================              ===================
 net_webxdc.c                    net_webxdc.c
   |                               |
   v                               v
 js_webxdc_send()                js_webxdc_send()
   |                               |
   v                               v
 _webxdcChannel.send()  <--->   _webxdcChannel.send()
   |        (broadcast)            |
   v                               v
 handleMessage()                 handleMessage()
   |                               |
   +-- filter by destUID           +-- filter by destUID
   |                               |
   v                               v
 _webxdcRecvQueue                _webxdcRecvQueue
   |                               |
   v                               v
 js_webxdc_recv()                js_webxdc_recv()
   |                               |
   v                               v
 PollReceivedPackets()           PollReceivedPackets()
```

### Packet Format on Wire

```
Byte offset:  0    1    2    3    4    5    6    7    8...
              [------to------] [-----from------] [doom payload]
              Little-endian     Little-endian
              uint32 destUID    uint32 srcUID
```

All packets are **broadcast** to all peers. The JS `handleMessage()` function
filters by destination UID. Only packets addressed to `_doomInstanceUID` (or
address 0 for broadcast) are delivered to C.

### Server Election

Handled entirely in `webxdc-net.js` before WASM starts:

1. Each instance records `thisAppStartedAt = Date.now()` on load
2. Sends `[42, 42, 42, 42]` every 300ms
3. If an existing server receives this, it responds with
   `[43, 43, 43, 43][unused(4)][timestamp(8)]`
4. Receiver compares timestamps: earlier start time wins
5. After 3 seconds with no earlier server found, self-elect as server
6. The server gets `instanceUID = 1` (set in C code in `D_InitNetGame()`)

### Address Mapping

- Server always has `instanceUID = 1`
- Clients get random UIDs: `rand() % 0xfffe`
- In `net_webxdc.c`, `FindAddressByIp()` maintains a static lookup table
  mapping uint32 UIDs to `net_addr_t` pointers
- The `handle` field of `net_addr_t` points to the UID value in the `ips[]` array
- Clients connect to address "1" (the server) via
  `net_webxdc_module.ResolveAddress("1")`

### Limitations

- realtimeChannel is **unreliable** (no delivery guarantee, no ordering)
- All packets are broadcast (bandwidth scales O(n) with peers)
- No fragmentation support -- packets must fit in a single realtimeChannel message
- The JS filtering means all instances receive and discard each other's traffic

---

## 14. Connection Lifecycle

### Server-Side Flow

```
1. SERVER_WAITING_LAUNCH
   - Accept SYN connections
   - Send WAITING_DATA every 1s to each client
   - Oldest client becomes controller

2. Controller sends LAUNCH packet
   -> Forward to all clients
   -> SERVER_WAITING_START

3. Each client sends GAMESTART with settings
   - Controller's settings are adopted
   - client.ready = true
   - When AllNodesReady(): StartGame()
   -> SERVER_IN_GAME

4. IN_GAME loop:
   - Receive GAMEDATA from clients
   - PumpSendQueue for each client
   - AdvanceWindow
   - CheckResends
   - CheckDeadlock
```

### Client-Side Flow

```
1. Send SYN every 1s (up to 120s timeout)
   - Include magic number, version, protocol list, connect data, name
   - Receive SYN response -> CONNECTED

2. CLIENT_STATE_WAITING_LAUNCH
   - Receive WAITING_DATA from server
   - Receive LAUNCH -> CLIENT_STATE_WAITING_START

3. CLIENT_STATE_WAITING_START
   - Send GAMESTART with settings
   - Receive GAMESTART -> CLIENT_STATE_IN_GAME
   - Clear receive/send windows

4. IN_GAME loop:
   - Send GAMEDATA (ticcmd diffs) to server
   - Receive GAMEDATA (full ticcmds) from server
   - AdvanceWindow -> D_ReceiveTic
   - CheckResends
```

---

## 15. Disconnect Detection ("Player X left the game")

Disconnect propagates through multiple layers:

### Layer 1: Connection Timeout (net_common.c)

```c
NET_Conn_Run():
  if (nowtime - conn->keepalive_recv_time > CONNECTION_TIMEOUT_LEN * 1000) {
      conn->state = NET_CONN_STATE_DISCONNECTED;
      conn->disconnect_reason = NET_DISCONNECT_TIMEOUT;
  }
```

`CONNECTION_TIMEOUT_LEN` = 4 seconds (reduced from 30 for WebXDC).

### Layer 2: Server Detects Disconnected Client (net_server.c)

```c
NET_SV_RunClient():
  if (conn->state == NET_CONN_STATE_DISCONNECTED
      && disconnect_reason == NET_DISCONNECT_TIMEOUT) {
      NET_SV_BroadcastMessage("Client '%s' timed out and disconnected");
  }
  if (conn->state == NET_CONN_STATE_DISCONNECTED) {
      client->active = false;
      // If in waiting state, abort game
      // If no players left, call NET_SV_GameEnded()
  }
```

### Layer 3: Server Stops Including Player in Tics

When `client->active` becomes false, `sv_players[i]` eventually becomes NULL
(reassigned by `NET_SV_AssignPlayers()`). In `PumpSendQueue()`, that player
gets `cmd.playeringame[i] = false`.

### Layer 4: Client Receives Updated playeringame

The `net_full_ticcmd_t` now has `playeringame[i] = false` for the
disconnected player. `NET_CL_ExpandFullTiccmd()` passes this through to
`D_ReceiveTic()`.

### Layer 5: Game Loop Detects Player Quit

```c
// d_loop.c: D_ReceiveTic()
ticdata[recvtic % BACKUPTICS].ingame[i] = players_mask[i];  // now false

// d_loop.c: TryRunTics() -> loop_interface->RunTic()
// copies ingame to local_playeringame

// doom/d_net.c: RunTic()
for (i = 0; i < MAXPLAYERS; ++i) {
    if (!demoplayback && playeringame[i] && !ingame[i]) {
        PlayerQuitGame(&players[i]);
        // -> "Player N left the game"
        // -> playeringame[i] = false
    }
}
```

The critical comparison is `playeringame[i] && !ingame[i]` -- the global
`playeringame[]` still has the player as active, but the new tic data says
they are gone.

---

## 16. Timeout and Keepalive Mechanisms

| Mechanism | Location | Period | Action |
|-----------|----------|--------|--------|
| **Connection timeout** | net_common.c `NET_Conn_Run()` | 4 seconds no recv | State -> DISCONNECTED (TIMEOUT) |
| **Keepalive send** | net_common.c `NET_Conn_Run()` | 2 seconds no send | Send KEEPALIVE packet |
| **Reliable retransmit** | net_common.c `NET_Conn_Run()` | 1 second | Resend head of reliable queue |
| **Disconnect retry** | net_common.c `NET_Conn_Run()` | 1 second, max 5 | Resend DISCONNECT packet |
| **Disconnect ACK sleep** | net_common.c `NET_Conn_Run()` | 5 seconds | Linger in DISCONNECTED_SLEEP |
| **Server deadlock** | net_server.c `CheckDeadlock()` | 1 second no gamedata | Send resend request for next expected tic |
| **Client deadlock** | net_client.c `CheckResends()` | 1 second no gamedata | Force resend request for recvwindow[0] |
| **Resend request retry** | Both | 300ms | Re-send resend request for still-missing tics |
| **Server waiting data** | net_server.c `RunClient()` | 1 second | Send WAITING_DATA to connected clients |
| **Client SYN retry** | net_client.c `Connect()` | 1 second, 120s total | Re-send SYN packet |
| **Game loop stall** | d_loop.c `TryRunTics()` | MAX_NETGAME_STALL_TICS (10) | Bail out and render frame anyway |
| **Server election** | webxdc-net.js | 300ms probe, 3s timeout | Broadcast [42,42,42,42], self-elect if no response |
| **Server election re-announce** | webxdc-net.js | 3 seconds (server only) | Keep broadcasting for late joiners |

---

## 17. Resend / Retransmission Logic

### Server Requesting Resends from Clients

**Trigger 1 -- Out-of-order detection** (`NET_SV_ParseGameData()`):
When a packet arrives with seq > expected, the server scans backward from seq
to find missing tics. If found, sends `GAMEDATA_RESEND` to that client.

**Trigger 2 -- Timeout retry** (`NET_SV_CheckResends()`):
Scans the entire recvwindow for each player. If a tic is not active and its
resend_time is >300ms ago, re-sends the resend request.

**Trigger 3 -- Deadlock** (`NET_SV_CheckDeadlock()`):
If no gamedata from a client for >1000ms, finds the first missing tic and
sends a resend request. Also resends any tics from the server's send queue
to break mutual deadlock.

### Client Requesting Resends from Server

**Trigger 1 -- Out-of-order detection** (`NET_CL_ParseGameData()`):
Same logic as server side -- if seq > expected, scan backward for gaps.

**Trigger 2 -- Timeout retry** (`NET_CL_CheckResends()`):
Same 300ms retry logic. Additionally, if no gamedata for >1000ms and
recvwindow[0] is not active and has never been requested, force a resend
(deadlock breaker).

**Trigger 3 -- Drone acknowledgment** (`NET_CL_CheckResends()`):
If `need_to_acknowledge` is true and >200ms since last gamedata, send a
standalone GAMEDATA_ACK.

### Resend Packet Format

```
GAMEDATA_RESEND: [type(16)][start_tic(32)][count(8)]
```

---

## 18. Clock Synchronization (PID Filter)

`UpdateClockSync()` in net_client.c adjusts `offsetms` (a global fixed-point
variable) to keep client clocks synchronized:

```c
latency = now - send_queue[seq].time;  // round-trip for this tic
error = latency - remote_latency;       // how we compare to worst other player
cumul_error += error;

offsetms = KP * error - KI * cumul_error + KD * (last_error - error);
// KP = 0.1, KI = 0.01, KD = 0.02
```

`offsetms` feeds into `GetAdjustedTime()` in d_loop.c, which controls how
many tics `NetUpdate()` generates. A positive offset speeds up the clock
(generate tics faster), negative slows it down. This keeps all clients
producing tics at roughly the same rate.

Only the last tic in each received packet triggers the update (to avoid
bogus latency from packets received via extratics redundancy).

---

## 19. Packet Type Reference

| Type | Value | Direction | Reliable? | Purpose |
|------|-------|-----------|-----------|---------|
| SYN | 0 | C->S, S->C | Yes (response) | Connection handshake |
| ACK | 1 | -- | -- | Deprecated |
| REJECTED | 2 | S->C | No | Connection rejected |
| KEEPALIVE | 3 | Both | No | Prevent timeout |
| WAITING_DATA | 4 | S->C | No | Lobby status |
| GAMESTART | 5 | C->S, S->C | Yes | Game settings / start signal |
| GAMEDATA | 6 | Both | No | Ticcmd data (main game traffic) |
| GAMEDATA_ACK | 7 | C->S | No | Acknowledge received tics |
| DISCONNECT | 8 | Both | No (retried) | Request disconnect |
| DISCONNECT_ACK | 9 | Both | No | Acknowledge disconnect |
| RELIABLE_ACK | 10 | Both | No | Acknowledge reliable packet |
| GAMEDATA_RESEND | 11 | Both | No | Request retransmission |
| CONSOLE_MESSAGE | 12 | S->C | Yes | Server message to client |
| QUERY | 13 | C->S | No | Server info query |
| QUERY_RESPONSE | 14 | S->C | No | Server info response |
| LAUNCH | 15 | C->S, S->C | Yes | Game launch signal |
| NAT_HOLE_PUNCH | 16 | -- | -- | Not used in WebXDC |

The `NET_RELIABLE_PACKET` flag (bit 15) is OR'd into the type field for
reliable packets. The receiver strips this flag, processes the reliable
sequence number, sends RELIABLE_ACK, and passes the inner packet type
through for normal processing.

---

## Summary of VectorDoom-Specific Changes

1. **Transport**: `net_websockets.c` replaced with `net_webxdc.c` + `webxdc-net.js`
2. **Server election**: JavaScript-level election via magic bytes and timestamps
3. **Non-blocking server**: `PumpSendQueue()` fabricates missing tics from
   `sv_last_ticcmd[]` instead of blocking
4. **Reduced timeouts**: `CONNECTION_TIMEOUT_LEN` = 4s (was 30s)
5. **Instance UIDs**: Server always UID=1, clients get random UIDs
6. **Consistency check softened**: Prints warning instead of fatal error
7. **Packet routing**: All packets broadcast, JS filters by destination UID
