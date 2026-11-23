import { CHUNKS_PER_SEC, ERR_CODE, MSG_TYPE } from "./protocol.js";
import { PodFlowWsClient } from "./ws_client.js";
import { PLAYER_STATE, WebAudioStreamPlayer } from "./player.js";

const el = {
    wsBadge: document.getElementById("ws-badge"),
    fsmBadge: document.getElementById("fsm-badge"),
    errorBanner: document.getElementById("error-banner"),
    errorText: document.getElementById("error-text"),
    btnRetry: document.getElementById("btn-retry"),
    btnDismissErr: document.getElementById("btn-dismiss-err"),
    searchInput: document.getElementById("search-input"),
    episodeList: document.getElementById("episode-list"),
    btnRefreshCatalog: document.getElementById("btn-refresh-catalog"),
    activeTrackName: document.getElementById("active-track-name"),
    pbTrackTitle: document.getElementById("pb-track-title"),
    coverArtBox: document.getElementById("cover-art-box"),
    timeCurrent: document.getElementById("time-current"),
    timeTotal: document.getElementById("time-total"),
    chunkIndicator: document.getElementById("chunk-indicator"),
    seekSlider: document.getElementById("seek-slider"),
    btnPlay: document.getElementById("btn-play"),
    btnPause: document.getElementById("btn-pause"),
    btnResume: document.getElementById("btn-resume"),
    btnStop: document.getElementById("btn-stop"),
    btnPing: document.getElementById("btn-ping"),
    metricBuffer: document.getElementById("metric-buffer"),
    metricJitter: document.getElementById("metric-jitter"),
    metricRtt: document.getElementById("metric-rtt")
};

let catalogFiles = [];
let selectedEpisode = null;
let wsConnected = false;
let isDraggingSlider = false;
let pingSentAt = 0;
let retryCount = 0;

const player = new WebAudioStreamPlayer({
    onStateChange: updateControlStates,
    onProgress: ({ currentChunk, totalChunks, currentSec, totalSec }) => {
        el.timeCurrent.textContent = formatTime(currentSec);
        el.timeTotal.textContent = formatTime(totalSec);
        el.chunkIndicator.textContent = `Chunk: ${currentChunk} / ${totalChunks || "?"}`;

        if (totalChunks > 0) {
            el.seekSlider.max = String(totalChunks);
            if (!isDraggingSlider) {
                el.seekSlider.value = String(Math.min(currentChunk, totalChunks));
            }
        }
    },
    onMetrics: ({ bufferMs, jitterMs }) => {
        el.metricBuffer.textContent = `${bufferMs} ms`;
        el.metricJitter.textContent = `${jitterMs.toFixed(2)} ms`;
    }
});

const client = new PodFlowWsClient({
    onOpen: () => {
        wsConnected = true;
        retryCount = 0;
        el.wsBadge.textContent = "WS: Connected";
        el.wsBadge.className = "pill-badge badge-online";
        hideError();
        updateControlStates(player.state);
    },
    onClose: (code, reason) => {
        wsConnected = false;
        el.wsBadge.textContent = "WS: Disconnected";
        el.wsBadge.className = "pill-badge badge-offline";
        player.stop();
        updateControlStates(PLAYER_STATE.IDLE);
        if (code !== 1000) {
            showError(reason, code === 4503);
        }
    },
    onError: (msg) => showError(msg, false),
    onFrame: (frame) => {
        switch (frame.msgType) {
            case MSG_TYPE.WELCOME:
                break;
            case MSG_TYPE.ACK:
                player.handleAck(frame.param);
                break;
            case MSG_TYPE.DATA:
                player.handleDataFrame(frame);
                break;
            case MSG_TYPE.END:
                player.handleEndFrame(frame.param);
                break;
            case MSG_TYPE.ECHO:
                if (pingSentAt > 0) {
                    const rtt = (performance.now() - pingSentAt).toFixed(1);
                    el.metricRtt.textContent = `${rtt} ms`;
                    pingSentAt = 0;
                }
                break;
            case MSG_TYPE.ERROR: {
                const detail = frame.textPayload();
                const isBusy = frame.param === ERR_CODE.SERVER_BUSY;
                showError(`Server ERROR ${frame.param}: ${detail}`, isBusy);
                if (frame.param === ERR_CODE.FILE_NOT_FOUND || isBusy) {
                    player.stop();
                } else {
                    player.awaitingAck = false;
                }
                break;
            }
        }
    }
});

function formatTime(sec) {
    const mins = Math.floor(sec / 60);
    const rem = (sec % 60).toFixed(1).padStart(4, "0");
    return `${String(mins).padStart(2, "0")}:${rem}`;
}

function showError(msg, allowRetry = false) {
    el.errorText.textContent = msg;
    el.errorBanner.classList.remove("hidden");
    el.btnRetry.classList.toggle("hidden", !allowRetry);
}

function hideError() {
    el.errorBanner.classList.add("hidden");
}

function updateControlStates(state) {
    el.fsmBadge.textContent = `STATE: ${state}`;
    el.fsmBadge.className = `pill-badge badge-${state.toLowerCase()}`;
    el.coverArtBox.classList.toggle("is-playing", state === PLAYER_STATE.PLAYING);

    const hasTrack = selectedEpisode !== null;
    const isPlaying = state === PLAYER_STATE.PLAYING;

    el.btnPing.disabled = !wsConnected;
    el.btnPlay.style.display = isPlaying ? "none" : "flex";
    el.btnPause.style.display = isPlaying ? "flex" : "none";

    el.btnPlay.disabled = !wsConnected || !hasTrack || isPlaying;
    el.btnPause.disabled = !wsConnected || !isPlaying;
    el.btnResume.disabled = !wsConnected || state !== PLAYER_STATE.PAUSED;
    el.btnStop.disabled = !wsConnected || state === PLAYER_STATE.IDLE;
    el.seekSlider.disabled = !wsConnected || state === PLAYER_STATE.IDLE || player.totalChunks <= 1;
}

async function loadCatalog() {
    try {
        const res = await fetch("/api/catalog");
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const data = await res.json();
        catalogFiles = data.files || [];
        renderCatalog();
    } catch (err) {
        el.episodeList.innerHTML = `<li class="empty-msg">Gagal memuat katalog (${err.message}).</li>`;
    }
}

function renderCatalog() {
    const query = (el.searchInput?.value || "").toLowerCase().trim();
    const filtered = catalogFiles.filter((ep) => ep.name.toLowerCase().includes(query));

    if (filtered.length === 0) {
        el.episodeList.innerHTML = `<li class="empty-msg">Tidak ada episode yang cocok.</li>`;
        return;
    }

    el.episodeList.innerHTML = "";
    filtered.forEach((ep, idx) => {
        const li = document.createElement("li");
        li.className = "episode-item" + (selectedEpisode?.name === ep.name ? " selected" : "");
        const durText = ep.duration_s ? `${ep.duration_s.toFixed(1)}s` : "-";
        const chunksText = ep.total_chunks ? `${ep.total_chunks} chunks` : "";
        li.innerHTML = `
            <div class="ep-art">#${idx + 1}</div>
            <div class="ep-info">
                <strong>${ep.name}</strong>
                <span class="episode-meta">WAV &bull; ${durText} &bull; ${chunksText}</span>
            </div>
        `;
        li.onclick = () => {
            selectedEpisode = ep;
            el.activeTrackName.textContent = ep.name;
            el.pbTrackTitle.textContent = ep.name;
            renderCatalog();
            updateControlStates(player.state);
        };
        el.episodeList.appendChild(li);
    });
}

el.searchInput.oninput = renderCatalog;

el.btnPlay.onclick = () => {
    if (!selectedEpisode) return;
    hideError();
    player.startNewTrack(selectedEpisode.total_chunks || 0);
    client.play(selectedEpisode.name);
};

el.btnPause.onclick = () => {
    player.markAwaitingAck(PLAYER_STATE.PAUSED);
    client.pause();
};

el.btnResume.onclick = () => {
    player.ensureContext();
    player.markAwaitingAck(PLAYER_STATE.PLAYING);
    client.resume();
};

el.btnStop.onclick = () => {
    player.markAwaitingAck(PLAYER_STATE.IDLE);
    client.stop();
    player.stop();
};

el.btnPing.onclick = () => {
    pingSentAt = performance.now();
    client.echo(`ping-${Date.now()}`);
};

el.seekSlider.oninput = () => {
    isDraggingSlider = true;
    const previewChunk = Number(el.seekSlider.value);
    el.timeCurrent.textContent = formatTime((previewChunk - 1) / CHUNKS_PER_SEC);
};

el.seekSlider.onchange = () => {
    isDraggingSlider = false;
    const maxChunk = Math.max(1, player.totalChunks);
    const targetChunk = Math.min(maxChunk, Math.max(1, Number(el.seekSlider.value)));
    player.markAwaitingAck(null);
    client.seek(targetChunk);
};

el.btnRefreshCatalog.onclick = loadCatalog;
el.btnDismissErr.onclick = hideError;

el.btnRetry.onclick = () => {
    retryCount++;
    const backoffMs = Math.min(4000, 500 * Math.pow(2, retryCount - 1));
    showError(`Mencoba menghubungkan ulang dalam ${backoffMs} ms...`, false);
    setTimeout(() => {
        client.connect();
        if (selectedEpisode) {
            setTimeout(() => {
                if (wsConnected) el.btnPlay.click();
            }, 300);
        }
    }, backoffMs);
};

window.addEventListener("beforeunload", () => {
    if (wsConnected && player.state !== PLAYER_STATE.IDLE) {
        try { client.stop(); } catch (_) {}
    }
    client.disconnect();
});

updateControlStates(PLAYER_STATE.IDLE);
loadCatalog();
client.connect();