//
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// Network server code
//

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#include "d_mode.h"
#include "doomtype.h"
#include "i_system.h"
#include "i_timer.h"
#include "m_argv.h"
#include "m_misc.h"

#include "debug.h"
#include "net_client.h"
#include "net_common.h"
#include "net_defs.h"
#include "net_io.h"
#include "net_loop.h"
#include "net_packet.h"
#include "net_query.h"
#include "net_webxdc.h"
#include "net_server.h"
#include "net_structrw.h"

#include <SDL_net.h>

typedef enum {
    // waiting for the game to be "launched" (key player to press the start
    // button)

    SERVER_WAITING_LAUNCH,

    // game has been launched, we are waiting for all players to be ready
    // so the game can start.

    SERVER_WAITING_START,

    // in a game

    SERVER_IN_GAME,
} net_server_state_t;

typedef struct {
    boolean active;
    int player_number;
    net_addr_t *addr;
    net_connection_t connection;
    int last_send_time;
    char *name;

    // If true, the client has sent the NET_PACKET_TYPE_GAMESTART
    // message indicating that it is ready for the game to start.

    boolean ready;

    // Time that this client connected to the server.
    // This is used to determine the controller (oldest client).

    unsigned int connect_time;

    // Last time new gamedata was received from this client

    int last_gamedata_time;

    // recording a demo without -longtics

    boolean recording_lowres;

    // send queue: items to send to the client
    // this is a circular buffer

    int sendseq;
    net_full_ticcmd_t sendqueue[BACKUPTICS];

    // Latest acknowledged by the client

    unsigned int acknowledged;

    // Value of max_players specified by the client on connect.

    int max_players;

    // Observer: receives data but does not participate in the game.

    boolean drone;

    // SHA1 hash sums of the client's WAD directory and dehacked data

    sha1_digest_t wad_sha1sum;
    sha1_digest_t deh_sha1sum;

    // Is this client is playing with the Freedoom IWAD?

    unsigned int is_freedoom;

    // Player class (for Hexen)

    int player_class;

} net_client_t;

// structure used for the recv window

typedef struct {
    // Whether this tic has been received yet

    boolean active;

    // Latency value received from the client

    signed int latency;

    // Last time we sent a resend request for this tic

    unsigned int resend_time;

    // Tic data itself

    net_ticdiff_t diff;
} net_client_recv_t;

static net_server_state_t server_state;
static boolean server_initialized = false;
static net_client_t clients[MAXNETNODES];
static net_client_t *sv_players[NET_MAXPLAYERS];
static net_context_t *server_context;
static unsigned int sv_gamemode;
static unsigned int sv_gamemission;
static net_gamesettings_t sv_settings;

// receive window

static unsigned int recvwindow_start;
static net_client_recv_t recvwindow[BACKUPTICS][NET_MAXPLAYERS];

// Non-blocking server: track each player's last known full ticcmd
// (reconstructed from running diffs, not just the last diff)
static ticcmd_t sv_last_ticcmd[NET_MAXPLAYERS];
static boolean sv_player_has_sent[NET_MAXPLAYERS];
static int sv_loopback_player = -1;  // player index of the host (loopback client)

#define NET_SV_ExpandTicNum(b) NET_ExpandTicNum(recvwindow_start, (b))

static void NET_SV_DisconnectClient(net_client_t *client)
{
    if (client->active) {
        NET_Conn_Disconnect(&client->connection);
    }
}

static boolean ClientConnected(net_client_t *client)
{
    // Check that the client is properly connected: ie. not in the
    // process of connecting or disconnecting

    return client->active && client->connection.state == NET_CONN_STATE_CONNECTED;
}

// Send a message to be displayed on a client's console

static void NET_SV_SendConsoleMessage(net_client_t *client, const char *s, ...) PRINTF_ATTR(2, 3);
static void NET_SV_SendConsoleMessage(net_client_t *client, const char *s, ...)
{
    char buf[1024];
    va_list args;
    net_packet_t *packet;

    va_start(args, s);
    M_vsnprintf(buf, sizeof(buf), s, args);
    va_end(args);

    packet = NET_Conn_NewReliable(&client->connection, NET_PACKET_TYPE_CONSOLE_MESSAGE);

    NET_WriteString(packet, buf);
}

// Send a message to all clients

static void NET_SV_BroadcastMessage(const char *s, ...) PRINTF_ATTR(1, 2);
static void NET_SV_BroadcastMessage(const char *s, ...)
{
    char buf[1024];
    va_list args;
    int i;

    va_start(args, s);
    M_vsnprintf(buf, sizeof(buf), s, args);
    va_end(args);

    for (i = 0; i < MAXNETNODES; ++i) {
        if (ClientConnected(&clients[i])) {
            NET_SV_SendConsoleMessage(&clients[i], "%s", buf);
        }
    }

    printf("%s\n", buf);
}

// Assign player numbers to connected clients

static void NET_SV_AssignPlayers(void)
{
    int i;
    int pl;

    pl = 0;

    for (i = 0; i < MAXNETNODES; ++i) {
        if (ClientConnected(&clients[i])) {
            if (!clients[i].drone) {
                sv_players[pl] = &clients[i];
                sv_players[pl]->player_number = pl;
                ++pl;
            }
            else {
                clients[i].player_number = -1;
            }
        }
    }

    for (; pl < NET_MAXPLAYERS; ++pl) {
        sv_players[pl] = NULL;
    }
}

// Returns the number of players currently connected.

static int NET_SV_NumPlayers(void)
{
    int i;
    int result;

    result = 0;

    for (i = 0; i < NET_MAXPLAYERS; ++i) {
        if (sv_players[i] != NULL && ClientConnected(sv_players[i])) {
            result += 1;
        }
    }

    return result;
}

// Returns the number of players ready to start the game.

static int NET_SV_NumReadyPlayers(void)
{
    int result = 0;
    int i;

    for (i = 0; i < MAXNETNODES; ++i) {
        if (ClientConnected(&clients[i]) && !clients[i].drone && clients[i].ready) {
            ++result;
        }
    }

    return result;
}

// Returns the maximum number of players that can play.

static int NET_SV_MaxPlayers(void)
{
    int i;

    for (i = 0; i < MAXNETNODES; ++i) {
        if (ClientConnected(&clients[i])) {
            return clients[i].max_players;
        }
    }

    return NET_MAXPLAYERS;
}

// Returns the number of drones currently connected.

static int NET_SV_NumDrones(void)
{
    int i;
    int result;

    result = 0;

    for (i = 0; i < MAXNETNODES; ++i) {
        if (ClientConnected(&clients[i]) && clients[i].drone) {
            result += 1;
        }
    }

    return result;
}

// returns the number of clients connected

static int NET_SV_NumClients(void)
{
    int count;
    int i;

    count = 0;

    for (i = 0; i < MAXNETNODES; ++i) {
        if (ClientConnected(&clients[i])) {
            ++count;
        }
    }

    return count;
}

// returns a pointer to the client which controls the server

static net_client_t *NET_SV_Controller(void)
{
    net_client_t *best;
    int i;

    // Find the oldest client (first to connect).

    best = NULL;

    for (i = 0; i < MAXNETNODES; ++i) {
        // Can't be controller?

        if (!ClientConnected(&clients[i]) || clients[i].drone) {
            continue;
        }

        if (best == NULL || clients[i].connect_time < best->connect_time) {
            best = &clients[i];
        }
    }

    return best;
}

static void NET_SV_SendWaitingData(net_client_t *client)
{
    net_waitdata_t wait_data;
    net_packet_t *packet;
    net_client_t *controller;
    int i;

    NET_SV_AssignPlayers();

    controller = NET_SV_Controller();

    wait_data.num_players = NET_SV_NumPlayers();
    wait_data.num_drones = NET_SV_NumDrones();
    wait_data.ready_players = NET_SV_NumReadyPlayers();
    wait_data.max_players = NET_SV_MaxPlayers();
    wait_data.is_controller = (client == controller);
    wait_data.consoleplayer = client->player_number;

    // Send the WAD and dehacked checksums of the controlling client.
    // If no controller found (?), send the details that the client
    // is expecting anyway.

    if (controller == NULL) {
        controller = client;
    }

    memcpy(&wait_data.wad_sha1sum, &controller->wad_sha1sum, sizeof(sha1_digest_t));
    memcpy(&wait_data.deh_sha1sum, &controller->deh_sha1sum, sizeof(sha1_digest_t));
    wait_data.is_freedoom = controller->is_freedoom;

    // set name and address of each player:

    for (i = 0; i < wait_data.num_players; ++i) {
        M_StringCopy(wait_data.player_names[i], sv_players[i]->name, MAXPLAYERNAME);
        M_StringCopy(wait_data.player_addrs[i], NET_AddrToString(sv_players[i]->addr), MAXPLAYERNAME);
    }

    // Construct packet:

    packet = NET_NewPacket(10);
    NET_WriteInt16(packet, NET_PACKET_TYPE_WAITING_DATA);
    NET_WriteWaitData(packet, &wait_data);

    // Send packet to client and free

    NET_Conn_SendPacket(&client->connection, packet);
    NET_FreePacket(packet);
}

// Find the latest tic which has been acknowledged as received by
// all clients.

static unsigned int NET_SV_LatestAcknowledged(void)
{
    unsigned int lowtic = UINT_MAX;
    int i;

    for (i = 0; i < MAXNETNODES; ++i) {
        if (ClientConnected(&clients[i])) {
            // VectorDoom: only consider loopback clients for the ack floor.
            // Remote clients don't receive tics (PumpSendQueue skips them),
            // so their acknowledged never advances. Including them would
            // stall AdvanceWindow and block the entire server.
            boolean is_loopback = (clients[i].addr
                && clients[i].addr->module == &net_loop_server_module);
            if (!is_loopback) continue;

            if (clients[i].acknowledged < lowtic) {
                lowtic = clients[i].acknowledged;
            }
        }
    }

    return lowtic;
}

// Possibly advance the recv window if all connected clients have
// used the data in the window

static void NET_SV_AdvanceWindow(void)
{
    unsigned int lowtic;

    if (NET_SV_NumPlayers() <= 0) {
        return;
    }

    lowtic = NET_SV_LatestAcknowledged();

    // Advance the recv window until it catches up with lowtic

    while (recvwindow_start < lowtic) {
        // Non-blocking: advance unconditionally up to lowtic.
        // PumpSendQueue handles missing tics by using last-known-input
        // directly (without modifying recvwindow), so it's safe to
        // discard old positions here. The window is naturally throttled
        // by client ack round-trip time, giving real data time to arrive.

        memmove(recvwindow, recvwindow + 1, sizeof(*recvwindow) * (BACKUPTICS - 1));
        memset(&recvwindow[BACKUPTICS - 1], 0, sizeof(*recvwindow));
        ++recvwindow_start;
        NET_Log("server: advanced receive window to %d", recvwindow_start);
    }
}

// Given an address, find the corresponding client

static net_client_t *NET_SV_FindClient(net_addr_t *addr)
{
    int i;

    int nc = 0;
    for (i = 0; i < MAXNETNODES; ++i) {
        if (clients[i].active) nc++;
    }

    for (i = 0; i < MAXNETNODES; ++i) {
        if (clients[i].active && clients[i].addr == addr) {
            // found the client

            return &clients[i];
        }
    }

    return NULL;
}

// send a rejection packet to a client

static void NET_SV_SendReject(net_addr_t *addr, const char *msg)
{
    net_packet_t *packet;

    NET_Log("server: sending reject to %s", NET_AddrToString(addr));

    packet = NET_NewPacket(10);
    NET_WriteInt16(packet, NET_PACKET_TYPE_REJECTED);
    NET_WriteString(packet, msg);
    NET_SendPacket(addr, packet);
    NET_FreePacket(packet);
}

static void NET_SV_InitNewClient(net_client_t *client, net_addr_t *addr, net_protocol_t protocol)
{
    client->active = true;
    client->connect_time = I_GetTimeMS();
    NET_Conn_InitServer(&client->connection, addr, protocol);
    client->addr = addr;
    NET_ReferenceAddress(addr);
    client->last_send_time = -1;

    // init the ticcmd send queue

    client->sendseq = 0;
    client->acknowledged = 0;
    client->drone = false;
    client->ready = false;

    client->last_gamedata_time = 0;

    memset(client->sendqueue, 0xff, sizeof(client->sendqueue));

    NET_Log("server: initialized new client from %s", NET_AddrToString(addr));
}

// Handle a late joiner connecting during SERVER_IN_GAME.
// Fast-track them through LAUNCH + GAMESTART and notify existing clients.

static void NET_SV_HandleLateJoin(net_client_t *new_client)
{
    net_packet_t *launchpacket;
    net_packet_t *startpacket;
    net_packet_t *joinpacket;
    int slot = -1;
    int max_players;
    int num_players;
    unsigned int i;

    // NET_SV_AssignPlayers() ran during ParseSYN and may have already
    // assigned this client to a sequential slot. Clear that assignment
    // first so we can pick the correct slot without double-booking.
    for (i = 0; i < NET_MAXPLAYERS; ++i) {
        if (sv_players[i] == new_client) {
            sv_players[i] = NULL;
        }
    }

    // Find a free player slot (don't reassign existing players)
    max_players = NET_SV_MaxPlayers();
    for (i = 0; i < (unsigned int)max_players; ++i) {
        if (sv_players[i] == NULL) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        NET_Log("server: late join rejected, game is full");
        NET_SV_SendReject(new_client->addr, "Game is full!");
        new_client->active = false;
        return;
    }

    // Assign player number
    sv_players[slot] = new_client;
    new_client->player_number = slot;
    new_client->drone = false;
    new_client->ready = true;

    // Compute num_players as highest occupied slot + 1 (not just count).
    // This ensures local_playeringame[] covers all active slots,
    // even if there are gaps from disconnected players.
    num_players = 0;
    for (i = 0; i < NET_MAXPLAYERS; ++i) {
        if (sv_players[i] != NULL && ClientConnected(sv_players[i])) {
            num_players = i + 1;
        }
    }

    NET_Log("server: late join accepted, player %d (num_players %d)", slot, num_players);
    printf("Server: player %d joined mid-game (%d total)\n", slot, num_players);

    // Sync ticcmd sequence to current server position.
    // Both sendseq and acknowledged must start at recvwindow_start —
    // otherwise CheckDeadlock tries to resend from acknowledged=0 through
    // sendseq-1, hitting uninitialized sendqueue slots.
    new_client->sendseq = recvwindow_start;
    new_client->acknowledged = recvwindow_start;
    new_client->last_gamedata_time = I_GetTimeMS();

    // Pre-fill sendqueue for the extratics lookback range.
    // PumpSendQueue does: starttic = sendseq - extratics, then reads
    // sendqueue[starttic % BACKUPTICS]. Without this, those slots are
    // 0xff from InitNewClient's memset, causing "Wanted to send X, but
    // -1 is in its place" crash.
    {
        int fill_start = recvwindow_start - sv_settings.extratics;
        int fill_end = recvwindow_start;  // exclusive
        int t;

        if (fill_start < 0) fill_start = 0;

        for (t = fill_start; t < fill_end; ++t) {
            net_full_ticcmd_t *cmd = &new_client->sendqueue[t % BACKUPTICS];
            memset(cmd, 0, sizeof(*cmd));
            cmd->seq = t;
        }
    }

    // Send LAUNCH (reliable) — transitions client from WAITING_LAUNCH to WAITING_START
    launchpacket = NET_Conn_NewReliable(&new_client->connection, NET_PACKET_TYPE_LAUNCH);
    NET_WriteInt8(launchpacket, num_players);

    // Send GAMESTART (reliable) — transitions client from WAITING_START to IN_GAME
    startpacket = NET_Conn_NewReliable(&new_client->connection, NET_PACKET_TYPE_GAMESTART);
    sv_settings.consoleplayer = slot;
    sv_settings.num_players = num_players;
    sv_settings.start_tic = recvwindow_start;
    NET_WriteSettings(startpacket, &sv_settings);

    // Broadcast PLAYER_JOINED to all connected clients (including loopback/host).
    // The host receives this via loopback → NET_CL_ParsePlayerJoined →
    // D_PlayerJoinedMidGame, keeping the code path unified.
    for (i = 0; i < MAXNETNODES; ++i) {
        if (!ClientConnected(&clients[i])) continue;
        if (&clients[i] == new_client) continue;
        joinpacket = NET_Conn_NewReliable(&clients[i].connection,
                                           NET_PACKET_TYPE_PLAYER_JOINED);
        NET_WriteInt8(joinpacket, slot);
    }
}

// parse a SYN from a client(initiating a connection)

static void NET_SV_ParseSYN(net_packet_t *packet, net_client_t *client, net_addr_t *addr)
{
    unsigned int magic;
    net_connect_data_t data;
    net_packet_t *reply;
    net_protocol_t protocol;
    char *player_name;
    char *client_version;
    int num_players;
    int i;

    if (addr->handle) {
        NET_Log("server: processing SYN packet from %u\n", (*(uint32_t *)(addr->handle)));
    }

    // Read the magic number and check it is the expected one.
    if (!NET_ReadInt32(packet, &magic)) {
        NET_Log("server: error: no magic number for SYN");
        return;
    }

    switch (magic) {
    case NET_MAGIC_NUMBER:
        break;

    case NET_OLD_MAGIC_NUMBER:
        NET_Log("server: error: client using old magic number: %d", magic);
        NET_SV_SendReject(addr, "You are using an old client version that is not supported by "
                                "this server. This server is running " PACKAGE_STRING ".");
        return;

    default:
        NET_Log("server: error: wrong magic number: %d", magic);
        return;
    }

    // Read the client version string. We actually now only use this when
    // sending a reject message, as we only reject if we can't negotiate a
    // common protocol (below).
    client_version = NET_ReadString(packet);
    if (client_version == NULL) {
        NET_Log("server: error: no version from client");
        return;
    }

    // Read the client's list of accepted protocols. Net play between forks
    // of Chocolate Doom is accepted provided that they can negotiate a
    // common accepted protocol.
    protocol = NET_ReadProtocolList(packet);
    if (protocol == NET_PROTOCOL_UNKNOWN) {
        char reject_msg[256];

        M_snprintf(reject_msg, sizeof(reject_msg),
                   "Version mismatch: server version is: " PACKAGE_STRING "; "
                   "client is: %s. No common compatible protocol could be "
                   "negotiated.",
                   client_version);
        NET_SV_SendReject(addr, reject_msg);
        NET_Log("server: error: no common protocol");
        return;
    }

    // Read connect data, and check that the game mode/mission are valid
    // and the max_players value is in a sensible range.
    if (!NET_ReadConnectData(packet, &data)) {
        NET_Log("server: error: failed to read connect data");
        return;
    }

    if (!D_ValidGameMode(data.gamemission, data.gamemode) || data.max_players > NET_MAXPLAYERS) {
        NET_Log("server: error: invalid connect data, max_players=%d, "
                "gamemission=%d, gamemode=%d",
                data.max_players, data.gamemission, data.gamemode);
        return;
    }

    // Read the player's name
    player_name = NET_ReadString(packet);
    if (player_name == NULL) {
        NET_Log("server: error: failed to read player name");
        return;
    }

    // At this point we have received a valid SYN.

    // Not accepting new connections?
    if (server_state != SERVER_WAITING_LAUNCH && server_state != SERVER_IN_GAME) {
        NET_Log("server: error: not in waiting launch/in-game state, server_state=%d", server_state);
        NET_SV_SendReject(addr, "Server is not currently accepting connections");
        return;
    }

    // Before accepting a new client, check that there is a slot free.
    NET_SV_AssignPlayers();
    num_players = NET_SV_NumPlayers();

    if ((!data.drone && num_players >= NET_SV_MaxPlayers()) || NET_SV_NumClients() >= MAXNETNODES) {
        NET_Log("server: no more players, num_players=%d, max=%d", num_players, NET_SV_MaxPlayers());
        NET_SV_SendReject(addr, "Server is full!");
        return;
    }

    // TODO: Add server option to allow rejecting clients which set
    // lowres_turn.  This is potentially desirable as the presence of such
    // clients affects turning resolution.

    // Adopt the game mode and mission of the first connecting client:
    if (num_players == 0 && !data.drone) {
        sv_gamemode = data.gamemode;
        sv_gamemission = data.gamemission;
        NET_Log("server: new game, mode=%d, mission=%d", sv_gamemode, sv_gamemission);
    }

    // Check the connecting client is playing the same game as all
    // the other clients
    if (data.gamemode != sv_gamemode || data.gamemission != sv_gamemission) {
        char msg[128];
        NET_Log("server: wrong mode/mission, %d != %d || %d != %d", data.gamemode, sv_gamemode, data.gamemission,
                sv_gamemission);
        M_snprintf(msg, sizeof(msg), "Game mismatch: server is %s (%s), client is %s (%s)",
                   D_GameMissionString(sv_gamemission), D_GameModeString(sv_gamemode),
                   D_GameMissionString(data.gamemission), D_GameModeString(data.gamemode));

        NET_SV_SendReject(addr, msg);
        return;
    }

    // Allocate a client slot if there isn't one already
    if (client == NULL) {
        // find a slot, or return if none found

        for (i = 0; i < MAXNETNODES; ++i) {
            if (!clients[i].active) {
                client = &clients[i];
                break;
            }
        }

        if (client == NULL) {
            return;
        }
    }
    else {
        // If this is a recently-disconnected client, deactivate
        // to allow immediate reconnection

        if (client->connection.state == NET_CONN_STATE_DISCONNECTED) {
            client->active = false;
        }
    }

    // Client already connected?
    if (client->active) {
        NET_Log("server: client is already initialized (duplicate SYN?)");
        return;
    }

    // Activate, initialize connection
    NET_SV_InitNewClient(client, addr, protocol);

    // Save the SHA1 checksums and other details.
    memcpy(client->wad_sha1sum, data.wad_sha1sum, sizeof(sha1_digest_t));
    memcpy(client->deh_sha1sum, data.deh_sha1sum, sizeof(sha1_digest_t));
    client->is_freedoom = data.is_freedoom;
    client->max_players = data.max_players;
    client->name = M_StringDuplicate(player_name);
    client->recording_lowres = data.lowres_turn;
    client->drone = data.drone;
    client->player_class = data.player_class;

    // Send a reply back to the client, indicating a successful connection
    // and specifying the protocol that will be used for communications.
    reply = NET_Conn_NewReliable(&client->connection, NET_PACKET_TYPE_SYN);
    NET_WriteString(reply, PACKAGE_STRING);
    NET_WriteProtocol(reply, protocol);

    // If game is already running, fast-track this client into the game
    if (server_state == SERVER_IN_GAME) {
        NET_SV_HandleLateJoin(client);
    }
}

// Parse a launch packet. This is sent by the key player when the "start"
// button is pressed, and causes the startup process to continue.

static void NET_SV_ParseLaunch(net_packet_t *packet, net_client_t *client)
{
    net_packet_t *launchpacket;
    int num_players;
    unsigned int i;

    NET_Log("server: processing launch packet");

    // Only the controller can launch the game.

    if (client != NET_SV_Controller()) {
        NET_Log("server: error: this client isn't the controller, %d != %d", client, NET_SV_Controller());
        return;
    }

    // Can only launch when we are in the waiting state.

    if (server_state != SERVER_WAITING_LAUNCH) {
        NET_Log("server: error: not in waiting launch state, state=%d", server_state);
        return;
    }

    // Forward launch on to all clients.
    NET_Log("server: sending launch to all clients");
    NET_SV_AssignPlayers();
    num_players = NET_SV_NumPlayers();

    for (i = 0; i < MAXNETNODES; ++i) {
        if (!ClientConnected(&clients[i])) continue;

        launchpacket = NET_Conn_NewReliable(&clients[i].connection, NET_PACKET_TYPE_LAUNCH);
        NET_WriteInt8(launchpacket, num_players);
    }

    // Now in launch state.

    server_state = SERVER_WAITING_START;
}

// Transition to the in-game state and send all players the start game
// message. Invoked once all players have indicated they are ready to
// start the game.

static void StartGame(void)
{
    net_packet_t *startpacket;
    unsigned int i;
    int nowtime;

    // Assign player numbers

    NET_SV_AssignPlayers();

    // Check if anyone is recording a demo and set lowres_turn if so.

    sv_settings.lowres_turn = false;

    for (i = 0; i < NET_MAXPLAYERS; ++i) {
        if (sv_players[i] != NULL && sv_players[i]->recording_lowres) {
            sv_settings.lowres_turn = true;
        }
    }

    sv_settings.num_players = NET_SV_NumPlayers();
    sv_settings.start_tic = 0;

    // Copy player classes:

    for (i = 0; i < NET_MAXPLAYERS; ++i) {
        if (sv_players[i] != NULL) {
            sv_settings.player_classes[i] = sv_players[i]->player_class;
        }
        else {
            sv_settings.player_classes[i] = 0;
        }
    }

    nowtime = I_GetTimeMS();

    // Send start packets to each connected node

    for (i = 0; i < MAXNETNODES; ++i) {
        if (!ClientConnected(&clients[i])) continue;

        clients[i].last_gamedata_time = nowtime;

        startpacket = NET_Conn_NewReliable(&clients[i].connection, NET_PACKET_TYPE_GAMESTART);

        sv_settings.consoleplayer = clients[i].player_number;

        NET_WriteSettings(startpacket, &sv_settings);
    }

    // Change server state
    NET_Log("server: beginning game state");
    server_state = SERVER_IN_GAME;

    memset(recvwindow, 0, sizeof(recvwindow));
    recvwindow_start = 0;

    memset(sv_last_ticcmd, 0, sizeof(sv_last_ticcmd));
    memset(sv_player_has_sent, 0, sizeof(sv_player_has_sent));

    // Find the loopback player (host)
    sv_loopback_player = -1;
    for (i = 0; i < MAXNETNODES; ++i) {
        if (clients[i].active && clients[i].addr
            && clients[i].addr->module == &net_loop_server_module) {
            sv_loopback_player = clients[i].player_number;
            break;
        }
    }
}

// Returns true when all nodes have indicated readiness to start the game.

static boolean AllNodesReady(void)
{
    unsigned int i;

    for (i = 0; i < MAXNETNODES; ++i) {
        if (ClientConnected(&clients[i]) && !clients[i].ready) {
            return false;
        }
    }

    return true;
}

// Check if the game should start, and if so, start it.

static void CheckStartGame(void)
{
    if (!AllNodesReady()) {
        NET_Log("server: not all clients ready to start yet");
        return;
    }

    NET_Log("server: all clients ready, starting game");
    StartGame();
}

// Send waiting data with current status to all nodes that are ready to
// start the game.

static void SendAllWaitingData(void)
{
    unsigned int i;

    for (i = 0; i < MAXNETNODES; ++i) {
        if (ClientConnected(&clients[i]) && clients[i].ready) {
            NET_SV_SendWaitingData(&clients[i]);
        }
    }
}

// Parse a game start packet

static void NET_SV_ParseGameStart(net_packet_t *packet, net_client_t *client)
{
    net_gamesettings_t settings;

    NET_Log("server: processing game start packet");

    // Can only start a game if we are in the waiting start state.

    if (server_state != SERVER_WAITING_START) {
        NET_Log("server: error: not in waiting start state, server_state=%d", server_state);
        return;
    }

    if (client == NET_SV_Controller()) {
        if (!NET_ReadSettings(packet, &settings)) {
            // Malformed packet
            NET_Log("server: error: no settings from controller");
            return;
        }

        // Check the game settings are valid

        if (!NET_ValidGameSettings(sv_gamemode, sv_gamemission, &settings)) {
            NET_Log("server: error: invalid game settings");
            return;
        }

        sv_settings = settings;
    }

    client->ready = true;

    CheckStartGame();

    // Update all ready clients with the current state (number of players
    // ready, etc.). This is used by games that show startup progress
    // (eg. Hexen's spinal loading)

    SendAllWaitingData();
}

// Send a resend request to a client

static void NET_SV_SendResendRequest(net_client_t *client, int start, int end)
{
    net_packet_t *packet;
    net_client_recv_t *recvobj;
    int i;
    unsigned int nowtime;
    int index;

    NET_Log("server: send resend to %s for tics %d-%d", NET_AddrToString(client->addr), start, end);

    packet = NET_NewPacket(20);

    NET_WriteInt16(packet, NET_PACKET_TYPE_GAMEDATA_RESEND);
    NET_WriteInt32(packet, start);
    NET_WriteInt8(packet, end - start + 1);

    NET_Conn_SendPacket(&client->connection, packet);
    NET_FreePacket(packet);

    // Store the time we send the resend request

    nowtime = I_GetTimeMS();

    for (i = start; i <= end; ++i) {
        index = i - recvwindow_start;

        if (index >= BACKUPTICS) {
            // Outside the range

            continue;
        }

        recvobj = &recvwindow[index][client->player_number];

        recvobj->resend_time = nowtime;
    }
}

// Check for expired resend requests

static void NET_SV_CheckResends(net_client_t *client)
{
    int i;
    int player;
    int resend_start, resend_end;
    unsigned int nowtime;

    nowtime = I_GetTimeMS();

    player = client->player_number;
    resend_start = -1;
    resend_end = -1;

    for (i = 0; i < BACKUPTICS; ++i) {
        net_client_recv_t *recvobj;
        boolean need_resend;

        recvobj = &recvwindow[i][player];

        // if need_resend is true, this tic needs another retransmit
        // request (300ms timeout)

        need_resend = !recvobj->active && recvobj->resend_time != 0 && nowtime > recvobj->resend_time + 300;

        if (need_resend) {
            // Start a new run of resend tics?
            if (resend_start < 0) {
                resend_start = i;
            }
            resend_end = i;
        }
        else if (resend_start >= 0) {
            // End of a run of resend tics
            NET_Log("server: resend request to %s timed out for %d-%d (%d)", NET_AddrToString(client->addr),
                    recvwindow_start + resend_start, recvwindow_start + resend_end,
                    &recvwindow[resend_start][player].resend_time);
            NET_SV_SendResendRequest(client, recvwindow_start + resend_start, recvwindow_start + resend_end);

            resend_start = -1;
        }
    }

    if (resend_start >= 0) {
        NET_Log("server: resend request to %s timed out for %d-%d (%d)", NET_AddrToString(client->addr),
                recvwindow_start + resend_start, recvwindow_start + resend_end,
                &recvwindow[resend_start][player].resend_time);
        NET_SV_SendResendRequest(client, recvwindow_start + resend_start, recvwindow_start + resend_end);
    }
}

// Process game data from a client

static void NET_SV_ParseGameData(net_packet_t *packet, net_client_t *client)
{
    net_client_recv_t *recvobj;
    unsigned int seq;
    unsigned int ackseq;
    unsigned int num_tics;
    unsigned int nowtime;
    size_t i;
    int player;
    int resend_start, resend_end;
    int index;

    if (server_state != SERVER_IN_GAME) {
        NET_Log("server: error: not in game state: server_state=%d", server_state);
        return;
    }

    if (client->drone) {
        // Drones do not contribute any game data.
        NET_Log("server: error: game data from a drone?");
        return;
    }

    player = client->player_number;

    // Read header
    if (!NET_ReadInt8(packet, &ackseq) || !NET_ReadInt8(packet, &seq) || !NET_ReadInt8(packet, &num_tics)) {
        NET_Log("server: error: failed to read header");
        return;
    }

    NET_Log("server: got game data, seq=%d, num_tics=%d, ackseq=%d", seq, num_tics, ackseq);

    // Get the current time
    nowtime = I_GetTimeMS();

    // Expand 8-bit values to the full sequence number
    ackseq = NET_SV_ExpandTicNum(ackseq);
    seq = NET_SV_ExpandTicNum(seq);

    // Sanity checks

    for (i = 0; i < num_tics; ++i) {
        net_ticdiff_t diff;
        signed int latency;

        if (!NET_ReadSInt16(packet, &latency) || !NET_ReadTiccmdDiff(packet, &diff, sv_settings.lowres_turn)) {
            return;
        }

        index = seq + i - recvwindow_start;

        if (index < 0 || index >= BACKUPTICS) {
            // Not in range of the recv window

            continue;
        }

        recvobj = &recvwindow[index][player];
        recvobj->active = true;
        recvobj->diff = diff;
        recvobj->latency = latency;

        // Reconstruct the full ticcmd by patching the running baseline.
        // This gives us complete absolute values for fabrication.
        NET_TiccmdPatch(&sv_last_ticcmd[player], &diff, &sv_last_ticcmd[player]);
        sv_player_has_sent[player] = true;

        client->last_gamedata_time = nowtime;
        NET_Log("server: stored tic %d for player %d", seq + i, player);
    }

    // Higher acknowledgement point?

    if (ackseq > client->acknowledged) {
        NET_Log("server: acknowledged up to %d", ackseq);
        client->acknowledged = ackseq;
    }

    // Has this been received out of sequence, ie. have we not received
    // all tics before the first tic in this packet?  If so, send a
    // resend request.

    // printf("SV: %p: %i\n", client, seq);

    resend_end = seq - recvwindow_start;

    if (resend_end <= 0) return;

    if (resend_end >= BACKUPTICS) resend_end = BACKUPTICS - 1;

    index = resend_end - 1;
    resend_start = resend_end;

    while (index >= 0) {
        recvobj = &recvwindow[index][player];

        if (recvobj->active) {
            // ended our run of unreceived tics

            break;
        }

        if (recvobj->resend_time != 0) {
            // Already sent a resend request for this tic

            break;
        }

        resend_start = index;
        --index;
    }

    // Possibly send a resend request
    if (resend_start < resend_end) {
        NET_Log("server: request resend for %d-%d before %d", recvwindow_start + resend_start,
                recvwindow_start + resend_end - 1, seq);
        NET_SV_SendResendRequest(client, recvwindow_start + resend_start, recvwindow_start + resend_end - 1);
    }
}

static void NET_SV_ParseGameDataACK(net_packet_t *packet, net_client_t *client)
{
    unsigned int ackseq;

    NET_Log("server: processing game data ack packet");

    if (server_state != SERVER_IN_GAME) {
        NET_Log("server: error: not in game state, server_state=%d", server_state);
        return;
    }

    // Read header

    if (!NET_ReadInt8(packet, &ackseq)) {
        NET_Log("server: error: missing acknowledgement field");
        return;
    }

    // Expand 8-bit values to the full sequence number

    ackseq = NET_SV_ExpandTicNum(ackseq);

    // Higher acknowledgement point than we already have?

    if (ackseq > client->acknowledged) {
        NET_Log("server: acknowledged up to %d", ackseq);
        client->acknowledged = ackseq;
    }
}

static void NET_SV_SendTics(net_client_t *client, unsigned int start, unsigned int end)
{
    net_packet_t *packet;
    unsigned int i;

    packet = NET_NewPacket(500);

    NET_WriteInt16(packet, NET_PACKET_TYPE_GAMEDATA);

    // Send the start tic and number of tics

    NET_WriteInt8(packet, start & 0xff);
    NET_WriteInt8(packet, end - start + 1);

    // Write the tics

    for (i = start; i <= end; ++i) {
        net_full_ticcmd_t *cmd;

        cmd = &client->sendqueue[i % BACKUPTICS];

        if (i != cmd->seq) {
            I_Error("Wanted to send %i, but %i is in its place", i, cmd->seq);
        }

        // Add command

        NET_WriteFullTiccmd(packet, cmd, sv_settings.lowres_turn);
    }

    // Send packet

    NET_Conn_SendPacket(&client->connection, packet);

    NET_FreePacket(packet);
}

// Parse a retransmission request from a client

static void NET_SV_ParseResendRequest(net_packet_t *packet, net_client_t *client)
{
    unsigned int start, last;
    unsigned int num_tics;
    unsigned int i;

    NET_Log("server: processing resend request");

    // Read the starting tic and number of tics

    if (!NET_ReadInt32(packet, &start) || !NET_ReadInt8(packet, &num_tics)) {
        NET_Log("server: error: missing fields for resend");
        return;
    }

    // printf("SV: %p: resend %i-%i\n", client, start, start+num_tics-1);

    // Check we have all the requested tics

    last = start + num_tics - 1;

    for (i = start; i <= last; ++i) {
        net_full_ticcmd_t *cmd;

        cmd = &client->sendqueue[i % BACKUPTICS];

        if (i != cmd->seq) {
            // We do not have the requested tic (any more)
            // This is pretty fatal.  We could disconnect the client,
            // but then again this could be a spoofed packet.  Just
            // ignore it.
            NET_Log("server: error: don't have tic %d any more, "
                    "can't resend",
                    i);
            return;
        }
    }

    // Resend those tics
    NET_Log("server: resending tics %d-%d", start, last);
    NET_SV_SendTics(client, start, last);
}

// Send a response back to the client

void NET_SV_SendQueryResponse(net_addr_t *addr)
{
    net_packet_t *reply;
    net_querydata_t querydata;
    int p;

    // Version

    querydata.version = PACKAGE_STRING;

    // Server state

    querydata.server_state = server_state;

    // Number of players/maximum players

    querydata.num_players = NET_SV_NumPlayers();
    querydata.max_players = NET_SV_MaxPlayers();

    // Game mode/mission

    querydata.gamemode = sv_gamemode;
    querydata.gamemission = sv_gamemission;

    //!
    // @category net
    // @arg <name>
    //
    // When starting a network server, specify a name for the server.
    //

    p = M_CheckParmWithArgs("-servername", 1);

    if (p > 0) {
        querydata.description = myargv[p + 1];
    }
    else {
        querydata.description = "Unnamed server";
    }

    // Send it and we're done.
    NET_Log("server: sending query response to %s", NET_AddrToString(addr));
    reply = NET_NewPacket(64);
    NET_WriteInt16(reply, NET_PACKET_TYPE_QUERY_RESPONSE);
    NET_WriteQueryData(reply, &querydata);
    NET_SendPacket(addr, reply);
    NET_FreePacket(reply);
}

// VectorDoom: generic raw packet forwarder — copies remaining packet bytes
// to all other connected clients with a new packet type header.
static void NET_SV_ForwardRawPacket(net_packet_t *packet, net_client_t *sender, int ptype)
{
    unsigned int i;
    // Snapshot remaining bytes after the type header (already consumed)
    int remaining = packet->len - packet->pos;
    unsigned char *raw = packet->data + packet->pos;

    for (i = 0; i < MAXNETNODES; ++i) {
        net_packet_t *fwd;
        if (!ClientConnected(&clients[i]) || &clients[i] == sender) continue;

        fwd = NET_NewPacket(remaining + 4);
        NET_WriteInt16(fwd, ptype);
        memcpy(fwd->data + fwd->len, raw, remaining);
        fwd->len += remaining;
        NET_Conn_SendPacket(&clients[i].connection, fwd);
        NET_FreePacket(fwd);
    }
}

// Forward a player state snapshot to all other connected clients

static void NET_SV_ForwardPlayerState(net_packet_t *packet, net_client_t *sender) {
    unsigned int i;
    unsigned int player, tick;
    int data[9];
    int j;

    if (!NET_ReadInt8(packet, &player)) return;
    if (!NET_ReadInt32(packet, &tick)) return;
    for (j = 0; j < 9; j++) {
        if (!NET_ReadSInt32(packet, &data[j])) return;
    }

    for (i = 0; i < MAXNETNODES; ++i) {
        net_packet_t *fwd;
        if (!ClientConnected(&clients[i]) || &clients[i] == sender) continue;

        fwd = NET_NewPacket(48);
        NET_WriteInt16(fwd, NET_PACKET_TYPE_PLAYER_STATE);
        NET_WriteInt8(fwd, player);
        NET_WriteInt32(fwd, tick);
        for (j = 0; j < 9; j++) NET_WriteInt32(fwd, (unsigned int)data[j]);
        NET_Conn_SendPacket(&clients[i].connection, fwd);
        NET_FreePacket(fwd);
    }
}

// Forward host-authoritative health broadcast to all other clients

static void NET_SV_ForwardHealthAuth(net_packet_t *packet, net_client_t *sender) {
    unsigned int i;
    unsigned int num_players;
    int data[NET_MAXPLAYERS * 5];
    int j;

    if (!NET_ReadInt8(packet, &num_players)) return;
    if (num_players > NET_MAXPLAYERS) return;
    for (j = 0; j < (int)num_players * 5; j++) {
        if (!NET_ReadSInt32(packet, &data[j])) return;
    }

    for (i = 0; i < MAXNETNODES; ++i) {
        net_packet_t *fwd;
        if (!ClientConnected(&clients[i]) || &clients[i] == sender) continue;

        fwd = NET_NewPacket(4 + num_players * 20);
        NET_WriteInt16(fwd, NET_PACKET_TYPE_HEALTH_AUTH);
        NET_WriteInt8(fwd, num_players);
        for (j = 0; j < (int)num_players * 5; j++) NET_WriteInt32(fwd, (unsigned int)data[j]);
        NET_Conn_SendPacket(&clients[i].connection, fwd);
        NET_FreePacket(fwd);
    }
}

// Handle respawn request from a client — call into the game layer
// to set PST_REBORN on the host's authoritative game state.

extern void D_HandleRespawnRequest(int player);
extern void D_HandleDamageEvent(int source_player, int target_player, int damage);
extern void D_HandleNPCDamageEvent(int source_player, unsigned short target_net_id, int damage);

static void NET_SV_HandleRespawnRequest(net_packet_t *packet, net_client_t *client) {
    unsigned int player;
    if (!NET_ReadInt8(packet, &player)) return;
    if (player >= NET_MAXPLAYERS) return;
    // Only allow respawn for the player this client controls
    if ((int)player != client->player_number) return;
    D_HandleRespawnRequest(player);
}

// Handle damage event from a client — client reports hitting a player.
// Server validates and calls into game layer to apply damage on host.

static void NET_SV_HandleDamageEvent(net_packet_t *packet, net_client_t *client) {
    unsigned int source, target, dmg;
    if (!NET_ReadInt8(packet, &source)) return;
    if (!NET_ReadInt8(packet, &target)) return;
    if (!NET_ReadInt16(packet, &dmg)) return;
    if ((int)source != client->player_number) return;
    if (target >= NET_MAXPLAYERS) return;
    D_HandleDamageEvent(source, target, dmg);
}

// Forward NPC state snapshot from host to all other clients

static void NET_SV_ForwardNPCState(net_packet_t *packet, net_client_t *sender) {
    unsigned char buf[4000];
    int len = 0;
    unsigned int byte_val;
    unsigned int i;

    // Read all remaining bytes
    while (NET_ReadInt8(packet, &byte_val) && len < (int)sizeof(buf)) {
        buf[len++] = (unsigned char)byte_val;
    }

    // Forward to all other connected clients
    for (i = 0; i < MAXNETNODES; ++i) {
        net_packet_t *fwd;
        int j;
        if (!ClientConnected(&clients[i]) || &clients[i] == sender) continue;
        fwd = NET_NewPacket(len + 4);
        NET_WriteInt16(fwd, NET_PACKET_TYPE_NPC_STATE);
        for (j = 0; j < len; j++) NET_WriteInt8(fwd, buf[j]);
        NET_Conn_SendPacket(&clients[i].connection, fwd);
        NET_FreePacket(fwd);
    }
}

// Handle NPC damage event from a client — client reports hitting a monster.
// Server validates and calls into game layer to apply damage on host.

// Handle USE event from a client — client reports pressing USE.
// Server calls into game layer to process interaction on host.
extern void D_HandleUseEvent(int player);
extern void D_HandleChatMessage(int player, const char *msg);
extern void D_HandlePlayerName(int player, const char *name);
extern void D_HandleKillMessage(const char *msg);

// Forward chat message to all other clients + apply on host
static void NET_SV_HandleChatMessage(net_packet_t *packet, net_client_t *client) {
    unsigned int player;
    char *msg;
    unsigned int i;
    if (!NET_ReadInt8(packet, &player)) return;
    if ((int)player != client->player_number) return;
    msg = NET_ReadString(packet);
    if (msg == NULL) return;

    // Apply on host
    D_HandleChatMessage((int)player, msg);

    // Forward to all other clients
    for (i = 0; i < MAXNETNODES; ++i) {
        net_packet_t *fwd;
        if (!ClientConnected(&clients[i]) || &clients[i] == client) continue;
        fwd = NET_NewPacket(4 + (int)strlen(msg));
        NET_WriteInt16(fwd, NET_PACKET_TYPE_CHAT_MSG);
        NET_WriteInt8(fwd, player);
        NET_WriteString(fwd, msg);
        NET_Conn_SendPacket(&clients[i].connection, fwd);
        NET_FreePacket(fwd);
    }
}

// Forward player name to all other clients + apply on host
static void NET_SV_HandlePlayerName(net_packet_t *packet, net_client_t *client) {
    unsigned int player;
    char *name;
    unsigned int i;
    if (!NET_ReadInt8(packet, &player)) return;
    if ((int)player != client->player_number) return;
    name = NET_ReadString(packet);
    if (name == NULL) return;

    // Apply on host
    D_HandlePlayerName((int)player, name);

    // Forward to all other clients
    for (i = 0; i < MAXNETNODES; ++i) {
        net_packet_t *fwd;
        if (!ClientConnected(&clients[i]) || &clients[i] == client) continue;
        fwd = NET_NewPacket(4 + (int)strlen(name));
        NET_WriteInt16(fwd, NET_PACKET_TYPE_PLAYER_NAME);
        NET_WriteInt8(fwd, player);
        NET_WriteString(fwd, name);
        NET_Conn_SendPacket(&clients[i].connection, fwd);
        NET_FreePacket(fwd);
    }
}

// Forward kill message to all other clients + apply on host
static void NET_SV_HandleKillMessage(net_packet_t *packet, net_client_t *client) {
    char *msg;
    unsigned int i;
    msg = NET_ReadString(packet);
    if (msg == NULL) return;

    // Apply on host
    D_HandleKillMessage(msg);

    // Forward to all other clients
    for (i = 0; i < MAXNETNODES; ++i) {
        net_packet_t *fwd;
        if (!ClientConnected(&clients[i]) || &clients[i] == client) continue;
        fwd = NET_NewPacket(4 + (int)strlen(msg));
        NET_WriteInt16(fwd, NET_PACKET_TYPE_KILL_MSG);
        NET_WriteString(fwd, msg);
        NET_Conn_SendPacket(&clients[i].connection, fwd);
        NET_FreePacket(fwd);
    }
}

static void NET_SV_HandleUseEvent(net_packet_t *packet, net_client_t *client) {
    unsigned int player;
    if (!NET_ReadInt8(packet, &player)) return;
    if ((int)player != client->player_number) return;
    D_HandleUseEvent(player);
}

static void NET_SV_HandleNPCDamage(net_packet_t *packet, net_client_t *client) {
    unsigned int source, net_id, dmg;
    if (!NET_ReadInt8(packet, &source)) return;
    if (!NET_ReadInt16(packet, &net_id)) return;
    if (!NET_ReadInt16(packet, &dmg)) return;
    if ((int)source != client->player_number) return;
    D_HandleNPCDamageEvent(source, (unsigned short)net_id, dmg);
}

// Process a packet received by the server

static void NET_SV_Packet(net_packet_t *packet, net_addr_t *addr)
{
    net_client_t *client;
    unsigned int packet_type;

    // Find which client this packet came from

    client = NET_SV_FindClient(addr);

    // Read the packet type

    if (!NET_ReadInt16(packet, &packet_type)) {
        // no packet type

        return;
    }

    NET_Log("server: packet from %s; type %d", NET_AddrToString(addr), packet_type & ~NET_RELIABLE_PACKET);
    NET_LogPacket(packet);

    if (packet_type == NET_PACKET_TYPE_SYN) {
        NET_SV_ParseSYN(packet, client, addr);
    }
    else if (packet_type == NET_PACKET_TYPE_QUERY) {
        NET_SV_SendQueryResponse(addr);
    }
    else if (client == NULL) {
        // Must come from a valid client; ignore otherwise
    }
    else if (NET_Conn_Packet(&client->connection, packet, &packet_type)) {
        // Packet was eaten by the common connection code
    }
    else {
        // printf("SV: %s: %i\n", NET_AddrToString(addr), packet_type);

        switch (packet_type) {
        case NET_PACKET_TYPE_GAMESTART:
            NET_SV_ParseGameStart(packet, client);
            break;
        case NET_PACKET_TYPE_LAUNCH:
            NET_SV_ParseLaunch(packet, client);
            break;
        case NET_PACKET_TYPE_GAMEDATA:
            NET_SV_ParseGameData(packet, client);
            break;
        case NET_PACKET_TYPE_GAMEDATA_ACK:
            NET_SV_ParseGameDataACK(packet, client);
            break;
        case NET_PACKET_TYPE_GAMEDATA_RESEND:
            NET_SV_ParseResendRequest(packet, client);
            break;
        case NET_PACKET_TYPE_PLAYER_STATE:
            NET_SV_ForwardPlayerState(packet, client);
            break;
        case NET_PACKET_TYPE_HEALTH_AUTH:
            NET_SV_ForwardHealthAuth(packet, client);
            break;
        case NET_PACKET_TYPE_RESPAWN_REQUEST:
            NET_SV_HandleRespawnRequest(packet, client);
            break;
        case NET_PACKET_TYPE_DAMAGE_EVENT:
            NET_SV_HandleDamageEvent(packet, client);
            break;
        case NET_PACKET_TYPE_NPC_STATE:
            NET_SV_ForwardNPCState(packet, client);
            break;
        case NET_PACKET_TYPE_NPC_DAMAGE:
            NET_SV_HandleNPCDamage(packet, client);
            break;
        case NET_PACKET_TYPE_USE_EVENT:
            NET_SV_HandleUseEvent(packet, client);
            break;
        case NET_PACKET_TYPE_WORLD_STATE:
            NET_SV_ForwardRawPacket(packet, client, NET_PACKET_TYPE_WORLD_STATE);
            break;
        case NET_PACKET_TYPE_CHAT_MSG:
            NET_SV_HandleChatMessage(packet, client);
            break;
        case NET_PACKET_TYPE_PLAYER_NAME:
            NET_SV_HandlePlayerName(packet, client);
            break;
        case NET_PACKET_TYPE_KILL_MSG:
            NET_SV_HandleKillMessage(packet, client);
            break;
        case NET_PACKET_TYPE_PING:
        {
            unsigned int ts;
            if (NET_ReadInt32(packet, &ts)) {
                net_packet_t *pong = NET_NewPacket(6);
                NET_WriteInt16(pong, NET_PACKET_TYPE_PONG);
                NET_WriteInt32(pong, ts);
                NET_Conn_SendPacket(&client->connection, pong);
                NET_FreePacket(pong);
            }
            break;
        }
        default:
            // unknown packet type

            break;
        }
    }
}

static void NET_SV_PumpSendQueue(net_client_t *client)
{
    net_full_ticcmd_t cmd;
    int recv_index;
    int num_players;
    int i;
    int starttic, endtic;
    boolean target_is_loopback;

    // VectorDoom: check loopback early — remote clients don't receive tics.
    target_is_loopback = (client->addr
        && client->addr->module == &net_loop_server_module);

    // VectorDoom: skip entirely for remote clients. They fabricate tics
    // locally and sync via snapshots. Don't touch sendseq or acknowledged.
    if (!target_is_loopback) {
        return;
    }

    // If a client has not sent any acknowledgments for a while,
    // wait until they catch up.

    if (client->sendseq - NET_SV_LatestAcknowledged() > 200) {
        return;
    }

    // Work out the index into the receive window

    recv_index = client->sendseq - recvwindow_start;

    if (recv_index < 0 || recv_index >= BACKUPTICS) {
        return;
    }

    // Check if we can generate a new entry for the send queue
    // using the data in recvwindow.

    num_players = 0;

    for (i = 0; i < NET_MAXPLAYERS; ++i) {
        if (sv_players[i] == client) {
            // Client does not rely on itself for data

            continue;
        }

        if (sv_players[i] == NULL || !ClientConnected(sv_players[i])) {
            continue;
        }

        // Non-blocking: don't return early if tic is missing.
        // We'll use last-known-input when building the cmd below.

        ++num_players;
    }

    // If this is a game with only a single player in it, we might
    // be sending a ticcmd set containing 0 ticcmds. This is fine;
    // however, there's nothing to stop the game running on ahead
    // and never stopping. Don't let the server get too far ahead
    // of the client.

    if (num_players == 0 && client->sendseq > recvwindow_start + 10) {
        return;
    }

    // We have all data we need to generate a command for this tic.

    cmd.seq = client->sendseq;

    // Add ticcmds from all players

    cmd.latency = 0;

    // target_is_loopback already set at top of function

    for (i = 0; i < NET_MAXPLAYERS; ++i) {
        net_client_recv_t *recvobj;

        if (sv_players[i] == client) {
            // Not the player we are sending to

            cmd.playeringame[i] = false;
            continue;
        }

        if (sv_players[i] == NULL || !ClientConnected(sv_players[i])) {
            cmd.playeringame[i] = false;
            continue;
        }

        cmd.playeringame[i] = true;

        if (recvwindow[recv_index][i].active) {
            // Real data available — use it
            recvobj = &recvwindow[recv_index][i];
            cmd.cmds[i] = recvobj->diff;
            if (recvobj->latency > cmd.latency) cmd.latency = recvobj->latency;
        } else {
            // Data missing for player i
            if (!target_is_loopback && i == sv_loopback_player) {
                // Sending to a remote client, but host data isn't ready yet.
                // Wait — it'll arrive via loopback next frame (~28ms).
                return;
            }
            // Fabricate for remote players (or host slot for loopback client).
            // Zero movement (player stands still) but preserve buttons
            // so attacks (BT_ATTACK) and use (BT_USE) are visible.
            // Snapshots correct position/angle every ~57ms.
            if (sv_player_has_sent[i]) {
                cmd.cmds[i].diff = NET_TICDIFF_FORWARD | NET_TICDIFF_SIDE
                                 | NET_TICDIFF_TURN | NET_TICDIFF_BUTTONS
                                 | NET_TICDIFF_CONSISTANCY;
                memset(&cmd.cmds[i].cmd, 0, sizeof(ticcmd_t));
                cmd.cmds[i].cmd.buttons = sv_last_ticcmd[i].buttons;
            } else {
                memset(&cmd.cmds[i], 0, sizeof(net_ticdiff_t));
            }
        }
    }

    // printf("SV: %i: latency %i\n", client->player_number, cmd.latency);

    // Add into the queue

    client->sendqueue[client->sendseq % BACKUPTICS] = cmd;

    // Transmit the new tic to the loopback client (host)

    starttic = client->sendseq - sv_settings.extratics;
    endtic = client->sendseq;

    if (starttic < 0) starttic = 0;

    NET_SV_SendTics(client, starttic, endtic);

    ++client->sendseq;
}

// Prevent against deadlock: resend requests are usually only
// triggered if we miss a packet and receive the next one.
// If we miss a whole load of packets, we can end up in a
// deadlock situation where the client will not send any more.
// If we don't receive any game data in a while, trigger a resend
// request for the next tic we're expecting.

void NET_SV_CheckDeadlock(net_client_t *client)
{
    int nowtime;
    int i;

    // Don't expect game data from clients.

    if (client->drone) {
        return;
    }

    nowtime = I_GetTimeMS();

    // If we haven't received anything for a long time, it may be a deadlock.

    // VectorDoom: Resend/deadlock detection disabled.
    // Tic transfer is no longer required for gameplay — the snapshot system
    // is the sole authority for remote player state. Flooding the Iroh gossip
    // channel with resend requests and tic backlogs only makes lag worse.
    // The connection timeout (60s) handles genuinely dead connections.
    (void)nowtime;
    (void)i;
}

// Called when all players have disconnected.  Return to listening for
// players to start a new game, and disconnect any drones still connected.

static void NET_SV_GameEnded(void)
{
    int i;

    server_state = SERVER_WAITING_LAUNCH;
    sv_gamemode = indetermined;

    for (i = 0; i < MAXNETNODES; ++i) {
        if (clients[i].active) {
            NET_SV_DisconnectClient(&clients[i]);
        }
    }
}

// Perform any needed action on a client

static void NET_SV_RunClient(net_client_t *client)
{
    // Run common code

    NET_Conn_Run(&client->connection);

    if (client->connection.state == NET_CONN_STATE_DISCONNECTED &&
        client->connection.disconnect_reason == NET_DISCONNECT_TIMEOUT) {
        NET_Log("server: client at %s timed out", NET_AddrToString(client->addr));
        NET_SV_BroadcastMessage("Client '%s' timed out and disconnected", client->name);
        printf("doom: 12, client '%s' timed out and disconnected", client->name);
    }

    // Is this client disconnected?

    if (client->connection.state == NET_CONN_STATE_DISCONNECTED) {
        // VectorDoom: notify the game logic to remove this player.
        // With tic transfer disabled, the ingame flag never goes false
        // through ticcmds, so we must explicitly trigger PlayerQuitGame.
        if (client->player_number >= 0 && client->player_number < NET_MAXPLAYERS) {
            extern void D_PlayerDisconnected(int player);
            D_PlayerDisconnected(client->player_number);
        }

        client->active = false;

        // If we were about to start a game, any player disconnecting
        // should cause an abort.

        if (server_state == SERVER_WAITING_START && !client->drone) {
            NET_SV_BroadcastMessage("Game startup aborted because "
                                    "player '%s' disconnected.",
                                    client->name);
            NET_SV_GameEnded();
        }

        free(client->name);
        NET_ReleaseAddress(client->addr);

        // Are there any clients left connected?  If not, return the
        // server to the waiting-for-players state.
        //
        // Disconnect any drones still connected.

        if (NET_SV_NumPlayers() <= 0) {
            NET_Log("server: no player clients left, game ended");
            NET_SV_GameEnded();
        }
    }

    if (!ClientConnected(client)) {
        // client has not yet finished connecting

        return;
    }

    if (server_state == SERVER_WAITING_LAUNCH) {
        // Waiting for the game to start

        // Send information once every second

        if (client->last_send_time < 0 || I_GetTimeMS() - client->last_send_time > 1000) {
            NET_SV_SendWaitingData(client);
            client->last_send_time = I_GetTimeMS();
        }
    }

    if (server_state == SERVER_IN_GAME) {
        NET_SV_PumpSendQueue(client);
        NET_SV_CheckDeadlock(client);
    }
}

// Add a network module to the server context

void NET_SV_AddModule(net_module_t *module)
{
    module->InitServer();
    NET_AddModule(server_context, module);
}

// VectorDoom: allow d_loop.c to set server state for host migration
void NET_SV_SetState(int state)
{
    server_state = state;
}

boolean NET_SV_IsInitialized(void)
{
    return server_initialized;
}

// Initialize server and wait for connections

void NET_SV_Init(void)
{
    int i;

    // initialize send/receive context

    server_context = NET_NewContext();

    // no clients yet

    for (i = 0; i < MAXNETNODES; ++i) {
        clients[i].active = false;
    }

    NET_SV_AssignPlayers();

    server_state = SERVER_WAITING_LAUNCH;
    sv_gamemode = indetermined;
    server_initialized = true;
}

// Run server code to check for new packets/send packets as the server
// requires

// net_addr_t
//
// struct _net_addr_s
// {
//     net_module_t *module;
//     int refcount;
//     void *handle;
// };

void NET_SV_Run(void)
{
    net_addr_t *addr;
    net_packet_t *packet;
    int i;

    if (!server_initialized) {
        return;
    }

    // printf("net_server.c :: while(NET_RecvPacket())\n");

    while (NET_RecvPacket(server_context, &addr, &packet)) {
        printf("doom: SV got packet len=%d from %s\n", packet->len, NET_AddrToString(addr));
        NET_SV_Packet(packet, addr);
        // printf("net_server.c :: NET_FreePacket()\n");
        NET_FreePacket(packet);
        NET_ReleaseAddress(addr);
    }

    // "Run" any clients that may have things to do, independent of responses
    // to received packets

    for (i = 0; i < MAXNETNODES; ++i) {
        if (clients[i].active) {
            // printf("net_server.c :: NET_SV_RunClient()\n");
            NET_SV_RunClient(&clients[i]);
        }
    }

    switch (server_state) {
    case SERVER_WAITING_LAUNCH:
        // printf("net_server.c :: SERVER_WAITING_LAUNCH\n");
        break;

    case SERVER_WAITING_START:
        // printf("net_server.c :: CheckStartGame()\n");
        CheckStartGame();
        break;

    case SERVER_IN_GAME:
        NET_SV_AdvanceWindow();

        for (i = 0; i < NET_MAXPLAYERS; ++i) {
            // printf("net_server.c :: ClientConnected()\n");
            if (sv_players[i] != NULL && ClientConnected(sv_players[i])) {
                // printf("net_server.c :: NET_SV_CheckResends()\n");
                NET_SV_CheckResends(sv_players[i]);
            }
        }
        break;
    }
}

void NET_SV_Shutdown(void)
{
    int i;
    boolean running;
    int start_time;

    if (!server_initialized) {
        return;
    }

    fprintf(stderr, "SV: Shutting down server...\n");

    // Disconnect all clients

    for (i = 0; i < MAXNETNODES; ++i) {
        if (clients[i].active) {
            NET_SV_DisconnectClient(&clients[i]);
        }
    }

    // Wait for all clients to finish disconnecting

    start_time = I_GetTimeMS();
    running = true;

    while (running) {
        // Check if any clients are still not finished

        running = false;

        for (i = 0; i < MAXNETNODES; ++i) {
            if (clients[i].active) {
                running = true;
            }
        }

        // Timed out?

        if (I_GetTimeMS() - start_time > 5000) {
            running = false;
            fprintf(stderr, "SV: Timed out waiting for clients to disconnect.\n");
        }

        // Run the client code in case this is a loopback client.

        NET_CL_Run();
        NET_SV_Run();

        // Don't hog the CPU

        I_Sleep(1);
    }
}
