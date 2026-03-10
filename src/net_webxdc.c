//
// Copyright(C) 2024 VectorDoom Contributors
//
// DESCRIPTION:
//      WebXDC realtimeChannel network module for VectorDoom
//      Replaces net_websockets.c - routes all packets through
//      webxdc.joinRealtimeChannel() instead of WebSockets.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>

#include "doomtype.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_misc.h"
#include "net_defs.h"
#include "net_packet.h"
#include "net_webxdc.h"
#include "z_zone.h"

#define MAX_QUEUE_SIZE 64

extern uint32_t instanceUID;

// Packet queue
typedef struct {
    net_packet_t *packets[MAX_QUEUE_SIZE];
    uint32_t froms[MAX_QUEUE_SIZE];
    int head, tail;
} packet_queue_t;

typedef struct {
    net_packet_t *packet;
    uint32_t *from;
} xdc_packet_t;

static xdc_packet_t xdcp;
static packet_queue_t client_queue;

// Address table
static int addrs_index = 0;
static net_addr_t addrs[MAX_QUEUE_SIZE];
static uint32_t ips[MAX_QUEUE_SIZE];

static boolean inittedWebXDC = false;

// Queue operations

static void QueueInit(packet_queue_t *queue) { queue->head = queue->tail = 0; }

static void QueuePush(packet_queue_t *queue, net_packet_t *packet, uint32_t from)
{
    int new_tail = (queue->tail + 1) % MAX_QUEUE_SIZE;
    if (new_tail == queue->head) return; // full
    queue->packets[queue->tail] = packet;
    queue->froms[queue->tail] = from;
    queue->tail = new_tail;
}

static xdc_packet_t *QueuePop(packet_queue_t *queue)
{
    if (queue->tail == queue->head) return NULL;
    xdcp.packet = queue->packets[queue->head];
    xdcp.from = &queue->froms[queue->head];
    queue->head = (queue->head + 1) % MAX_QUEUE_SIZE;
    return &xdcp;
}

// EM_JS: Send a binary packet via realtimeChannel
// Packet format: [to(4)][from(4)][doom_payload]
EM_JS(void, js_webxdc_send, (const uint8_t *data, int len), {
    if (!globalThis._webxdcChannel) return;
    // Copy from WASM heap (buffer may relocate)
    var packet = new Uint8Array(len);
    packet.set(HEAPU8.subarray(data, data + len));
    globalThis._webxdcChannel.send(packet);
});

// EM_JS: Receive next packet from the JS queue
// Returns byte count written, or 0 if empty
// Delivered format: [from(4)][doom_payload] (to field already stripped by JS)
EM_JS(int, js_webxdc_recv, (uint8_t *buf, int maxlen), {
    if (!globalThis._webxdcRecvQueue || globalThis._webxdcRecvQueue.length === 0) {
        return 0;
    }
    var packet = globalThis._webxdcRecvQueue.shift();
    var len = Math.min(packet.length, maxlen);
    HEAPU8.set(packet.subarray(0, len), buf);
    return len;
});

// Poll JS receive queue and push into C queue
static void PollReceivedPackets(void)
{
    uint8_t buf[4096];
    int len;

    while ((len = js_webxdc_recv(buf, sizeof(buf))) > 0) {
        if (len < 4) continue; // need at least the from address

        // Extract sender address (first 4 bytes, little-endian)
        uint32_t from_ip;
        memcpy(&from_ip, buf, 4);

        // Create doom packet from remaining data
        net_packet_t *packet = NET_NewPacket(len - 4);
        memcpy(packet->data, &buf[4], len - 4);
        packet->len = len - 4;

        QueuePush(&client_queue, packet, from_ip);
    }
}

static boolean InitWebXDC(void)
{
    if (inittedWebXDC) return true;

    // Check that the realtimeChannel was set up by webxdc-net.js
    int ready = EM_ASM_INT({
        return (globalThis._webxdcChannel != null) ? 1 : 0;
    });

    if (!ready) {
        printf("doom: WebXDC realtimeChannel not available\n");
        return false;
    }

    // Register our instanceUID with the JS layer for packet filtering
    EM_ASM({
        globalThis._doomInstanceUID = $0;
        console.log("doom: registered instanceUID=" + $0);
    }, instanceUID);

    inittedWebXDC = true;
    printf("doom: WebXDC networking initialized, uid=%u\n", instanceUID);
    return true;
}

static boolean NET_WebXDC_InitClient(void)
{
    if (!InitWebXDC()) return false;
    QueueInit(&client_queue);
    return true;
}

static boolean NET_WebXDC_InitServer(void)
{
    if (!InitWebXDC()) return false;
    QueueInit(&client_queue);
    return true;
}

static void NET_WebXDC_SendPacket(net_addr_t *addr, net_packet_t *packet)
{
    if (!InitWebXDC()) return;
    if (!addr->handle) return;

    uint32_t to_ip = (*(uint32_t *)(addr->handle));

    // Build: [to(4)][from(4)][payload]
    int total_len = packet->len + 8;
    uint8_t *buf = malloc(total_len);

    memcpy(&buf[0], &to_ip, 4);
    memcpy(&buf[4], &instanceUID, 4);
    memcpy(&buf[8], packet->data, packet->len);

    js_webxdc_send(buf, total_len);
    free(buf);
}

static net_addr_t *FindAddressByIp(uint32_t ip)
{
    // Look for existing address
    for (int i = 0; i < MAX_QUEUE_SIZE; i++) {
        if (ips[i] == ip) {
            return (&addrs[i]);
        }
    }
    // Create new entry
    if (addrs_index >= MAX_QUEUE_SIZE) {
        printf("doom: out of client addresses\n");
        return (0);
    }
    addrs[addrs_index].refcount = 1;
    addrs[addrs_index].module = &net_webxdc_module;
    ips[addrs_index] = ip;
    addrs[addrs_index].handle = &ips[addrs_index];
    return (&addrs[addrs_index++]);
}

static boolean NET_WebXDC_RecvPacket(net_addr_t **addr, net_packet_t **packet)
{
    xdc_packet_t *popped;

    if (!InitWebXDC()) return false;

    PollReceivedPackets();

    popped = QueuePop(&client_queue);
    if (popped != NULL) {
        *packet = popped->packet;
        *addr = FindAddressByIp((*(uint32_t *)(popped->from)));
        return true;
    }
    return false;
}

static void NET_WebXDC_AddrToString(net_addr_t *addr, char *buffer, int buffer_len)
{
    M_snprintf(buffer, buffer_len, "xdc peer %u", (*(uint32_t *)(addr->handle)));
}

static void NET_WebXDC_FreeAddress(net_addr_t *addr) { /* static addresses, no-op */ }

static net_addr_t *NET_WebXDC_ResolveAddress(const char *address)
{
    return FindAddressByIp((uint32_t)atoi(address));
}

net_module_t net_webxdc_module = {
    NET_WebXDC_InitClient,
    NET_WebXDC_InitServer,
    NET_WebXDC_SendPacket,
    NET_WebXDC_RecvPacket,
    NET_WebXDC_AddrToString,
    NET_WebXDC_FreeAddress,
    NET_WebXDC_ResolveAddress,
};
