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
//	DOOM Network game communication and protocol,
//	all OS independend parts.
//

#include <stdlib.h>

#include "d_main.h"
#include "debug.h"
#include "doomdef.h"
#include "doomstat.h"
#include "g_game.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_menu.h"
#include "m_misc.h"
#include "w_checksum.h"
#include "w_wad.h"

#include "deh_main.h"

#include "d_loop.h"
#include "hu_stuff.h"
#include "net_client.h"
#include "p_local.h"
#include "p_netsync.h"
#include "s_sound.h"
#include "sounds.h"

// P_KillMobj and damage_from_event are defined in p_inter.c
void P_KillMobj(mobj_t *source, mobj_t *target);
void P_DamageMobj(mobj_t *target, mobj_t *inflictor, mobj_t *source, int damage);
extern boolean damage_from_event;

ticcmd_t *netcmds;

// Called when a player leaves the game

void P_RemoveMobj(mobj_t *mobj);

// Declared in d_loop.c — we need to flip these for host migration
extern boolean net_client_player;
void D_PromoteToHost(void);

static void PlayerQuitGame(player_t *player)
{
    static char exitmsg[80];
    unsigned int player_num;

    player_num = player - players;

    // Do this the same way as Vanilla Doom does, to allow dehacked
    // replacements of this message

    M_StringCopy(exitmsg, DEH_String("Player 1 left the game"), sizeof(exitmsg));

    exitmsg[7] += player_num;

    playeringame[player_num] = false;
    players[consoleplayer].message = exitmsg;

    // Remove the player's mobj to avoid ghost bodies.
    if (player->mo) {
        player->mo->player = NULL;
        P_RemoveMobj(player->mo);
        player->mo = NULL;
    }

    // VectorDoom: Host migration — if the host left and we're a client,
    // promote ourselves to host. NPCs resume local physics, damage applies
    // locally, doors/switches work. Game continues seamlessly.
    if (net_client_player && player_num == 0) {
        D_PromoteToHost();
        players[consoleplayer].message = "Host left. You are now the host.";
    }

    if (demorecording) {
        G_CheckDemoStatus();
    }
}

static void RunTic(ticcmd_t *cmds, boolean *ingame)
{
    extern boolean advancedemo;
    unsigned int i;

    // Check for player joins and quits (driven by tic data from server).

    for (i = 0; i < MAXPLAYERS; ++i) {
        if (!demoplayback && !playeringame[i] && ingame[i]) {
            // New player appeared in tic data — spawn them
            playeringame[i] = true;
            players[i].playerstate = PST_REBORN;
            printf("RunTic: player %d joined\n", i);
        }
        if (!demoplayback && playeringame[i] && !ingame[i]) {
            PlayerQuitGame(&players[i]);
        }
    }

    netcmds = cmds;

    // check that there are players in the game.  if not, we cannot
    // run a tic.

    if (advancedemo) D_DoAdvanceDemo();

    G_Ticker();
}

// Per-player last received attack flag (from their actual snapshot, not lock-modified state).
// Used by BuildPlayerSnapshot for remote players to avoid the lock feeding back into broadcasts.
static int last_received_attack[MAXPLAYERS];

// --- Player movement interpolation ---
// Instead of teleporting remote players to snapshot positions, we use
// exponential smoothing: each tic, move 60% of the remaining error toward
// the target. The target is extrapolated forward using momentum so the
// mobj tracks where the player probably IS, not where they WERE.
//
// This eliminates the 57ms teleport jerk of the old system while keeping
// movement responsive. Physics (P_XYMovement/P_ZMovement) is skipped for
// remote players since interpolation drives their position directly.

#define INTERP_FRAC  16384  // ~25% of FRACUNIT (0.25 * 65536)
// 25% per tic = natural ease-out: fast when far, slows as it approaches.
// Converges in ~8-10 tics (~230-285ms) — smooth even at 500ms+ ping.
#define TELEPORT_THRESH (128 * FRACUNIT)  // instant teleport above this

typedef struct {
    fixed_t target_x, target_y, target_z;
    angle_t target_angle;
    fixed_t momx, momy, momz;
    boolean active;
} player_interp_t;

static player_interp_t player_interp[MAXPLAYERS];

// Player snapshot (8 ints: x, y, z, angle, momx, momy, momz, attack_weapon)
// attack_weapon: 0 = not attacking, 1-9 = attacking with weapon (readyweapon+1)
static void BuildPlayerSnapshot(int player, int *data) {
    mobj_t *mo = players[player].mo;
    if (!mo) { memset(data, 0, 8 * sizeof(int)); return; }
    data[0] = mo->x;     data[1] = mo->y;     data[2] = mo->z;
    data[3] = mo->angle;
    data[4] = mo->momx;  data[5] = mo->momy;  data[6] = mo->momz;
    // Local player: read actual mobj state.
    // Remote players: relay their last received flag, NOT the lock-modified mobj state.
    if (player == consoleplayer) {
        data[7] = (mo->state == &states[S_PLAY_ATK1]
                || mo->state == &states[S_PLAY_ATK2])
                ? (int)players[player].readyweapon + 1 : 0;
    } else {
        data[7] = last_received_attack[player];
    }
    data[8] = NET_CL_GetLatency();
}

// Attack animation lock: prevents local logic from overriding the
// attack animation for ~250ms after it's triggered by a snapshot.
#define ATTACK_LOCK_TICS 8      // minimum tics between new attack triggers
#define ATTACK_STUCK_TICS 35    // force idle if stuck in attack for 1 second
static int remote_attack_tics[MAXPLAYERS];
static int remote_attack_stuck[MAXPLAYERS];  // counts how long stuck in attack

// Weapon fire sounds indexed by weapontype_t
static const int weapon_fire_sfx[] = {
    sfx_None,    // wp_fist
    sfx_pistol,  // wp_pistol
    sfx_shotgn,  // wp_shotgun
    sfx_pistol,  // wp_chaingun
    sfx_rlaunc,  // wp_missile
    sfx_plasma,  // wp_plasma
    sfx_bfg,     // wp_bfg
    sfx_None,    // wp_chainsaw
    sfx_shotgn,  // wp_supershotgun (DOOM2, fallback)
};

// Apply player snapshot: sets interpolation target instead of teleporting.
// The actual movement happens in D_TickPlayerInterp() each tic.
static void ApplyPlayerSnapshot(int player, int *data) {
    mobj_t *mo = players[player].mo;
    player_interp_t *interp = &player_interp[player];
    if (!mo || !playeringame[player]) return;

    // Don't update dead/respawning players via position snapshot
    if (players[player].playerstate != PST_LIVE) return;

    // Store the actual received attack flag for relay (breaks feedback loop)
    last_received_attack[player] = data[7];

    // Store remote player's self-reported ping
    player_pings[player] = data[8];

    // First snapshot or respawn: instant snap to position
    if (!interp->active) {
        P_UnsetThingPosition(mo);
        mo->x = data[0];  mo->y = data[1];  mo->z = data[2];
        P_SetThingPosition(mo);
        mo->angle = (unsigned int)data[3];
    }

    // Update interpolation target — D_TickPlayerInterp handles all movement.
    // Never teleport on regular snapshots; let smoothing close the gap.
    interp->target_x = data[0];
    interp->target_y = data[1];
    interp->target_z = data[2];
    interp->target_angle = (angle_t)data[3];
    interp->momx = data[4];
    interp->momy = data[5];
    interp->momz = data[6];
    interp->active = true;

    // Update mobj momentum (drives walk/idle animation state machine)
    mo->momx = data[4];  mo->momy = data[5];  mo->momz = data[6];

    // Detect attack from snapshot — trigger animation + sound + lock.
    // Lock countdown happens in D_TickPlayerInterp (every tic), not here.
    if (data[7] > 0) {
        if (remote_attack_tics[player] <= 0) {
            int weapon = data[7] - 1;
            // Start at ATK2 (muzzle flash, fullbright) → chains to ATK1 → PLAY.
            // Starting at ATK1 skips the flash entirely (ATK1 → PLAY directly).
            P_SetMobjState(mo, S_PLAY_ATK2);
            if (weapon >= 0 && weapon < (int)(sizeof(weapon_fire_sfx)/sizeof(weapon_fire_sfx[0]))
                && weapon_fire_sfx[weapon] != sfx_None)
                S_StartSound(mo, weapon_fire_sfx[weapon]);
        }
        remote_attack_tics[player] = ATTACK_LOCK_TICS;
    }
}

// Called every tic from P_MobjThinker for remote players.
// Smoothly moves the mobj toward the interpolation target using
// exponential smoothing, with momentum-based extrapolation.
void D_TickPlayerInterp(int player, mobj_t *mo)
{
    player_interp_t *interp = &player_interp[player];
    fixed_t dx, dy, dz;

    if (!interp->active) return;

    // Extrapolate target forward using momentum (predicts where
    // the player probably IS now, not where the snapshot said they WERE)
    interp->target_x += interp->momx;
    interp->target_y += interp->momy;
    // Don't extrapolate Z: gravity/floors make it unreliable

    // Exponential smoothing: each tic, close 60% of the gap
    dx = interp->target_x - mo->x;
    dy = interp->target_y - mo->y;
    dz = interp->target_z - mo->z;

    P_UnsetThingPosition(mo);
    mo->x += FixedMul(dx, INTERP_FRAC);
    mo->y += FixedMul(dy, INTERP_FRAC);
    P_SetThingPosition(mo);

    mo->z += FixedMul(dz, INTERP_FRAC);

    // VectorDoom: Angle IS interpolated from snapshots now.
    // Ticcmd-based rotation is zeroed for remote players, so there's no
    // competing system. Snapshots are the sole authority for remote angle.
    // Use shift-based smoothing to avoid int32 overflow on angle_diff * N.
    // angle_t is unsigned, subtraction wraps correctly for shortest path,
    // casting to int gives signed delta. Shift right by 2 = 25% smoothing
    // (matches position interp — natural ease-out for turning too).
    mo->angle += (angle_t)((int)(interp->target_angle - mo->angle) >> 2);

    // Movement animation: set run/idle based on momentum.
    // P_MovePlayer is skipped for remote players, so we drive it here.
    // Don't override attack animations (ATK1/ATK2) or death/pain.
    {
        boolean moving = (interp->momx != 0 || interp->momy != 0);
        boolean in_idle = (mo->state == &states[S_PLAY]);
        boolean in_run = (mo->state >= &states[S_PLAY_RUN1]
                       && mo->state <= &states[S_PLAY_RUN4]);

        if (moving && in_idle) {
            P_SetMobjStateNoAction(mo, S_PLAY_RUN1);
        } else if (!moving && in_run) {
            P_SetMobjStateNoAction(mo, S_PLAY);
        }
    }

    // Attack lock countdown (prevents retrigger spam)
    if (remote_attack_tics[player] > 0) {
        remote_attack_tics[player]--;
    }

    // Safety: force idle if stuck in attack state for too long (1 second).
    // Normal flow: DOOM state machine transitions ATK1 → ATK2 → PLAY on its own.
    // This only fires if the state machine gets stuck (e.g. lag, desync).
    if (mo->state == &states[S_PLAY_ATK1] || mo->state == &states[S_PLAY_ATK2]) {
        remote_attack_stuck[player]++;
        if (remote_attack_stuck[player] > ATTACK_STUCK_TICS) {
            P_SetMobjStateNoAction(mo, S_PLAY);
            remote_attack_stuck[player] = 0;
        }
    } else {
        remote_attack_stuck[player] = 0;
    }
}

// Host-authoritative health: host broadcasts all players' health + state.
// Clients apply this as ground truth — the host decides who lives and dies.

static void BuildHealthAuth(int *data) {
    unsigned int i;
    for (i = 0; i < MAXPLAYERS; i++) {
        if (playeringame[i] && players[i].mo) {
            data[i * 5]     = players[i].health;
            data[i * 5 + 1] = players[i].playerstate;
            data[i * 5 + 2] = players[i].killcount;
            data[i * 5 + 3] = players[i].itemcount;
            data[i * 5 + 4] = players[i].secretcount;
        } else {
            data[i * 5]     = 0;
            data[i * 5 + 1] = 0;
            data[i * 5 + 2] = 0;
            data[i * 5 + 3] = 0;
            data[i * 5 + 4] = 0;
        }
    }
}

static void ApplyHealthAuth(int localplayer, int *data) {
    unsigned int i;
    for (i = 0; i < MAXPLAYERS; i++) {
        int auth_health = data[i * 5];
        int auth_state  = data[i * 5 + 1];
        mobj_t *mo;

        if (!playeringame[i]) continue;

        // Sync scores for all players (host is authoritative)
        players[i].killcount   = data[i * 5 + 2];
        players[i].itemcount   = data[i * 5 + 3];
        players[i].secretcount = data[i * 5 + 4];

        mo = players[i].mo;
        if (!mo) continue;

        // Host says player is dead, but locally they're alive: kill them
        if (auth_state == PST_DEAD && players[i].playerstate == PST_LIVE) {
            players[i].health = mo->health = auth_health;
            P_KillMobj(NULL, mo);
            continue;
        }

        // Host says player is alive, but locally they're dead: respawn
        if (auth_state == PST_LIVE && players[i].playerstate == PST_DEAD) {
            players[i].playerstate = PST_REBORN;
            continue;
        }

        // Both agree player is alive: sync health directly
        if (auth_state == PST_LIVE && players[i].playerstate == PST_LIVE) {
            // Pain animation when health decreases (blood spatter)
            if (auth_health < players[i].health && auth_health > 0) {
                P_SetMobjState(mo, mo->info->painstate);
            }
            players[i].health = mo->health = auth_health;
        }
    }
}

// Host receives respawn request: set the player to PST_REBORN
static void HandleRespawnRequest(int player) {
    if (player < 0 || player >= MAXPLAYERS) return;
    if (!playeringame[player]) return;
    if (players[player].playerstate != PST_DEAD) return;
    players[player].playerstate = PST_REBORN;
}

// Host receives damage event: a client reports hitting a player.
// Apply damage through P_DamageMobj with the damage_from_event flag
// so the host-side remote-source skip is bypassed.
static void HandleDamageEvent(int source_player, int target_player, int damage) {
    mobj_t *source_mo, *target_mo;
    if (source_player < 0 || source_player >= MAXPLAYERS) return;
    if (target_player < 0 || target_player >= MAXPLAYERS) return;
    if (!playeringame[source_player] || !playeringame[target_player]) return;
    source_mo = players[source_player].mo;
    target_mo = players[target_player].mo;
    if (!source_mo || !target_mo) return;
    if (players[target_player].playerstate != PST_LIVE) return;

    // Show attack animation for the source player on the host
    if (players[source_player].playerstate == PST_LIVE)
        P_SetMobjState(source_mo, S_PLAY_ATK1);

    damage_from_event = true;
    P_DamageMobj(target_mo, source_mo, source_mo, damage);
    damage_from_event = false;
}

// Host receives NPC damage event: a client reports hitting a monster.
// Look up the target by net_id and apply damage on the host.
static void HandleNPCDamageEvent(int source_player, unsigned short target_net_id, int damage) {
    mobj_t *source_mo, *target_mo;
    if (source_player < 0 || source_player >= MAXPLAYERS) return;
    if (!playeringame[source_player]) return;
    source_mo = players[source_player].mo;
    if (!source_mo) return;

    target_mo = P_NetLookup(target_net_id);
    if (!target_mo) return;
    if (!(target_mo->flags & MF_SHOOTABLE)) return;

    P_DamageMobj(target_mo, source_mo, source_mo, damage);
}

// Host receives USE event: a client reports pressing USE.
// Call P_UseLines so the host processes the interaction (doors, switches).
static void HandleUseEvent(int player) {
    if (player < 0 || player >= MAXPLAYERS) return;
    if (!playeringame[player]) return;
    if (players[player].playerstate != PST_LIVE) return;
    if (!players[player].mo) return;
    P_UseLines(&players[player]);
}

// A new player joined mid-game (late join).
// Don't set playeringame here — RunTic handles it when tic data arrives
// with ingame[player]=true, avoiding a race with old tics that could
// trigger PlayerQuitGame before new tic data arrives.
extern char *net_player_name;
extern void D_SendPlayerName(int player, const char *name);

static void HandlePlayerJoined(int player) {
    if (player < 0 || player >= MAXPLAYERS) return;
    printf("Player %d joined mid-game\n", player);

    // Re-broadcast our name so the late joiner learns it.
    // The original name broadcast happened at game start, before they joined.
    if (net_player_name && net_player_name[0]) {
        D_SendPlayerName(consoleplayer, net_player_name);
    }
}

static loop_interface_t doom_loop_interface = {
    D_ProcessEvents, G_BuildTiccmd, RunTic, M_Ticker,
    BuildPlayerSnapshot, ApplyPlayerSnapshot,
    BuildHealthAuth, ApplyHealthAuth,
    HandleRespawnRequest, HandleDamageEvent,
    HandleNPCDamageEvent, HandleUseEvent,
    HandlePlayerJoined
};

// Load game settings from the specified structure and
// set global variables.

static void LoadGameSettings(net_gamesettings_t *settings)
{
    unsigned int i;

    deathmatch = settings->deathmatch;
    startepisode = settings->episode;
    startmap = settings->map;
    startskill = settings->skill;
    startloadgame = settings->loadgame;
    lowres_turn = settings->lowres_turn;
    nomonsters = settings->nomonsters;
    fastparm = settings->fast_monsters;
    respawnparm = settings->respawn_monsters;
    timelimit = settings->timelimit;
    consoleplayer = settings->consoleplayer;

    if (lowres_turn) {
        printf("NOTE: Turning resolution is reduced; this is probably "
               "because there is a client recording a Vanilla demo.\n");
    }

    for (i = 0; i < MAXPLAYERS; ++i) {
        playeringame[i] = i < settings->num_players;
    }
}

// Save the game settings from global variables to the specified
// game settings structure.

static void SaveGameSettings(net_gamesettings_t *settings)
{
    // Fill in game settings structure with appropriate parameters
    // for the new game

    settings->deathmatch = deathmatch;
    settings->episode = startepisode;
    settings->map = startmap;
    settings->skill = startskill;
    settings->loadgame = startloadgame;
    settings->gameversion = gameversion;
    settings->nomonsters = nomonsters;
    settings->fast_monsters = fastparm;
    settings->respawn_monsters = respawnparm;
    settings->timelimit = timelimit;

    settings->lowres_turn = (M_ParmExists("-record") && !M_ParmExists("-longtics")) || M_ParmExists("-shorttics");
}

static void InitConnectData(net_connect_data_t *connect_data)
{
    boolean shorttics;

    connect_data->max_players = MAXPLAYERS;
    connect_data->drone = false;

    //!
    // @category net
    //
    // Run as the left screen in three screen mode.
    //

    if (M_CheckParm("-left") > 0) {
        viewangleoffset = ANG90;
        connect_data->drone = true;
    }

    //!
    // @category net
    //
    // Run as the right screen in three screen mode.
    //

    if (M_CheckParm("-right") > 0) {
        viewangleoffset = ANG270;
        connect_data->drone = true;
    }

    //
    // Connect data
    //

    // Game type fields:

    connect_data->gamemode = gamemode;
    connect_data->gamemission = gamemission;

    //!
    // @category demo
    //
    // Play with low turning resolution to emulate demo recording.
    //

    shorttics = M_ParmExists("-shorttics");

    // Are we recording a demo? Possibly set lowres turn mode

    connect_data->lowres_turn = (M_ParmExists("-record") && !M_ParmExists("-longtics")) || shorttics;

    // Read checksums of our WAD directory and dehacked information

    W_Checksum(connect_data->wad_sha1sum);
    DEH_Checksum(connect_data->deh_sha1sum);

    // Are we playing with the Freedoom IWAD?

    connect_data->is_freedoom = W_CheckNumForName("FREEDOOM") >= 0;
}

void D_ConnectNetGame(void)
{
    net_connect_data_t connect_data;

    InitConnectData(&connect_data);
    netgame = D_InitNetGame(&connect_data);

    //!
    // @category net
    //
    // Start the game playing as though in a netgame with a single
    // player.  This can also be used to play back single player netgame
    // demos.
    //

    if (M_CheckParm("-solo-net") > 0) {
        netgame = true;
    }
}

//
// D_CheckNetGame
// Works out player numbers among the net participants
//
void D_CheckNetGame(void)
{
    net_gamesettings_t settings;

    if (netgame) {
        autostart = true;
    }

    D_RegisterLoopCallbacks(&doom_loop_interface);

    SaveGameSettings(&settings);
    D_StartNetGame(&settings, NULL);
    LoadGameSettings(&settings);

    DEH_printf("startskill %i  deathmatch: %i  startmap: %i  startepisode: %i\n", startskill, deathmatch, startmap,
               startepisode);

    DEH_printf("player %i of %i (%i nodes)\n", consoleplayer + 1, settings.num_players, settings.num_players);

    // Show players here; the server might have specified a time limit

    if (timelimit > 0 && deathmatch) {
        // Gross hack to work like Vanilla:

        if (timelimit == 20 && M_CheckParm("-avg")) {
            DEH_printf("Austin Virtual Gaming: Levels will end "
                       "after 20 minutes\n");
        }
        else {
            DEH_printf("Levels will end after %d minute", timelimit);
            if (timelimit > 1) printf("s");
            printf(".\n");
        }
    }
    printf("doom: 10, game started\n");

    // Broadcast our player name to other players
    if (netgame) {
        extern char *net_player_name;
        if (net_player_name && net_player_name[0]) {
            extern void HU_SetPlayerName(int player, const char *name);
            HU_SetPlayerName(consoleplayer, net_player_name);
            D_SendPlayerName(consoleplayer, net_player_name);
        }
    }
}

