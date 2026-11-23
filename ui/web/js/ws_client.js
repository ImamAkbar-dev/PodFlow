import { MSG_TYPE, packFrame, unpackFrame } from "./protocol.js";

const WS_CLOSE_REASONS = {
    4100: "Server menutup koneksi: BAD_MAGIC (100)",
    4101: "Server menutup koneksi: BAD_VERSION (101)",
    4103: "Server menutup koneksi: PAYLOAD_TOO_LARGE (103)",
    4503: "Server sibuk: batas --max-clients tercapai (503)",
    4900: "Gateway gagal terhubung ke server C",
    4901: "Koneksi ke server C terputus atau timeout"
};

export class PodFlowWsClient {
    constructor({ onFrame, onOpen, onClose, onError }) {
        this.onFrame = onFrame || (() => {});
        this.onOpen = onOpen || (() => {});
        this.onClose = onClose || (() => {});
        this.onError = onError || (() => {});
        this.ws = null;
        this.nextSeq = 1;
    }

    connect() {
        if (this.ws && (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING)) {
            return;
        }

        const proto = window.location.protocol === "https:" ? "wss:" : "ws:";
        const url = `${proto}//${window.location.host}/ws`;

        this.ws = new WebSocket(url);
        this.ws.binaryType = "arraybuffer";
        this.nextSeq = 1;

        this.ws.onopen = () => {
            // Browser sends HELLO first per Section 2.1 contract
            this.send(MSG_TYPE.HELLO, "podflow-web-player", 0);
            this.onOpen();
        };

        this.ws.onmessage = (event) => {
            if (!(event.data instanceof ArrayBuffer)) {
                return;
            }
            try {
                const frame = unpackFrame(event.data);
                this.onFrame(frame);
            } catch (err) {
                this.onError(`Frame decode error: ${err.message}`);
            }
        };

        this.ws.onerror = () => {
            this.onError("Terjadi gangguan koneksi WebSocket ke gateway.");
        };

        this.ws.onclose = (event) => {
            const detail = WS_CLOSE_REASONS[event.code] || event.reason || `Koneksi tertutup (code ${event.code})`;
            this.onClose(event.code, detail);
        };
    }

    send(msgType, payload = null, param = 0) {
        if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
            throw new Error("WebSocket belum terhubung");
        }
        const seq = this.nextSeq++;
        const frame = packFrame(msgType, seq, payload, param);
        this.ws.send(frame);
        return seq;
    }

    play(filename) {
        return this.send(MSG_TYPE.PLAY, filename, 0);
    }

    resume() {
        // Resume sends PLAY with empty payload
        return this.send(MSG_TYPE.PLAY, null, 0);
    }

    pause() {
        return this.send(MSG_TYPE.PAUSE, null, 0);
    }

    seek(targetChunk) {
        return this.send(MSG_TYPE.SEEK, null, targetChunk >>> 0);
    }

    stop() {
        return this.send(MSG_TYPE.STOP, null, 0);
    }

    echo(pingPayload) {
        return this.send(MSG_TYPE.ECHO, pingPayload, 0);
    }

    disconnect() {
        if (this.ws) {
            this.ws.close(1000, "Client closed");
            this.ws = null;
        }
    }
}