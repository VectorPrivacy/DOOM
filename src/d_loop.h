//
// Copyright(C) 1993-1996 Id Software, Inc.
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
// DESCRIPTION:
//	Main loop stuff.
//

#ifndef __D_LOOP__
#define __D_LOOP__

#include "net_defs.h"

// Callback function invoked while waiting for the netgame to start.
// The callback is invoked when new players are ready. The callback
// should return true, or return false to abort startup.

typedef boolean (*netgame_startup_callback_t)(int ready_players,
                                              int num_players);

typedef struct
{
    // Read events from the event queue, and process them.

    void (*ProcessEvents)();

    // Given the current input state, fill in the fields of the specified
    // ticcmd_t structure with data for a new tic.

    void (*BuildTiccmd)(ticcmd_t *cmd, int maketic);

    // Advance the game forward one tic, using the specified player input.

    void (*RunTic)(ticcmd_t *cmds, boolean *ingame);

    // Run the menu (runs independently of the game).

    void (*RunMenu)();

    // Position snapshot callbacks for desync correction (7 ints: x,y,z,angle,momx,momy,momz)
    void (*BuildPlayerSnapshot)(int player, int *data);
    void (*ApplyPlayerSnapshot)(int player, int *data);

    // Host-authoritative health broadcast (MAXPLAYERS * 2 ints: health + playerstate per player)
    void (*BuildHealthAuth)(int *data);
    void (*ApplyHealthAuth)(int localplayer, int *data);

    // Host handles a respawn request from a client
    void (*HandleRespawnRequest)(int player);

    // Host handles a damage event from a client
    void (*HandleDamageEvent)(int source_player, int target_player, int damage);

    // Host handles an NPC damage event from a client
    void (*HandleNPCDamageEvent)(int source_player, unsigned short target_net_id, int damage);

    // Host handles a USE event from a client (door/switch interaction)
    void (*HandleUseEvent)(int player);

    // A new player joined mid-game (late join)
    void (*PlayerJoined)(int player);
} loop_interface_t;

// Register callback functions for the main loop code to use.
void D_RegisterLoopCallbacks(loop_interface_t *i);

// Create any new ticcmds and broadcast to other players.
void NetUpdate (void);

// Broadcasts special packets to other players
//  to notify of game exit
void D_QuitNetGame (void);

//? how many ticks to run?
void TryRunTics (void);

// Called at start of game loop to initialize timers
void D_StartGameLoop(void);

// Initialize networking code and connect to server.

boolean D_InitNetGame(net_connect_data_t *connect_data);

// Start game with specified settings. The structure will be updated
// with the actual settings for the game.

void D_StartNetGame(net_gamesettings_t *settings,
                    netgame_startup_callback_t callback);

extern boolean singletics;
extern int gametic, ticdup;

// Position snapshot bridge functions
void D_BuildAndSendSnapshot(void);
void D_ApplyPlayerSnapshot(int player, int gametic_snap, int *data);

// Host-authoritative health broadcast
void D_BuildAndSendHealthAuth(void);
void D_ApplyHealthAuth(int *data);

// Respawn request handling
void D_HandleRespawnRequest(int player);
void D_SendRespawnRequest(void);

// Client damage event: client reports hitting a player to the host
void D_SendDamageEvent(int source_player, int target_player, int damage);
void D_HandleDamageEvent(int source_player, int target_player, int damage);

// NPC sync: host broadcasts state, clients apply; clients report NPC hits
void D_BuildAndSendNPCState(void);
void D_ApplyNPCState(unsigned char *data, int len);
void D_SendNPCDamageEvent(int source_player, unsigned short target_net_id, int damage);
void D_HandleNPCDamageEvent(int source_player, unsigned short target_net_id, int damage);

// USE event: client reports pressing USE to host (doors, switches)
void D_SendUseEvent(int player);
void D_HandleUseEvent(int player);

// Chat messages: bypass ticcmd system, sent as full messages
void D_SendChatMessage(int player, const char *msg);
void D_HandleChatMessage(int player, const char *msg);

// Kill messages: host broadcasts kill notifications
void D_SendKillMessage(const char *msg);
void D_HandleKillMessage(const char *msg);

// Player name exchange: broadcast names after game starts
void D_SendPlayerName(int player, const char *name);
void D_HandlePlayerName(int player, const char *name);

// Mid-game join: activate a new player during a running game
void D_PlayerJoinedMidGame(int player);

// Check if it is permitted to record a demo with a non-vanilla feature.
boolean D_NonVanillaRecord(boolean conditional, const char *feature);

// Check if it is permitted to play back a demo with a non-vanilla feature.
boolean D_NonVanillaPlayback(boolean conditional, int lumpnum,
                             const char *feature);

void D_GameLoop();
#endif

