#!/usr/bin/env python3
"""
Generate Laporan Evaluasi Target Sesi 1 PodFlow in professional PDF format.
"""
import ctypes
import os
import sys

# Load libraries
cairo = ctypes.CDLL('libcairo.so.2')
pango = ctypes.CDLL('libpango-1.0.so.0')
pangocairo = ctypes.CDLL('libpangocairo-1.0.so.0')

# Setup Cairo types
cairo.cairo_pdf_surface_create.restype = ctypes.c_void_p
cairo.cairo_pdf_surface_create.argtypes = [ctypes.c_char_p, ctypes.c_double, ctypes.c_double]
cairo.cairo_surface_destroy.argtypes = [ctypes.c_void_p]
cairo.cairo_create.restype = ctypes.c_void_p
cairo.cairo_create.argtypes = [ctypes.c_void_p]
cairo.cairo_destroy.argtypes = [ctypes.c_void_p]
cairo.cairo_save.argtypes = [ctypes.c_void_p]
cairo.cairo_restore.argtypes = [ctypes.c_void_p]
cairo.cairo_show_page.argtypes = [ctypes.c_void_p]
cairo.cairo_set_source_rgb.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_double, ctypes.c_double]
cairo.cairo_set_source_rgba.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_double, ctypes.c_double, ctypes.c_double]
cairo.cairo_move_to.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_double]
cairo.cairo_line_to.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_double]
cairo.cairo_rectangle.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_double, ctypes.c_double, ctypes.c_double]
cairo.cairo_fill.argtypes = [ctypes.c_void_p]
cairo.cairo_stroke.argtypes = [ctypes.c_void_p]
cairo.cairo_set_line_width.argtypes = [ctypes.c_void_p, ctypes.c_double]

# Setup Pango types
pangocairo.pango_cairo_create_layout.restype = ctypes.c_void_p
pangocairo.pango_cairo_create_layout.argtypes = [ctypes.c_void_p]
pangocairo.pango_cairo_show_layout.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
pango.pango_layout_set_markup.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
pango.pango_layout_set_width.argtypes = [ctypes.c_void_p, ctypes.c_int]
pango.pango_layout_get_size.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
pango.g_object_unref.argtypes = [ctypes.c_void_p]

PAGE_W = 595.28  # A4 width in pt
PAGE_H = 841.89  # A4 height in pt
MARGIN_X = 42.0
CONTENT_W = PAGE_W - (2 * MARGIN_X)
PANGO_SCALE = 1024

class PDFDoc:
    def __init__(self, filename):
        self.filename = filename.encode('utf-8')
        self.surf = cairo.cairo_pdf_surface_create(self.filename, PAGE_W, PAGE_H)
        self.cr = cairo.cairo_create(self.surf)
        self.page_num = 1
        self.y = 45.0
        self.draw_header()

    def draw_header(self):
        # Header banner line & title
        cairo.cairo_save(self.cr)
        cairo.cairo_set_source_rgb(self.cr, 0.12, 0.28, 0.49) # Navy
        cairo.cairo_rectangle(self.cr, MARGIN_X, 32.0, CONTENT_W, 2.0)
        cairo.cairo_fill(self.cr)

        layout = pangocairo.pango_cairo_create_layout(self.cr)
        markup = "<span font_desc='Sans 8' foreground='#666666'><b>PodFlow</b> — Laporan Evaluasi Ketercapaian Target Sesi 1</span>"
        pango.pango_layout_set_markup(layout, markup.encode('utf-8'), -1)
        cairo.cairo_move_to(self.cr, MARGIN_X, 18.0)
        pangocairo.pango_cairo_show_layout(self.cr, layout)
        pango.g_object_unref(layout)
        cairo.cairo_restore(self.cr)

    def draw_footer(self):
        cairo.cairo_save(self.cr)
        cairo.cairo_set_source_rgb(self.cr, 0.85, 0.85, 0.85)
        cairo.cairo_rectangle(self.cr, MARGIN_X, PAGE_H - 35.0, CONTENT_W, 1.0)
        cairo.cairo_fill(self.cr)

        layout = pangocairo.pango_cairo_create_layout(self.cr)
        page_str = f"<span font_desc='Sans 8' foreground='#888888'>Halaman {self.page_num}</span>"
        pango.pango_layout_set_markup(layout, page_str.encode('utf-8'), -1)
        cairo.cairo_move_to(self.cr, PAGE_W - MARGIN_X - 60.0, PAGE_H - 28.0)
        pangocairo.pango_cairo_show_layout(self.cr, layout)
        pango.g_object_unref(layout)

        l2 = pangocairo.pango_cairo_create_layout(self.cr)
        sys_str = "<span font_desc='Sans 8' foreground='#888888'>Pemrograman Jaringan — Sesi 1 Audio Streaming Server</span>"
        pango.pango_layout_set_markup(l2, sys_str.encode('utf-8'), -1)
        cairo.cairo_move_to(self.cr, MARGIN_X, PAGE_H - 28.0)
        pangocairo.pango_cairo_show_layout(self.cr, l2)
        pango.g_object_unref(l2)
        cairo.cairo_restore(self.cr)

    def new_page(self):
        self.draw_footer()
        cairo.cairo_show_page(self.cr)
        self.page_num += 1
        self.y = 48.0
        self.draw_header()

    def check_space(self, needed_h):
        if self.y + needed_h > PAGE_H - 45.0:
            self.new_page()

    def draw_markup(self, markup, x=None, width=None, line_spacing=0):
        if x is None:
            x = MARGIN_X
        if width is None:
            width = CONTENT_W

        layout = pangocairo.pango_cairo_create_layout(self.cr)
        pango.pango_layout_set_markup(layout, markup.encode('utf-8'), -1)
        pango.pango_layout_set_width(layout, int(width * PANGO_SCALE))

        w = ctypes.c_int()
        h = ctypes.c_int()
        pango.pango_layout_get_size(layout, ctypes.byref(w), ctypes.byref(h))
        height_pt = h.value / PANGO_SCALE

        self.check_space(height_pt)

        cairo.cairo_move_to(self.cr, x, self.y)
        pangocairo.pango_cairo_show_layout(self.cr, layout)
        pango.g_object_unref(layout)

        self.y += height_pt + line_spacing
        return height_pt

    def draw_card(self, title, items, bg_rgb=(0.96, 0.97, 0.99), border_rgb=(0.75, 0.82, 0.90)):
        # Calculate heights
        layout_t = pangocairo.pango_cairo_create_layout(self.cr)
        t_markup = f"<span font_desc='Sans Bold 10' foreground='#1A365D'>{title.replace('&', '&amp;')}</span>"
        pango.pango_layout_set_markup(layout_t, t_markup.encode('utf-8'), -1)
        w = ctypes.c_int()
        h = ctypes.c_int()
        pango.pango_layout_get_size(layout_t, ctypes.byref(w), ctypes.byref(h))
        th = h.value / PANGO_SCALE
        pango.g_object_unref(layout_t)

        item_layouts = []
        total_items_h = 0
        for it in items:
            l = pangocairo.pango_cairo_create_layout(self.cr)
            pango.pango_layout_set_markup(l, it.encode('utf-8'), -1)
            pango.pango_layout_set_width(l, int((CONTENT_W - 24.0) * PANGO_SCALE))
            lw = ctypes.c_int()
            lh = ctypes.c_int()
            pango.pango_layout_get_size(l, ctypes.byref(lw), ctypes.byref(lh))
            h_val = lh.value / PANGO_SCALE
            item_layouts.append((l, h_val))
            total_items_h += h_val + 4.0

        card_h = th + total_items_h + 18.0
        self.check_space(card_h + 6.0)

        # Draw card background
        cairo.cairo_save(self.cr)
        cairo.cairo_set_source_rgb(self.cr, bg_rgb[0], bg_rgb[1], bg_rgb[2])
        cairo.cairo_rectangle(self.cr, MARGIN_X, self.y, CONTENT_W, card_h)
        cairo.cairo_fill(self.cr)

        cairo.cairo_set_source_rgb(self.cr, border_rgb[0], border_rgb[1], border_rgb[2])
        cairo.cairo_set_line_width(self.cr, 1.0)
        cairo.cairo_rectangle(self.cr, MARGIN_X, self.y, CONTENT_W, card_h)
        cairo.cairo_stroke(self.cr)

        # Title highlight bar on left
        cairo.cairo_set_source_rgb(self.cr, 0.12, 0.35, 0.65)
        cairo.cairo_rectangle(self.cr, MARGIN_X, self.y, 4.0, card_h)
        cairo.cairo_fill(self.cr)
        cairo.cairo_restore(self.cr)

        # Draw Title
        layout_t = pangocairo.pango_cairo_create_layout(self.cr)
        pango.pango_layout_set_markup(layout_t, t_markup.encode('utf-8'), -1)
        cairo.cairo_move_to(self.cr, MARGIN_X + 12.0, self.y + 8.0)
        pangocairo.pango_cairo_show_layout(self.cr, layout_t)
        pango.g_object_unref(layout_t)

        cur_y = self.y + 8.0 + th + 6.0
        for l, h_val in item_layouts:
            cairo.cairo_move_to(self.cr, MARGIN_X + 12.0, cur_y)
            pangocairo.pango_cairo_show_layout(self.cr, l)
            pango.g_object_unref(l)
            cur_y += h_val + 4.0

        self.y += card_h + 10.0

    def draw_table(self, headers, rows, col_widths):
        # Calculate cell layouts for header
        header_layouts = []
        max_h_head = 0
        for i, h in enumerate(headers):
            l = pangocairo.pango_cairo_create_layout(self.cr)
            markup = f"<span font_desc='Sans Bold 8.5' foreground='#FFFFFF'>{h.replace('&', '&amp;')}</span>"
            pango.pango_layout_set_markup(l, markup.encode('utf-8'), -1)
            pango.pango_layout_set_width(l, int((col_widths[i] - 10.0) * PANGO_SCALE))
            w = ctypes.c_int()
            height = ctypes.c_int()
            pango.pango_layout_get_size(l, ctypes.byref(w), ctypes.byref(height))
            h_val = height.value / PANGO_SCALE
            if h_val > max_h_head:
                max_h_head = h_val
            header_layouts.append(l)

        head_row_h = max_h_head + 10.0
        self.check_space(head_row_h + 30.0)

        # Draw header bg
        cairo.cairo_save(self.cr)
        cairo.cairo_set_source_rgb(self.cr, 0.15, 0.28, 0.44)
        cairo.cairo_rectangle(self.cr, MARGIN_X, self.y, CONTENT_W, head_row_h)
        cairo.cairo_fill(self.cr)
        cairo.cairo_restore(self.cr)

        cur_x = MARGIN_X
        for i, l in enumerate(header_layouts):
            cairo.cairo_move_to(self.cr, cur_x + 5.0, self.y + 5.0)
            pangocairo.pango_cairo_show_layout(self.cr, l)
            pango.g_object_unref(l)
            cur_x += col_widths[i]

        self.y += head_row_h

        # Draw rows
        for r_idx, row in enumerate(rows):
            row_layouts = []
            max_rh = 0
            for i, cell in enumerate(row):
                l = pangocairo.pango_cairo_create_layout(self.cr)
                markup = f"<span font_desc='Sans 8' foreground='#222222'>{cell}</span>"
                pango.pango_layout_set_markup(l, markup.encode('utf-8'), -1)
                pango.pango_layout_set_width(l, int((col_widths[i] - 10.0) * PANGO_SCALE))
                w = ctypes.c_int()
                height = ctypes.c_int()
                pango.pango_layout_get_size(l, ctypes.byref(w), ctypes.byref(height))
                h_val = height.value / PANGO_SCALE
                if h_val > max_rh:
                    max_rh = h_val
                row_layouts.append(l)

            row_h = max_rh + 8.0
            self.check_space(row_h)

            cairo.cairo_save(self.cr)
            if r_idx % 2 == 1:
                cairo.cairo_set_source_rgb(self.cr, 0.96, 0.97, 0.98)
                cairo.cairo_rectangle(self.cr, MARGIN_X, self.y, CONTENT_W, row_h)
                cairo.cairo_fill(self.cr)

            # Bottom row border
            cairo.cairo_set_source_rgb(self.cr, 0.88, 0.90, 0.92)
            cairo.cairo_set_line_width(self.cr, 0.5)
            cairo.cairo_move_to(self.cr, MARGIN_X, self.y + row_h)
            cairo.cairo_line_to(self.cr, MARGIN_X + CONTENT_W, self.y + row_h)
            cairo.cairo_stroke(self.cr)
            cairo.cairo_restore(self.cr)

            cur_x = MARGIN_X
            for i, l in enumerate(row_layouts):
                cairo.cairo_move_to(self.cr, cur_x + 5.0, self.y + 4.0)
                pangocairo.pango_cairo_show_layout(self.cr, l)
                pango.g_object_unref(l)
                cur_x += col_widths[i]

            self.y += row_h

        self.y += 10.0

    def close(self):
        self.draw_footer()
        cairo.cairo_show_page(self.cr)
        cairo.cairo_destroy(self.cr)
        cairo.cairo_surface_destroy(self.surf)

def build_pdf(out_path):
    doc = PDFDoc(out_path)

    # Title Block
    title = (
        "<span font_desc='Sans Bold 16' foreground='#1A365D'>LAPORAN EVALUASI KETERCAPAIAN TARGET SESI 1</span>\n"
        "<span font_desc='Sans 10' foreground='#4A5568'>Proyek: <b>PodFlow (Concurrent Chunked Audio Streaming Server)</b> | Topik 9</span>\n"
        "<span font_desc='Sans 8.5' foreground='#718096'>Mata Kuliah: Pemrograman Jaringan | Waktu Audit: 2 November 2026</span>"
    )
    doc.draw_markup(title, line_spacing=12)

    # Executive Summary Box
    doc.draw_card(
        "RINGKASAN EKSEKUTIF STATUS PROYEK",
        [
            "<b>Tujuan Sesi 1:</b> Menyelesaikan fondasi arsitektur server multi-thread C11 (POSIX Sockets &amp; pthreads), spesifikasi protokol biner v1 16-byte, fungsi short read/write (<tt>readn()</tt>, <tt>writen()</tt>), dan streaming chunk WAV 50 ms (8.820 byte) multi-klien dengan verifikasi md5sum 100% identik.",
            "<b>Hasil Evaluasi:</b> Komponen <b>Protokol &amp; I/O Socket (Anggota 2 &amp; Sebagian Anggota 1)</b> telah selesai dan lulus <b>100% pada 8 Unit Test Suite</b> (termasuk AddressSanitizer dan Valgrind bebas kebocoran memori).",
            "<b>Kebutuhan Tindak Lanjut:</b> Server konkurensi (<tt>server.c</tt>), client referensi (<tt>client.c</tt>), generator audio (<tt>generate_wav.sh</tt>), dan skrip uji beban (<tt>test_md5.sh</tt>) masih berukuran 0 byte. Selain itu ada <b>merge conflict Git</b> pada <tt>.gitignore</tt> yang perlu segera dibersihkan."
        ],
        bg_rgb=(0.95, 0.98, 0.96), border_rgb=(0.7, 0.88, 0.75)
    )

    # Section 1
    doc.draw_markup(
        "<span font_desc='Sans Bold 12' foreground='#1A365D'>1. Tabel Evaluasi 6 Kriteria Keberhasilan ('Done When' Sesi 1)</span>\n"
        "<span font_desc='Sans 8' foreground='#718096'>Mengacu pada standar rubrik penilaian panduan teknis Halaman 6:</span>",
        line_spacing=6
    )

    headers = ["No", "Tahap Sesi 1", "Target Standar Panduan", "Status Repositori", "Catatan Teknis & Verifikasi"]
    col_w = [25.0, 90.0, 115.0, 75.0, CONTENT_W - 305.0]

    rows = [
        [
            "1",
            "<b>Persiapan Alat &amp; Build</b>",
            "Semua anggota menjalankan make dengan 0 warning (-Wall -Wextra -Werror).",
            "<span foreground='#D97706'><b>Sebagian</b></span>",
            "Target <tt>make m2</tt>, <tt>make test</tt>, <tt>make test-asan</tt> berhasil (0 warning). <tt>make all</tt> gagal link karena <tt>server.c</tt> dan <tt>client.c</tt> kosong."
        ],
        [
            "2",
            "<b>Konfigurasi Proyek</b>",
            "README.md &amp; include/protocol.h masuk Git (port 8888, chunk 8.820B).",
            "<span foreground='#D97706'><b>Perlu Perbaikan</b></span>",
            "<tt>protocol.h</tt> sudah lengkap. Namun <tt>README.md</tt> masih 0 byte (konten di <tt>readme.md</tt> duplikat) dan <tt>.gitignore</tt> terdapat konflik git."
        ],
        [
            "3",
            "<b>Dokumen Protokol v1</b>",
            "docs/protocol_spec_v1.md lengkap (header 16B, 12 tipe pesan, 10 kode error).",
            "<span foreground='#059669'><b>Lengkap (100%)</b></span>",
            "Dokumen sangat komprehensif (293 baris) mencakup penurunan matematis chunk 8.820B, sequence diagram, hex dump, dan error table. Siap di-commit ke Git."
        ],
        [
            "4",
            "<b>Framing &amp; I/O Dasar</b>",
            "5 klien konkuren masing-masing kirim/terima 1.000 frame tanpa korupsi.",
            "<span foreground='#059669'><b>Inti Lulus (100%)</b></span>",
            "<tt>readn()</tt>, <tt>writen()</tt>, <tt>send_frame()</tt>, <tt>recv_frame()</tt> lulus 8 test suite di <tt>test_protocol.c</tt> (1.000 frame roundtrip, fuzzing, dribble 1-byte)."
        ],
        [
            "5",
            "<b>Server Multi-Thread</b>",
            "Server melayani 5-10 klien serentak dengan pthread_detach &amp; bebas leak.",
            "<span foreground='#DC2626'><b>Belum Ada (0%)</b></span>",
            "Berkas <tt>src/server/server.c</tt> masih 0 byte. Implementasi listener socket, loop accept, thread worker, dan <tt>stream_wav_file()</tt> belum diimplementasikan."
        ],
        [
            "6",
            "<b>Kirim Berkas WAV</b>",
            "5 klien mengunduh sample_30s.wav serentak dengan md5sum 100% identik.",
            "<span foreground='#DC2626'><b>Belum Ada (0%)</b></span>",
            "Berkas <tt>client.c</tt>, <tt>generate_wav.sh</tt>, dan <tt>test_md5.sh</tt> masih kosong. Berkas media WAV dan capture <tt>.pcap</tt> belum dibuat."
        ]
    ]

    doc.draw_table(headers, rows, col_w)

    # Section 2: Progress per Member
    doc.draw_markup(
        "<span font_desc='Sans Bold 12' foreground='#1A365D'>2. Evaluasi Pembagian Kepemilikan &amp; Kontribusi Tim</span>\n"
        "<span font_desc='Sans 8' foreground='#718096'>Berdasarkan matriks tanggung jawab 3 anggota (Panduan Hal. 3 &amp; 12):</span>",
        line_spacing=6
    )

    doc.draw_card(
        "ANGGOTA 2 (Protokol & Framing Lead) — Progres: ~95% (Sangat Baik)",
        [
            "<b>Hasil Implementasi:</b>",
            "• <tt>include/protocol.h</tt>: Struct <tt>frame_hdr_t</tt> 16-byte packed (<tt>_Static_assert</tt>), konstanta <tt>0x5753</tt>, opcodes <tt>0x01..0x0B, 0xFF</tt>, error codes <tt>0..500</tt>.",
            "• <tt>src/common/protocol.c</tt>: Serialisasi (<tt>send_frame</tt>), deserialisasi (<tt>recv_frame</tt>), validasi endianness, proteksi batas payload 65.536 byte.",
            "• <tt>docs/protocol_spec_v1.md</tt>: Dokumentasi spesifikasi protokol 15% bobot penilaian telah rampung secara mendalam.",
            "• <tt>tests/test_protocol.c</tt>: 8 suite test (Roundtrip, Boundary, Bad Magic/Version, Dribble, 1000 Frame Stream, Fuzzing) lulus <b>ASan &amp; Valgrind (0 leaks)</b>.",
            "<i>Catatan Penyempurnaan:</i> Tambahkan alias <tt>typedef proto_msg_t msg_type_t;</tt> untuk menyelaraskan dengan kode di modul panduan PDF."
        ],
        bg_rgb=(0.95, 0.98, 0.96), border_rgb=(0.6, 0.85, 0.65)
    )

    doc.draw_card(
        "ANGGOTA 1 (Socket Core & Pacer) — Progres: ~30% (Perlu Dikejar)",
        [
            "<b>Sudah Selesai:</b> <tt>src/common/io_utils.c</tt> dan <tt>include/io_utils.h</tt> (<tt>readn()</tt> dan <tt>writen()</tt> dengan penanganan short read/write &amp; <tt>EINTR</tt>), serta <tt>Makefile</tt> terstruktur.",
            "<b>Belum Selesai (Prioritas Utama):</b>",
            "• <tt>src/server/server.c</tt>: Inisialisasi TCP socket (port 8888), concurrent worker pool (<tt>pthread_create</tt> + <tt>pthread_detach</tt>), parser pesan masuk (<tt>HELLO</tt>, <tt>ECHO</tt>, <tt>PLAY</tt>).",
            "• Implementasi fungsi inti <tt>stream_wav_file()</tt> untuk kirim 44B WAV header (<tt>seq=0</tt>) dilanjutkan blok PCM 8.820B (<tt>seq &gt;= 1</tt>) dan <tt>MSG_END</tt>.",
            "• <tt>src/server/pacing.c</tt>: Implementasi awal <tt>pacer_wait_next()</tt> dengan <tt>clock_nanosleep()</tt> untuk persiapan Sesi 2."
        ],
        bg_rgb=(0.99, 0.97, 0.93), border_rgb=(0.95, 0.82, 0.55)
    )

    doc.draw_card(
        "ANGGOTA 3 (Klien Referensi, Pengujian, & Captures) — Progres: ~10% (Kritis)",
        [
            "<b>Belum Selesai (Tugas Kunci):</b>",
            "• <tt>src/client/client.c</tt>: Klien referensi C untuk mode <tt>echo</tt> (uji pantul) dan mode <tt>get</tt> (mengunduh chunk audio WAV dan menuliskan ke file lokal).",
            "• <tt>media/generate_wav.sh</tt>: Skrip deterministik ffmpeg pembuat <tt>sample_30s.wav</tt> (5.292.044 byte) dan <tt>sample_5m.wav</tt> (44.1kHz 16-bit Stereo).",
            "• <tt>tests/test_framing.sh</tt> &amp; <tt>tests/test_md5.sh</tt>: Pengujian otomatis 5 klien serentak membuktikan checksum MD5 100% identik.",
            "• <tt>captures/</tt>: Perekaman paket <tt>session1_stream.pcap</tt> via tcpdump untuk artefak penilaian 20%."
        ],
        bg_rgb=(0.99, 0.94, 0.94), border_rgb=(0.92, 0.65, 0.65)
    )

    # Section 3: Action Plan & Hygiene
    doc.draw_markup(
        "<span font_desc='Sans Bold 12' foreground='#1A365D'>3. Temuan Anomali Repositori &amp; Rencana Aksi Segera</span>\n"
        "<span font_desc='Sans 8' foreground='#718096'>Langkah teknis mendesak untuk menstabilkan repositori dan mengejar target Sesi 1:</span>",
        line_spacing=6
    )

    doc.draw_card(
        "ACTION PLAN — PEMBERSIHAN HIGIENE & PENYELESAIAN KODE",
        [
            "<b>1. Selesaikan Merge Conflict Git:</b> Berkas <tt>.gitignore</tt> saat ini memiliki conflict markers (<tt>&lt;&lt;&lt;&lt;&lt;&lt;&lt; HEAD</tt>). Perlu dibersihkan dan distandarkan.",
            "<b>2. Standarisasi Dokumentasi:</b> Sinkronkan isi <tt>readme.md</tt> (bersihkan instruksi prompt) ke <tt>README.md</tt> canonical.",
            "<b>3. Pindahkan Dokumen Spesifikasi:</b> Lakukan <tt>git add docs/protocol_spec_v1.md</tt> agar resmi masuk riwayat commit Git.",
            "<b>4. Implementasi Server &amp; Client C11:</b> Lengkapi <tt>server.c</tt> dan <tt>client.c</tt> agar <tt>make all</tt> berhasil dikompilasi tanpa error.",
            "<b>5. Jalankan Pengujian MD5:</b> Eksekusi pembuatan WAV, jalankan streaming 5 klien, dan lakukan verifikasi <tt>md5sum output_*.wav == sample_30s.wav</tt>."
        ],
        bg_rgb=(0.95, 0.97, 0.99), border_rgb=(0.75, 0.82, 0.90)
    )

    doc.close()
    print(f"Laporan PDF berhasil dibuat di: {out_path}")

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'Laporan_Evaluasi_Sesi_1_PodFlow.pdf'
    build_pdf(out)
