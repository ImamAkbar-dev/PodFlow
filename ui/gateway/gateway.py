"""Entrypoint gateway PodFlow: web statis + /ws + /api/catalog + /api/stats.

    python3 gateway/gateway.py --port 8080 --server-host 127.0.0.1 \
        --server-port 8888 --stats-csv /tmp/podflow_stats.csv --server-pid 1234

Kontrak: ../Tugas.md bagian 2. Gateway tidak mengubah protokol dan tidak
menyentuh server C. Logika jembatan WS<->TCP ada di session.py (Imam),
katalog di catalog.py (Imam), statistik di stats.py.
"""
import argparse
import asyncio
import logging
import os
import sys

from aiohttp import web

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stats import StatsCollector  # noqa: E402

log = logging.getLogger("gateway")
WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web")


# --------------------------------------------------------------------------
# Handler
# --------------------------------------------------------------------------
async def handle_ws(request):
    # Import di sini supaya gateway tetap bisa hidup (statis + stats) selama
    # session.py milik Imam belum selesai.
    from session import Session

    cfg = request.app["cfg"]
    ws = web.WebSocketResponse(heartbeat=20, max_msg_size=70 * 1024)
    await ws.prepare(request)

    with request.app["stats"].track_session():
        # Antarmuka yang diharapkan dari session.py (sepakati dengan Imam):
        #   Session(ws, server_host, server_port); await session.run()
        # run() selesai saat salah satu sisi menutup, dan sudah menutup ws
        # dengan kode 4xxx (tabel 2.2) serta membersihkan soket TCP.
        session = Session(ws, cfg.server_host, cfg.server_port)
        try:
            await session.run()
        except asyncio.CancelledError:
            raise
        except Exception:
            log.exception("session error")
            if not ws.closed:
                await ws.close(code=4901, message=b"gateway error")
    return ws


async def handle_catalog(request):
    cfg = request.app["cfg"]
    try:
        from catalog import fetch_catalog   # milik Imam (async)
        data = await fetch_catalog(cfg.server_host, cfg.server_port)
    except Exception as exc:
        log.warning("catalog gagal: %s", exc)
        return web.json_response({"error": "server tidak terjangkau"}, status=502)
    return web.json_response(data)


async def handle_stats(request):
    return web.json_response(request.app["stats"].snapshot())


async def handle_index(request):
    return web.FileResponse(os.path.join(WEB_DIR, "index.html"))


@web.middleware
async def no_cache(request, handler):
    """Saat pengembangan, browser tidak boleh memakai js/css lama."""
    resp = await handler(request)
    resp.headers["Cache-Control"] = "no-store"
    return resp


# --------------------------------------------------------------------------
def make_app(cfg):
    app = web.Application(middlewares=[no_cache])
    app["cfg"] = cfg
    app["stats"] = StatsCollector(cfg.stats_csv, cfg.server_pid)
    app.router.add_get("/", handle_index)
    app.router.add_get("/ws", handle_ws)
    app.router.add_get("/api/catalog", handle_catalog)
    app.router.add_get("/api/stats", handle_stats)
    app.router.add_static("/", os.path.abspath(WEB_DIR), show_index=False)
    return app


def parse_args(argv=None):
    p = argparse.ArgumentParser(description="PodFlow web gateway")
    p.add_argument("--host", default="0.0.0.0", help="alamat bind gateway")
    p.add_argument("--port", type=int, default=8080, help="port HTTP/WS gateway")
    p.add_argument("--server-host", default="127.0.0.1")
    p.add_argument("--server-port", type=int, default=8888)
    p.add_argument("--stats-csv", default=None,
                   help="CSV yang ditulis server C lewat --stats-csv")
    p.add_argument("--server-pid", type=int, default=None,
                   help="PID server C, untuk CPU dan RSS dari /proc")
    return p.parse_args(argv)


def main():
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    cfg = parse_args()
    log.info("gateway http://%s:%d -> server %s:%d | stats=%s pid=%s",
             cfg.host, cfg.port, cfg.server_host, cfg.server_port,
             cfg.stats_csv or "off", cfg.server_pid or "off")
    web.run_app(make_app(cfg), host=cfg.host, port=cfg.port, print=None)


if __name__ == "__main__":
    main()
