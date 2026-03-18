//
// Copyright(C) 2024 VectorDoom contributors
//
// Network synchronization for NPCs (monsters) and sectors.
// Host is authoritative; clients receive snapshots.
//

#ifndef __P_NETSYNC__
#define __P_NETSYNC__

#include "doomtype.h"
#include "info.h"

// Forward declare mobj_t to avoid circular includes
typedef struct mobj_s mobj_t;

#define MAX_NET_MOBJS 512
#define MAX_SYNC_SECTORS 128
#define MAX_PENDING_MISSILES 8

// Net ID registry
extern mobj_t *net_mobj_table[MAX_NET_MOBJS];
extern unsigned short net_id_counter;

// Assign a net_id to a newly spawned MF_COUNTKILL mobj
void P_NetAssignId(mobj_t *mobj);

// Look up mobj by net_id (returns NULL if not found/removed)
mobj_t *P_NetLookup(unsigned short net_id);

// Clear the table (called on level load, before P_LoadThings)
void P_NetResetIds(void);

// Store initial sector heights (called after P_LoadSectors)
void P_NetStoreSectorHeights(void);

// Remove from table (called on P_RemoveMobj)
void P_NetRemoveId(mobj_t *mobj);

// Set mobj state visually WITHOUT calling action functions (client-only)
boolean P_SetMobjStateNoAction(mobj_t *mobj, statenum_t state);

// Queue a missile spawn for network broadcast (host only).
// Called from P_SpawnMissile when source is a synced NPC.
void P_NetQueueMissile(mobj_t *missile, unsigned short source_net_id);

// Host builds NPC + sector + missile snapshot into buffer. Returns bytes written.
int P_BuildNPCSnapshot(unsigned char *buf, int maxlen);

// Client applies NPC + sector + missile snapshot from buffer.
void P_ApplyNPCSnapshot(unsigned char *buf, int len);

#endif
