"""Satu tab browser = satu koneksi TCP ke server.

TODO:
- teruskan frame WS <-> TCP apa adanya
- jaga backpressure: berhenti membaca TCP saat buffer WebSocket penuh
- aturan stale-DATA: abaikan DATA setelah PAUSE/SEEK/STOP sampai ACK diterima
"""
