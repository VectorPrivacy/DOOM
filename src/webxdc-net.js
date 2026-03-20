// VectorDoom WebXDC Networking Layer
// Pre-loaded before DOOM WASM. Handles:
// 1. Joining realtimeChannel (or BroadcastChannel fallback for testing)
// 2. Server election (magic bytes pattern, earliest timestamp wins)
// 3. Packet routing: broadcast all, filter by destination instanceUID

(function() {
    'use strict';

    var thisAppStartedAt = Date.now();
    globalThis._appStartedAt = thisAppStartedAt;

    // Received packet queue — C code polls this via js_webxdc_recv
    // Packets stored as: [from(4)][doom_payload] (to field stripped)
    globalThis._webxdcRecvQueue = [];

    // Set by C code in InitWebXDC when instanceUID is known
    globalThis._doomInstanceUID = 0;

    // Server election state
    var serverElected = false;
    var amIServer = false;
    var resolveElection;

    // Promise that resolves to true (server) or false (client)
    globalThis._serverElectionP = new Promise(function(resolve) {
        resolveElection = resolve;
    });

    // Magic bytes for server election (same pattern as VectorQuake)
    // Request: [42, 42, 42, 42]
    // Response: [43, 43, 43, 43][address(4)][timestamp(8)] = 16 bytes
    var WHO_IS_SERVER_RESPONSE_SIZE = 16;

    function isServerRequest(data) {
        return data.length === 4 &&
            data[0] === 42 && data[1] === 42 &&
            data[2] === 42 && data[3] === 42;
    }

    function isServerResponse(data) {
        return data.length === WHO_IS_SERVER_RESPONSE_SIZE &&
            data[0] === 43 && data[1] === 43 &&
            data[2] === 43 && data[3] === 43;
    }

    function makeServerResponse(startedAt) {
        var buf = new ArrayBuffer(WHO_IS_SERVER_RESPONSE_SIZE);
        var u8 = new Uint8Array(buf);
        u8[0] = 43; u8[1] = 43; u8[2] = 43; u8[3] = 43;
        // bytes 4-7: unused (we don't need an address, just timestamp)
        var f64 = new Float64Array(buf, 8, 1);
        f64[0] = startedAt;
        return u8;
    }

    function resolveAndCleanUp(isServer) {
        if (serverElected) return;
        serverElected = true;
        amIServer = isServer;
        clearTimeout(electionTimeout);
        clearInterval(requestInterval);
        resolveElection(isServer);
    }

    // Handle incoming realtimeChannel messages
    function handleMessage(data) {
        // Ensure Uint8Array
        if (!(data instanceof Uint8Array)) {
            data = new Uint8Array(data);
        }

        // --- Server election messages ---
        if (isServerRequest(data)) {
            if (serverElected && amIServer) {
                trySend(makeServerResponse(thisAppStartedAt));
            }
            return;
        }

        if (isServerResponse(data)) {
            var respTime = new Float64Array(data.buffer, 8, 1)[0];
            if (respTime < thisAppStartedAt) {
                if (!serverElected) {
                    // Pre-election: they started earlier, yield
                    resolveAndCleanUp(false);
                } else if (amIServer) {
                    // Post-election demotion: we self-elected as server,
                    // but an earlier server just appeared (slow gossip).
                    // Yield by reloading — the new connection will find the
                    // real server and become a client.
                    console.log('VectorDoom: yielding to earlier server (slow gossip recovery)');
                    if (globalThis.location) globalThis.location.reload();
                }
            }
            // If we started earlier, ignore their claim
            return;
        }

        // --- Game packets: [to(4)][from(4)][doom_payload] ---
        if (data.length < 8) return;

        // Read destination UID (little-endian uint32 at offset 0)
        var destUID = data[0] | (data[1] << 8) | (data[2] << 16) | ((data[3] << 24) >>> 0);

        // Deliver [from(4)][doom_payload] to C (strip the to field)
        var delivered = new Uint8Array(data.length - 4);
        delivered.set(data.subarray(4));

        if (globalThis._doomInstanceUID === 0) {
            // C code hasn't initialized yet — buffer packets for later.
            // On slow mobile devices, SYN packets can arrive before InitWebXDC runs.
            if (!globalThis._webxdcEarlyQueue) globalThis._webxdcEarlyQueue = [];
            globalThis._webxdcEarlyQueue.push({ destUID: destUID, data: delivered });
            return;
        }

        if (destUID !== globalThis._doomInstanceUID && destUID !== 0) return;
        globalThis._webxdcRecvQueue.push(delivered);
    }

    // --- Set up realtimeChannel ---
    var channel;

    if (globalThis.webxdc) {
        if (!webxdc.joinRealtimeChannel) {
            if (globalThis._vectorDoomUI) {
                globalThis._vectorDoomUI.showError(
                    'Your messenger does not support realtimeChannel (required for multiplayer).'
                );
            }
            throw new Error('webxdc.joinRealtimeChannel not supported');
        }

        try {
            channel = webxdc.joinRealtimeChannel();
        } catch(e) {
            // Delta Chat workaround: leave stale channel and retry
            try { window.top.__webxdcRealtimeChannel.leave(); } catch(e2) {}
            channel = webxdc.joinRealtimeChannel();
        }

        try { window.top.__webxdcRealtimeChannel = channel; } catch(e) {}
    } else {
        // Fallback for local testing: BroadcastChannel
        var bc = new BroadcastChannel('vectordoom-net');
        channel = {
            send: function(data) { bc.postMessage(data); },
            setListener: function(fn) {
                bc.addEventListener('message', function(e) { fn(e.data); });
            },
            leave: function() { bc.close(); }
        };
    }

    globalThis._webxdcChannel = channel;

    channel.setListener(handleMessage);

    // --- Server election ---
    var whoIsServerReq = new Uint8Array([42, 42, 42, 42]);

    // Broadcast "who is server?" every 500ms
    function trySend(data) {
        try { channel.send(data); } catch(e) { /* channel not ready yet */ }
    }
    trySend(whoIsServerReq);
    var requestInterval = setInterval(function() {
        if (!serverElected) trySend(whoIsServerReq);
    }, 500);

    // If no earlier server found within 5 seconds, we're it
    // (Iroh gossip peer discovery can take a few seconds)
    var electionTimeout = setTimeout(function() {
        trySend(makeServerResponse(thisAppStartedAt));
        resolveAndCleanUp(true);
    }, 5000);

    globalThis._serverElectionP.then(function(isServer) {
        console.log('VectorDoom: I am ' + (isServer ? 'the SERVER' : 'a CLIENT'));

        // If server, periodically broadcast our server beacon
        // so late-joining peers discover us immediately
        if (isServer) {
            var beacon = makeServerResponse(thisAppStartedAt);
            var beaconInterval = setInterval(function() {
                if (!globalThis._webxdcChannel) { clearInterval(beaconInterval); return; }
                trySend(beacon);
            }, 5000);
        }
    });

})();
