import { CHUNK_BYTES, CHUNKS_PER_SEC } from "./protocol.js";

export const PLAYER_STATE = Object.freeze({
    IDLE: "IDLE",
    PLAYING: "PLAYING",
    PAUSED: "PAUSED"
});

const SAMPLE_RATE = 44100;
const NUM_CHANNELS = 2;
const PREBUFFER_CHUNKS = 6; // 6 * 50ms = 300ms jitter buffer

export class WebAudioStreamPlayer {
    constructor({ onStateChange, onProgress, onMetrics }) {
        this.onStateChange = onStateChange || (() => {});
        this.onProgress = onProgress || (() => {});
        this.onMetrics = onMetrics || (() => {});

        this.ctx = null;
        this.state = PLAYER_STATE.IDLE;
        this.awaitingAck = false; // Stale-DATA gate after PAUSE/SEEK/STOP

        this.currentChunk = 1;
        this.totalChunks = 0;
        this.streamEnded = false;

        this.prebufferQueue = [];
        this.isPrebuffering = true;
        this.nextPlayTime = 0;
        this.activeSources = new Set();

        this.lastArrivalMs = null;
        this.rfcJitterMs = 0;
        this.progressTimer = null;
    }

    ensureContext() {
        if (!this.ctx) {
            const AudioCtx = window.AudioContext || window.webkitAudioContext;
            this.ctx = new AudioCtx({ sampleRate: SAMPLE_RATE });
        }
        if (this.ctx.state === "suspended") {
            this.ctx.resume();
        }
    }

    setState(newState) {
        if (this.state !== newState) {
            this.state = newState;
            this.onStateChange(newState);
        }
    }

    startNewTrack(knownTotalChunks = 0) {
        this.ensureContext();
        this.flushAudioQueue();
        this.totalChunks = knownTotalChunks || 0;
        this.currentChunk = 1;
        this.streamEnded = false;
        this.awaitingAck = false;
        this.isPrebuffering = true;
        this.lastArrivalMs = null;
        this.rfcJitterMs = 0;
        this.setState(PLAYER_STATE.PLAYING);
        this.startProgressLoop();
    }

    markAwaitingAck(targetState = null) {
        this.awaitingAck = true;
        this.flushAudioQueue();
        if (targetState) {
            this.setState(targetState);
        }
    }

    handleAck(ackParam) {
        this.awaitingAck = false;
        if (this.state === PLAYER_STATE.PLAYING) {
            this.isPrebuffering = true;
            if (ackParam > 0) {
                this.currentChunk = ackParam;
            }
        }
    }

    flushAudioQueue() {
        for (const src of this.activeSources) {
            try {
                src.stop();
                src.disconnect();
            } catch (_) {
                // Ignore already stopped sources
            }
        }
        this.activeSources.clear();
        this.prebufferQueue = [];
        this.nextPlayTime = 0;
    }

    handleDataFrame(frame) {
        // Ignore stale DATA while waiting for ACK after PAUSE/SEEK/STOP
        if (this.awaitingAck || this.state !== PLAYER_STATE.PLAYING) {
            return;
        }

        // seq = 0 is the 44-byte WAV header
        if (frame.seqNum === 0) {
            if (frame.payload.byteLength >= 44) {
                const dv = new DataView(
                    frame.payload.buffer,
                    frame.payload.byteOffset,
                    frame.payload.byteLength
                );
                const dataSize = dv.getUint32(40, true); // Little-Endian subchunk2Size
                if (dataSize > 0) {
                    this.totalChunks = Math.ceil(dataSize / CHUNK_BYTES);
                }
            }
            return;
        }

        // Compute indicative inter-arrival jitter (RFC 3550)
        const nowMs = performance.now();
        if (this.lastArrivalMs !== null) {
            const delta = nowMs - this.lastArrivalMs;
            const d = Math.abs(delta - 50.0);
            this.rfcJitterMs += (d - this.rfcJitterMs) / 16.0;
        }
        this.lastArrivalMs = nowMs;

        this.currentChunk = frame.seqNum;
        const audioBuf = this.pcm16ToAudioBuffer(frame.payload);
        if (!audioBuf) {
            return;
        }

        if (this.isPrebuffering) {
            this.prebufferQueue.push(audioBuf);
            if (this.prebufferQueue.length >= PREBUFFER_CHUNKS) {
                this.isPrebuffering = false;
                this.nextPlayTime = this.ctx.currentTime + 0.02;
                while (this.prebufferQueue.length > 0) {
                    this.scheduleBuffer(this.prebufferQueue.shift());
                }
            }
        } else {
            this.scheduleBuffer(audioBuf);
        }

        this.emitStats();
    }

    handleEndFrame(totalBytesParam) {
        this.streamEnded = true;
        if (this.totalChunks === 0 && totalBytesParam > 44) {
            this.totalChunks = Math.ceil((totalBytesParam - 44) / CHUNK_BYTES);
        }
        // Flush any remaining prebuffered chunks if track was shorter than PREBUFFER_CHUNKS
        if (this.isPrebuffering && this.prebufferQueue.length > 0) {
            this.isPrebuffering = false;
            this.nextPlayTime = this.ctx.currentTime + 0.02;
            while (this.prebufferQueue.length > 0) {
                this.scheduleBuffer(this.prebufferQueue.shift());
            }
        }
        if (this.activeSources.size === 0) {
            this.stop();
        }
    }

    pcm16ToAudioBuffer(uint8Bytes) {
        const bytesPerFrame = NUM_CHANNELS * 2; // 4 bytes per stereo sample frame
        const frameCount = Math.floor(uint8Bytes.byteLength / bytesPerFrame);
        if (frameCount <= 0) {
            return null;
        }

        this.ensureContext();
        const audioBuffer = this.ctx.createBuffer(NUM_CHANNELS, frameCount, SAMPLE_RATE);
        const left = audioBuffer.getChannelData(0);
        const right = audioBuffer.getChannelData(1);

        const view = new DataView(
            uint8Bytes.buffer,
            uint8Bytes.byteOffset,
            frameCount * bytesPerFrame
        );

        for (let i = 0; i < frameCount; i++) {
            const offset = i * 4;
            const sL = view.getInt16(offset, true);     // Little-Endian
            const sR = view.getInt16(offset + 2, true); // Little-Endian
            left[i] = sL / 32768.0;
            right[i] = sR / 32768.0;
        }
        return audioBuffer;
    }

    scheduleBuffer(audioBuffer) {
        this.ensureContext();
        const now = this.ctx.currentTime;
        if (this.nextPlayTime < now) {
            this.nextPlayTime = now + 0.01;
        }

        const source = this.ctx.createBufferSource();
        source.buffer = audioBuffer;
        source.connect(this.ctx.destination);

        source.onended = () => {
            this.activeSources.delete(source);
            this.emitStats();
            if (this.streamEnded && this.activeSources.size === 0 && this.state === PLAYER_STATE.PLAYING) {
                this.stop();
            }
        };

        this.activeSources.add(source);
        source.start(this.nextPlayTime);
        this.nextPlayTime += audioBuffer.duration;
    }

    stop() {
        this.flushAudioQueue();
        this.awaitingAck = false;
        this.streamEnded = false;
        this.currentChunk = 1;
        this.stopProgressLoop();
        this.setState(PLAYER_STATE.IDLE);
        this.emitProgress();
        this.emitStats();
    }

    getBufferedMs() {
        if (!this.ctx) return 0;
        const queuedAheadSec = Math.max(0, this.nextPlayTime - this.ctx.currentTime);
        const prebufSec = this.prebufferQueue.length * 0.05;
        return Math.round((queuedAheadSec + prebufSec) * 1000);
    }

    emitStats() {
        this.onMetrics({
            bufferMs: this.getBufferedMs(),
            jitterMs: Number(this.rfcJitterMs.toFixed(2)),
            activeChunks: this.activeSources.size + this.prebufferQueue.length
        });
    }

    emitProgress() {
        const curSec = Math.max(0, (this.currentChunk - 1) / CHUNKS_PER_SEC);
        const totSec = this.totalChunks > 0 ? this.totalChunks / CHUNKS_PER_SEC : 0;
        this.onProgress({
            currentChunk: this.currentChunk,
            totalChunks: this.totalChunks,
            currentSec: curSec,
            totalSec: totSec
        });
    }

    startProgressLoop() {
        this.stopProgressLoop();
        this.progressTimer = setInterval(() => {
            this.emitProgress();
            this.emitStats();
        }, 150);
    }

    stopProgressLoop() {
        if (this.progressTimer) {
            clearInterval(this.progressTimer);
            this.progressTimer = null;
        }
    }
}