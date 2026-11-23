"""Tes stats.py dan /api/stats (kontrak Tugas.md 2.4). Tidak butuh server C."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "gateway"))
import stats  # noqa: E402

HEADER = "t_s,clients,streaming,chunks_sent,underruns,late_resets,max_lateness_us\n"
KEYS = {"t_s", "clients", "streaming", "chunks_sent", "underruns", "late_resets",
        "max_lateness_us", "cpu_pct", "rss_kb", "ws_sessions"}


def write_tmp(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False)
    f.write(text)
    f.close()
    return f.name


class CsvTest(unittest.TestCase):
    def test_missing_file(self):
        self.assertIsNone(stats.read_last_csv_row("/tmp/tidak_ada.csv"))
        self.assertIsNone(stats.read_last_csv_row(None))

    def test_header_only(self):
        p = write_tmp(HEADER)
        self.addCleanup(os.remove, p)
        self.assertIsNone(stats.read_last_csv_row(p))

    def test_last_row(self):
        p = write_tmp(HEADER + "1,3,3,190,0,0,1759\n2,3,3,390,1,2,1384\n")
        self.addCleanup(os.remove, p)
        r = stats.read_last_csv_row(p)
        self.assertEqual((r["t_s"], r["clients"], r["underruns"], r["late_resets"],
                          r["max_lateness_us"]), (2, 3, 1, 2, 1384))

    def test_truncated_last_row_falls_back(self):
        p = write_tmp(HEADER + "1,3,3,190,0,0,1759\n2,3,3,39")
        self.addCleanup(os.remove, p)
        self.assertEqual(stats.read_last_csv_row(p)["t_s"], 1)


class CollectorTest(unittest.TestCase):
    def test_snapshot_keys_and_ws_counter(self):
        p = write_tmp(HEADER + "5,2,1,100,0,0,300\n")
        self.addCleanup(os.remove, p)
        c = stats.StatsCollector(p, os.getpid())
        with c.track_session():
            with c.track_session():
                self.assertEqual(c.snapshot()["ws_sessions"], 2)
        s = c.snapshot()
        self.assertEqual(set(s), KEYS)
        self.assertEqual(s["ws_sessions"], 0)
        self.assertEqual(s["t_s"], 5)
        self.assertGreater(s["rss_kb"], 0)

    def test_no_pid_gives_null(self):
        s = stats.StatsCollector(None, None).snapshot()
        self.assertIsNone(s["cpu_pct"])
        self.assertIsNone(s["rss_kb"])

    def test_dead_pid(self):
        c = stats.StatsCollector(None, 2 ** 22 + 12345)
        self.assertIsNone(c.snapshot()["rss_kb"])


try:
    import aiohttp  # noqa: F401
    from aiohttp.test_utils import AioHTTPTestCase
    import gateway as gw

    class ApiStatsTest(AioHTTPTestCase):
        async def get_application(self):
            p = write_tmp(HEADER + "7,1,1,50,0,0,200\n")
            self.addCleanup(os.remove, p)
            cfg = gw.parse_args(["--stats-csv", p])
            return gw.make_app(cfg)

        async def test_api_stats(self):
            r = await self.client.get("/api/stats")
            self.assertEqual(r.status, 200)
            self.assertEqual(set(await r.json()), KEYS)

        async def test_catalog_502_when_server_down(self):
            r = await self.client.get("/api/catalog")
            self.assertEqual(r.status, 502)
except ImportError:
    pass

if __name__ == "__main__":
    unittest.main()
