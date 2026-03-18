//
// Copyright(C) 2024 VectorDoom contributors
//
// Network synchronization for NPCs (monsters) and sectors.
// Host builds snapshots, clients apply them.
//

#include <string.h>

#include "doomdef.h"
#include "doomstat.h"
#include "i_system.h"
#include "p_local.h"
#include "p_netsync.h"
#include "r_state.h"
#include "s_sound.h"
#include "info.h"

// Net ID registry
mobj_t *net_mobj_table[MAX_NET_MOBJS];
unsigned short net_id_counter;

// Initial sector heights (stored at level load for delta detection)
static fixed_t sector_init_floor[MAX_SYNC_SECTORS];
static fixed_t sector_init_ceiling[MAX_SYNC_SECTORS];

// Pending missile spawns queued by host for next snapshot
typedef struct {
    unsigned short source_net_id;
    mobjtype_t type;
    fixed_t x, y, z;
    fixed_t momx, momy, momz;
    angle_t angle;
} pending_missile_t;

static pending_missile_t pending_missiles[MAX_PENDING_MISSILES];
static int pending_missile_count = 0;

void P_NetAssignId(mobj_t *mobj)
{
    net_id_counter++;
    if (net_id_counter >= MAX_NET_MOBJS)
    {
        mobj->net_id = 0;
        return;
    }
    mobj->net_id = net_id_counter;
    net_mobj_table[net_id_counter] = mobj;
}

mobj_t *P_NetLookup(unsigned short net_id)
{
    if (net_id == 0 || net_id >= MAX_NET_MOBJS)
        return NULL;
    return net_mobj_table[net_id];
}

void P_NetResetIds(void)
{
    memset(net_mobj_table, 0, sizeof(net_mobj_table));
    net_id_counter = 0;
}

//
// P_NetStoreSectorHeights
// Store initial ceiling/floor heights for all sectors.
// Called after P_LoadSectors so we can detect changes later.
//
void P_NetStoreSectorHeights(void)
{
    int i;
    int max = numsectors < MAX_SYNC_SECTORS ? numsectors : MAX_SYNC_SECTORS;
    memset(sector_init_floor, 0, sizeof(sector_init_floor));
    memset(sector_init_ceiling, 0, sizeof(sector_init_ceiling));
    for (i = 0; i < max; i++)
    {
        sector_init_floor[i] = sectors[i].floorheight;
        sector_init_ceiling[i] = sectors[i].ceilingheight;
    }
}

void P_NetRemoveId(mobj_t *mobj)
{
    if (mobj->net_id > 0 && mobj->net_id < MAX_NET_MOBJS)
    {
        net_mobj_table[mobj->net_id] = NULL;
    }
}

void P_NetQueueMissile(mobj_t *missile, unsigned short source_net_id)
{
    pending_missile_t *pm;
    if (pending_missile_count >= MAX_PENDING_MISSILES)
        return;
    pm = &pending_missiles[pending_missile_count++];
    pm->source_net_id = source_net_id;
    pm->type = missile->type;
    pm->x = missile->x;
    pm->y = missile->y;
    pm->z = missile->z;
    pm->momx = missile->momx;
    pm->momy = missile->momy;
    pm->momz = missile->momz;
    pm->angle = missile->angle;
}

//
// P_SetMobjStateNoAction
// Same as P_SetMobjState but does NOT call action functions.
// Used on clients to set visual state without side effects
// (no projectile spawning, no AI decisions, no sounds).
//
boolean P_SetMobjStateNoAction(mobj_t *mobj, statenum_t state)
{
    state_t *st;
    int cycle_counter = 0;

    do
    {
        if (state == S_NULL)
        {
            mobj->state = (state_t *) S_NULL;
            P_RemoveMobj(mobj);
            return false;
        }

        st = &states[state];
        mobj->state = st;
        mobj->tics = st->tics;
        mobj->sprite = st->sprite;
        mobj->frame = st->frame;

        // NO action function call
        state = st->nextstate;

        if (cycle_counter++ > 1000000)
        {
            I_Error("P_SetMobjStateNoAction: Infinite state cycle!");
        }
    } while (!mobj->tics);

    return true;
}

//
// P_BuildNPCSnapshot
// Host builds a compact snapshot of all alive synced NPCs,
// followed by changed sectors and pending missile spawns.
// Returns number of bytes written to buf.
//
// Format:
//   [npc_count: u8][gametic: u16]
//   Per NPC (20 bytes):
//     net_id(2) x(4) y(4) z(4) angle(1) statenum(2) health(2) flags(1)
//   [sector_count: u8]
//   Per changed sector (10 bytes):
//     sector_index(2) floorheight(4) ceilingheight(4)
//   [missile_count: u8]
//   Per missile (32 bytes):
//     source_net_id(2) type(2) x(4) y(4) z(4) momx(4) momy(4) momz(4) angle(4)
//
int P_BuildNPCSnapshot(unsigned char *buf, int maxlen)
{
    int offset = 0;
    int npc_count = 0;
    int npc_count_pos;
    int sector_count = 0;
    int sector_count_pos;
    unsigned short i;
    int max_sectors;

    if (maxlen < 5)
        return 0;

    // Reserve space for NPC count byte
    npc_count_pos = offset;
    offset++;

    // gametic low 16 bits
    buf[offset++] = gametic & 0xff;
    buf[offset++] = (gametic >> 8) & 0xff;

    // Write NPC data
    for (i = 1; i <= net_id_counter && i < MAX_NET_MOBJS; i++)
    {
        mobj_t *mo = net_mobj_table[i];
        unsigned short sn;
        short h;
        unsigned char sf;

        if (!mo)
            continue;

        if (offset + 20 > maxlen - 1)  // leave room for sector_count byte
            break;

        // net_id (2 bytes LE)
        buf[offset++] = i & 0xff;
        buf[offset++] = (i >> 8) & 0xff;

        // x (4 bytes)
        memcpy(buf + offset, &mo->x, 4);
        offset += 4;

        // y (4 bytes)
        memcpy(buf + offset, &mo->y, 4);
        offset += 4;

        // z (4 bytes)
        memcpy(buf + offset, &mo->z, 4);
        offset += 4;

        // angle (1 byte: top 8 bits)
        buf[offset++] = (unsigned char)(mo->angle >> 24);

        // statenum (2 bytes)
        sn = (unsigned short)(mo->state - states);
        memcpy(buf + offset, &sn, 2);
        offset += 2;

        // health (2 bytes)
        h = (short)(mo->health > 32767 ? 32767 : (mo->health < -32768 ? -32768 : mo->health));
        memcpy(buf + offset, &h, 2);
        offset += 2;

        // sync_flags (1 byte)
        sf = 0;
        if (mo->flags & MF_SHOOTABLE) sf |= 1;
        if (mo->flags & MF_CORPSE)    sf |= 2;
        if (mo->flags & MF_SOLID)     sf |= 4;
        buf[offset++] = sf;

        npc_count++;
    }

    buf[npc_count_pos] = (unsigned char)npc_count;

    // Write sector data (changed sectors only)
    sector_count_pos = offset;
    offset++;  // reserve for sector count

    max_sectors = numsectors < MAX_SYNC_SECTORS ? numsectors : MAX_SYNC_SECTORS;
    for (i = 0; i < max_sectors; i++)
    {
        if (sectors[i].floorheight != sector_init_floor[i]
            || sectors[i].ceilingheight != sector_init_ceiling[i])
        {
            if (offset + 10 > maxlen)
                break;  // packet full

            // sector index (2 bytes LE)
            buf[offset++] = i & 0xff;
            buf[offset++] = (i >> 8) & 0xff;

            // floorheight (4 bytes)
            memcpy(buf + offset, &sectors[i].floorheight, 4);
            offset += 4;

            // ceilingheight (4 bytes)
            memcpy(buf + offset, &sectors[i].ceilingheight, 4);
            offset += 4;

            sector_count++;
        }
    }

    buf[sector_count_pos] = (unsigned char)sector_count;

    // Write pending missile spawns
    {
        int missile_count = 0;
        int missile_count_pos = offset;
        offset++;  // reserve for missile count

        for (i = 0; i < pending_missile_count; i++)
        {
            pending_missile_t *pm = &pending_missiles[i];

            if (offset + 32 > maxlen)
                break;  // packet full

            // source_net_id (2 bytes)
            buf[offset++] = pm->source_net_id & 0xff;
            buf[offset++] = (pm->source_net_id >> 8) & 0xff;

            // mobjtype (2 bytes)
            buf[offset++] = pm->type & 0xff;
            buf[offset++] = (pm->type >> 8) & 0xff;

            // x, y, z (12 bytes)
            memcpy(buf + offset, &pm->x, 4); offset += 4;
            memcpy(buf + offset, &pm->y, 4); offset += 4;
            memcpy(buf + offset, &pm->z, 4); offset += 4;

            // momx, momy, momz (12 bytes)
            memcpy(buf + offset, &pm->momx, 4); offset += 4;
            memcpy(buf + offset, &pm->momy, 4); offset += 4;
            memcpy(buf + offset, &pm->momz, 4); offset += 4;

            // angle (4 bytes)
            memcpy(buf + offset, &pm->angle, 4); offset += 4;

            missile_count++;
        }

        buf[missile_count_pos] = (unsigned char)missile_count;
        pending_missile_count = 0;  // clear queue
    }

    return offset;
}

//
// P_ApplyNPCSnapshot
// Client applies a host-authoritative NPC + sector snapshot.
//
void P_ApplyNPCSnapshot(unsigned char *buf, int len)
{
    int offset = 0;
    int npc_count, sector_count, i;

    if (len < 3)
        return;

    npc_count = buf[offset++];
    // skip gametic (2 bytes)
    offset += 2;

    // Apply NPC data
    for (i = 0; i < npc_count && offset + 20 <= len; i++)
    {
        unsigned short nid;
        fixed_t x, y, z;
        unsigned char angle_byte;
        unsigned short statenum;
        short health;
        unsigned char sync_flags;
        mobj_t *mo;
        statenum_t current_sn;

        nid = buf[offset] | (buf[offset + 1] << 8);
        offset += 2;

        memcpy(&x, buf + offset, 4);
        offset += 4;
        memcpy(&y, buf + offset, 4);
        offset += 4;
        memcpy(&z, buf + offset, 4);
        offset += 4;

        angle_byte = buf[offset++];

        memcpy(&statenum, buf + offset, 2);
        offset += 2;

        memcpy(&health, buf + offset, 2);
        offset += 2;

        sync_flags = buf[offset++];

        mo = P_NetLookup(nid);
        if (!mo)
            continue;

        // Update position
        P_UnsetThingPosition(mo);
        mo->x = x;
        mo->y = y;
        mo->z = z;
        P_SetThingPosition(mo);

        // Update angle
        mo->angle = ((angle_t)angle_byte) << 24;

        // Update health
        mo->health = health;

        // Handle death transition: host says corpse, we're still alive
        if ((sync_flags & 2) && !(mo->flags & MF_CORPSE))
        {
            // Play death sound
            if (mo->info->deathsound)
                S_StartSound(mo, mo->info->deathsound);

            mo->flags &= ~(MF_SHOOTABLE | MF_FLOAT | MF_SKULLFLY);
            if (mo->type != MT_SKULL)
                mo->flags &= ~MF_NOGRAVITY;
            mo->flags |= MF_CORPSE | MF_DROPOFF;
            mo->height >>= 2;

            // Clear COUNTKILL so local code doesn't credit kills.
            // Kill counts are synced authoritatively via health auth.
            mo->flags &= ~MF_COUNTKILL;
        }

        // Update state if changed
        if (statenum < NUMSTATES)
        {
            current_sn = (statenum_t)(mo->state - states);
            if (current_sn != (statenum_t)statenum)
            {
                P_SetMobjStateNoAction(mo, (statenum_t)statenum);
            }
        }

        // Sync relevant flags (only if not already handled by death transition)
        if (!(sync_flags & 2))
        {
            if (sync_flags & 1) mo->flags |= MF_SHOOTABLE;
            else mo->flags &= ~MF_SHOOTABLE;
        }
        if (sync_flags & 4) mo->flags |= MF_SOLID;
        else mo->flags &= ~MF_SOLID;
    }

    // Apply sector data
    if (offset >= len)
        return;

    sector_count = buf[offset++];
    for (i = 0; i < sector_count && offset + 10 <= len; i++)
    {
        unsigned short sec_idx;
        fixed_t floor_h, ceil_h;

        sec_idx = buf[offset] | (buf[offset + 1] << 8);
        offset += 2;

        memcpy(&floor_h, buf + offset, 4);
        offset += 4;
        memcpy(&ceil_h, buf + offset, 4);
        offset += 4;

        if (sec_idx < numsectors)
        {
            sectors[sec_idx].floorheight = floor_h;
            sectors[sec_idx].ceilingheight = ceil_h;
        }
    }

    // Spawn missiles from host
    if (offset >= len)
        return;

    {
        int missile_count = buf[offset++];
        for (i = 0; i < missile_count && offset + 32 <= len; i++)
        {
            unsigned short src_nid, mtype;
            fixed_t mx, my, mz, mmx, mmy, mmz;
            angle_t mangle;
            mobj_t *th;
            mobj_t *src;

            src_nid = buf[offset] | (buf[offset + 1] << 8);
            offset += 2;

            mtype = buf[offset] | (buf[offset + 1] << 8);
            offset += 2;

            memcpy(&mx, buf + offset, 4); offset += 4;
            memcpy(&my, buf + offset, 4); offset += 4;
            memcpy(&mz, buf + offset, 4); offset += 4;
            memcpy(&mmx, buf + offset, 4); offset += 4;
            memcpy(&mmy, buf + offset, 4); offset += 4;
            memcpy(&mmz, buf + offset, 4); offset += 4;
            memcpy(&mangle, buf + offset, 4); offset += 4;

            if (mtype >= NUMMOBJTYPES)
                continue;

            th = P_SpawnMobj(mx, my, mz, (mobjtype_t)mtype);
            th->momx = mmx;
            th->momy = mmy;
            th->momz = mmz;
            th->angle = mangle;

            // Set source as target (owner tracking prevents self-collision)
            src = P_NetLookup(src_nid);
            if (src)
                th->target = src;

            // Play launch sound
            if (th->info->seesound)
                S_StartSound(th, th->info->seesound);
        }
    }
}
