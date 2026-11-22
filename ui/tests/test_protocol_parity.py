"""Pastikan konstanta di protocol.py dan protocol.js sama dengan include/protocol.h."""
import unittest


class ProtocolParity(unittest.TestCase):
    @unittest.skip("belum diimplementasikan")
    def test_constants_match_header(self):
        pass


if __name__ == "__main__":
    unittest.main()
