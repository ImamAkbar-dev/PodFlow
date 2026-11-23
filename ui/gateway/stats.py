"""Statistik peraga untuk GET /api/stats (kontrak Tugas.md bagian 2.4).

Sumber data (server C TIDAK diubah):
  1. baris terakhir --stats-csv  -> 7 kolom pertama
       t_s,clients,streaming,chunks_sent,underruns,late_resets,max_lateness_us
  2. /proc/<pid>/stat dan statm   -> cpu_pct, rss_kb  (butuh --server-pid)
  3. penghitung internal gateway  -> ws_sessions

Angka ini peraga, BUKAN data benchmark resmi (lihat ../Kekurangan.md).
"""
import os
import time
from contextlib import contextmanager

CSV_FIELDS = ("t_s", "clients", "streaming", "chunks_sent",
              "underruns", "late_resets", "max_lateness_us")

_CLK_TCK = os.sysconf("SC_CLK_TCK")
_PAGE = os.sysconf("SC_PAGE_SIZE")


def read_last_csv_row(path):
    """Kembalikan dict 7 kolom dari baris data terakhir, atau None.

    None bila file tidak ada / masih kosong / baru berisi header / baris
    terakhir rusak (misal sedang ditulis). Hanya membaca ekor file, jadi
    murah walau CSV sudah panjang.
    """
    if not path:
        return None
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - 2048))
            tail = f.read().decode("ascii", errors="ignore")
    except OSError:
        return None

    for line in reversed(tail.splitlines()):
        parts = line.strip().split(",")
        if len(parts) != len(CSV_FIELDS):
            continue                      # baris terpotong
        try:
            return {k: int(v) for k, v in zip(CSV_FIELDS, parts)}
        except ValueError:
            continue                      # header atau baris rusak
    return None


def read_proc_times(pid):
    """(jiffies utime+stime, rss_kb) proses `pid`, atau None bila tidak ada."""
    try:
        with open(f"/proc/{pid}/stat", "rb") as f:
            raw = f.read().decode("ascii", errors="ignore")
        # nama proses (field 2) bisa berisi spasi/kurung: potong sesudah ')' terakhir
        rest = raw[raw.rindex(")") + 2:].split()
        jiffies = int(rest[11]) + int(rest[12])        # utime, stime
        with open(f"/proc/{pid}/statm", "rb") as f:
            rss_pages = int(f.read().split()[1])
        return jiffies, rss_pages * _PAGE // 1024
    except (OSError, ValueError, IndexError):
        return None


class StatsCollector:
    """Satu instance per proses gateway."""

    MIN_SAMPLE_S = 0.5      # beberapa tab yang polling tidak merusak jendela CPU

    def __init__(self, stats_csv=None, server_pid=None):
        self.stats_csv = stats_csv
        self.server_pid = server_pid
        self.ws_sessions = 0
        self._last_t = None
        self._last_jiffies = None
        self._cpu_pct = None
        self._rss_kb = None

    # --- penghitung sesi WebSocket -----------------------------------
    @contextmanager
    def track_session(self):
        self.ws_sessions += 1
        try:
            yield
        finally:
            self.ws_sessions -= 1

    # --- CPU / RSS ----------------------------------------------------
    def _sample_proc(self):
        if self.server_pid is None:
            return
        now = time.monotonic()
        if self._last_t is not None and now - self._last_t < self.MIN_SAMPLE_S:
            return                              # pakai nilai cache
        sample = read_proc_times(self.server_pid)
        if sample is None:                      # server mati
            self._cpu_pct = self._rss_kb = None
            self._last_t = self._last_jiffies = None
            return
        jiffies, rss_kb = sample
        if self._last_t is not None:
            dt = now - self._last_t
            self._cpu_pct = round(
                (jiffies - self._last_jiffies) / _CLK_TCK / dt * 100.0, 1)
        self._rss_kb = rss_kb
        self._last_t, self._last_jiffies = now, jiffies

    # --- hasil akhir ---------------------------------------------------
    def snapshot(self):
        """Dict sesuai kontrak 2.4. Nilai yang tidak diketahui = None (null)."""
        row = read_last_csv_row(self.stats_csv) or {}
        self._sample_proc()
        out = {k: row.get(k) for k in CSV_FIELDS}
        out["cpu_pct"] = self._cpu_pct
        out["rss_kb"] = self._rss_kb
        out["ws_sessions"] = self.ws_sessions
        return out
