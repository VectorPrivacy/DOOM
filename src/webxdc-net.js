// VectorDoom WebXDC Networking Layer
// Pre-loaded before DOOM WASM. Handles:
// 1. Joining realtimeChannel (or BroadcastChannel fallback for testing)
// 2. Server election (magic bytes pattern, earliest timestamp wins)
// 3. Packet routing: broadcast all, filter by destination instanceUID

(function() {
    'use strict';

    var thisAppStartedAt = Date.now();

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
                channel.send(makeServerResponse(thisAppStartedAt));
            }
            return;
        }

        if (isServerResponse(data)) {
            var respTime = new Float64Array(data.buffer, 8, 1)[0];
            if (respTime < thisAppStartedAt) {
                // They started earlier — they're the server
                resolveAndCleanUp(false);
            }
            // If we started earlier, ignore their claim
            return;
        }

        // --- Game packets: [to(4)][from(4)][doom_payload] ---
        if (data.length < 8) return;

        // Read destination UID (little-endian uint32 at offset 0)
        var destUID = data[0] | (data[1] << 8) | (data[2] << 16) | ((data[3] << 24) >>> 0);

        // Filter: only accept packets addressed to our instanceUID
        // _doomInstanceUID is 0 until C code initializes — drop packets until then
        if (globalThis._doomInstanceUID === 0) return;
        if (destUID !== globalThis._doomInstanceUID && destUID !== 0) return;

        // Deliver [from(4)][doom_payload] to C (strip the to field)
        var delivered = new Uint8Array(data.length - 4);
        delivered.set(data.subarray(4));

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

    // Broadcast "who is server?" every 300ms
    channel.send(whoIsServerReq);
    var requestInterval = setInterval(function() {
        channel.send(whoIsServerReq);
    }, 300);

    // If no earlier server found within 3 seconds, we're it
    var electionTimeout = setTimeout(function() {
        channel.send(makeServerResponse(thisAppStartedAt));
        resolveAndCleanUp(true);
    }, 3000);

    globalThis._serverElectionP.then(function(isServer) {
        console.log('VectorDoom: I am ' + (isServer ? 'the SERVER' : 'a CLIENT'));

        // If server, keep responding to late-joining peers
        if (isServer) {
            setInterval(function() {
                channel.send(whoIsServerReq);
            }, 3000);
        }
    });

})();
