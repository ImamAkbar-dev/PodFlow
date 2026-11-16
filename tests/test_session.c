/**
 * @file test_session.c
 * @brief Comprehensive state-machine, seek offset math, partial-chunk EOF,
 *        path-traversal security, pause/resume continuity, and
 * seek-while-paused tests for stream_session.c.
 *
 * Owner: Member 2 (Protocol, Ring Buffer, Stream Session)
 */

#include "stream_session.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TEST_MEDIA_DIR "./test_media_tmp"

#define RUN_TEST(fn)                                                           \
  do {                                                                         \
    printf("  [RUN ] %-48s ... ", #fn);                                        \
    fflush(stdout);                                                            \
    fn();                                                                      \
    printf("PASS\n");                                                          \
  } while (0)

/* ========================================================================== *
 * HELPER: Generate Deterministic Test WAV Files                              *
 *   Each byte at PCM offset `k` (0-based) is set to a deterministic pattern  *
 *   based on its chunk index (1-based) and offset within the chunk so tests  *
 *   can verify exact seek offset math down to the individual byte!           *
 * ========================================================================== */

static void write_u16_le(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static void write_u32_le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8) & 0xFFU);
  p[2] = (uint8_t)((v >> 16) & 0xFFU);
  p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint8_t expected_pcm_byte(uint32_t chunk_seq, uint32_t byte_in_chunk) {
  return (uint8_t)(((chunk_seq * 37U) + (byte_in_chunk * 13U) + 0x5AU) & 0xFFU);
}

static void create_test_wav(const char *filename, uint32_t pcm_bytes) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", TEST_MEDIA_DIR, filename);

  FILE *fp = fopen(path, "wb");
  assert(fp != NULL);

  uint8_t hdr[WAV_HEADER_SIZE];
  memset(hdr, 0, sizeof(hdr));
  memcpy(hdr + 0, "RIFF", 4);
  write_u32_le(hdr + 4, 36U + pcm_bytes);
  memcpy(hdr + 8, "WAVE", 4);
  memcpy(hdr + 12, "fmt ", 4);
  write_u32_le(hdr + 16, 16U);     /* Subchunk1Size = 16 (PCM)     */
  write_u16_le(hdr + 20, 1U);      /* AudioFormat = 1 (PCM)        */
  write_u16_le(hdr + 22, 2U);      /* NumChannels = 2 (Stereo)     */
  write_u32_le(hdr + 24, 44100U);  /* SampleRate = 44100 Hz        */
  write_u32_le(hdr + 28, 176400U); /* ByteRate = 44100 * 2 * 2     */
  write_u16_le(hdr + 32, 4U);      /* BlockAlign = 4               */
  write_u16_le(hdr + 34, 16U);     /* BitsPerSample = 16           */
  memcpy(hdr + 36, "data", 4);
  write_u32_le(hdr + 40, pcm_bytes);

  assert(fwrite(hdr, 1, WAV_HEADER_SIZE, fp) == WAV_HEADER_SIZE);

  uint8_t chunk[PCM_CHUNK_SIZE];
  uint32_t remaining = pcm_bytes;
  uint32_t seq = 1;
  while (remaining > 0) {
    uint32_t n = (remaining > PCM_CHUNK_SIZE) ? PCM_CHUNK_SIZE : remaining;
    for (uint32_t i = 0; i < n; i++) {
      chunk[i] = expected_pcm_byte(seq, i);
    }
    assert(fwrite(chunk, 1, n, fp) == n);
    remaining -= n;
    seq++;
  }

  fclose(fp);
}

static void create_corrupt_wav(const char *filename) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", TEST_MEDIA_DIR, filename);
  FILE *fp = fopen(path, "wb");
  assert(fp != NULL);
  uint8_t bad[100];
  memset(bad, 'X', sizeof(bad));
  assert(fwrite(bad, 1, sizeof(bad), fp) == sizeof(bad));
  fclose(fp);
}

/* Helper that polls session_next() with a short sleep until an item arrives */
static void wait_next_item(stream_session_t *s, session_item_t *out) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 500000L}; /* 0.5 ms */
  for (int tries = 0; tries < 4000; tries++) {
    if (session_next(s, out) == 1) {
      return;
    }
    nanosleep(&ts, NULL);
  }
  assert(!"Timed out waiting for session_next() item");
}

/* Verify that a popped chunk's bytes match the file's offset for `expected_seq`
 */
static void verify_chunk_content(const session_item_t *item,
                                 uint32_t expected_seq, uint32_t expected_len) {
  assert(item->kind == ITEM_CHUNK);
  assert(item->seq == expected_seq);
  assert(item->len == expected_len);
  for (uint32_t i = 0; i < expected_len; i++) {
    assert(item->data[i] == expected_pcm_byte(expected_seq, i));
  }
}

/* ========================================================================== *
 * TEST 1: Path Traversal, Missing File (200), and Invalid WAV (201)          *
 * ========================================================================== */
static void test_security_and_validation_errors(void) {
  stream_session_t *s = session_create(TEST_MEDIA_DIR);
  assert(s != NULL);
  uint32_t ack = 0;

  /* Path traversal and illegal characters -> ERR_FILE_NOT_FOUND (200) */
  assert(session_play(s, "../Makefile", 11, 0, &ack) == ERR_FILE_NOT_FOUND);
  assert(session_play(s, "..", 2, 0, &ack) == ERR_FILE_NOT_FOUND);
  assert(session_play(s, "/etc/passwd", 11, 0, &ack) == ERR_FILE_NOT_FOUND);
  assert(session_play(s, "sub/file.wav", 12, 0, &ack) == ERR_FILE_NOT_FOUND);
  assert(session_play(s, "nonexistent.wav", 15, 0, &ack) == ERR_FILE_NOT_FOUND);
  assert(session_state(s) == SESS_IDLE);

  /* Corrupt WAV header -> ERR_INVALID_WAV (201) */
  assert(session_play(s, "corrupt.wav", 11, 0, &ack) == ERR_INVALID_WAV);
  assert(session_state(s) == SESS_IDLE);

  session_destroy(s);
}

/* ========================================================================== *
 * TEST 2: Every State-Machine Transition & Wrong-State Error (300)           *
 * ========================================================================== */
static void test_all_state_transitions(void) {
  stream_session_t *s = session_create(TEST_MEDIA_DIR);
  assert(s != NULL);
  uint32_t ack = 999;
  session_item_t item;

  /* --- In SESS_IDLE --- */
  assert(session_state(s) == SESS_IDLE);
  assert(session_next(s, &item) == 0);
  assert(session_pause(s, &ack) == ERR_INVALID_STATE);
  assert(session_seek(s, 1, &ack) == ERR_INVALID_STATE);
  assert(session_stop(s, &ack) == ERR_INVALID_STATE);
  /* Resume (empty filename) in IDLE -> ERR_INVALID_STATE (300) */
  assert(session_play(s, "", 0, 0, &ack) == ERR_INVALID_STATE);

  /* Start chunk out of range (10-chunk file, start_chunk = 11) -> 202, stays
   * IDLE */
  assert(session_play(s, "ten_chunks.wav", 14, 11, &ack) ==
         ERR_SEEK_OUT_OF_RANGE);
  assert(session_state(s) == SESS_IDLE);

  /* Valid PLAY from IDLE -> SESS_PLAYING */
  assert(session_play(s, "ten_chunks.wav", 14, 0, &ack) == ERR_NONE);
  assert(ack == 1U);
  assert(session_state(s) == SESS_PLAYING);

  /* --- In SESS_PLAYING --- */
  /* PLAY with filename or empty payload while PLAYING -> 300 */
  assert(session_play(s, "ten_chunks.wav", 14, 0, &ack) == ERR_INVALID_STATE);
  assert(session_play(s, "", 0, 0, &ack) == ERR_INVALID_STATE);

  /* Pop WAV header (seq=0) and chunk 1 */
  wait_next_item(s, &item);
  assert(item.kind == ITEM_HEADER && item.seq == 0 &&
         item.len == WAV_HEADER_SIZE);
  wait_next_item(s, &item);
  verify_chunk_content(&item, 1, PCM_CHUNK_SIZE);

  /* PAUSE in PLAYING -> SESS_PAUSED, ack_param = next_send_seq (2) */
  assert(session_pause(s, &ack) == ERR_NONE);
  assert(ack == 2U);
  assert(session_state(s) == SESS_PAUSED);

  /* --- In SESS_PAUSED --- */
  /* session_next() must return 0 while paused */
  assert(session_next(s, &item) == 0);
  /* PAUSE when already paused -> 300 */
  assert(session_pause(s, &ack) == ERR_INVALID_STATE);
  /* PLAY with filename when paused -> 300 */
  assert(session_play(s, "ten_chunks.wav", 14, 0, &ack) == ERR_INVALID_STATE);

  /* RESUME (empty filename) in PAUSED -> SESS_PLAYING, ack_param = 2 */
  assert(session_play(s, NULL, 0, 0, &ack) == ERR_NONE);
  assert(ack == 2U);
  assert(session_state(s) == SESS_PLAYING);

  /* Pop chunk 2 */
  wait_next_item(s, &item);
  verify_chunk_content(&item, 2, PCM_CHUNK_SIZE);

  /* STOP in PLAYING -> SESS_IDLE, ack_param = last_sent_seq (2), no END */
  assert(session_stop(s, &ack) == ERR_NONE);
  assert(ack == 2U);
  assert(session_state(s) == SESS_IDLE);
  assert(session_next(s, &item) == 0);

  session_destroy(s);
}

/* ========================================================================== *
 * TEST 3: Seek Offset Math at Chunk 1, Middle (5), Last (10), and Last+1     *
 * ========================================================================== */
static void test_seek_offset_math_and_bounds(void) {
  stream_session_t *s = session_create(TEST_MEDIA_DIR);
  assert(s != NULL);
  uint32_t ack = 0;
  session_item_t item;

  assert(session_play(s, "ten_chunks.wav", 14, 1, &ack) == ERR_NONE);
  assert(ack == 1U);

  /* Consume header and chunk 1 */
  wait_next_item(s, &item);
  assert(item.kind == ITEM_HEADER);
  wait_next_item(s, &item);
  verify_chunk_content(&item, 1, PCM_CHUNK_SIZE);

  /* Out-of-range seeks: chunk 0 and chunk 11 -> 202, state unchanged */
  assert(session_seek(s, 0, &ack) == ERR_SEEK_OUT_OF_RANGE);
  assert(session_seek(s, 11, &ack) == ERR_SEEK_OUT_OF_RANGE);
  assert(session_state(s) == SESS_PLAYING);

  /* Seek to middle chunk (chunk 5) -> header is NOT resent, next chunk is 5 */
  assert(session_seek(s, 5, &ack) == ERR_NONE);
  assert(ack == 5U);
  wait_next_item(s, &item);
  verify_chunk_content(&item, 5, PCM_CHUNK_SIZE);

  /* Seek to last chunk (chunk 10) */
  assert(session_seek(s, 10, &ack) == ERR_NONE);
  assert(ack == 10U);
  wait_next_item(s, &item);
  verify_chunk_content(&item, 10, PCM_CHUNK_SIZE);

  /* Seek back to chunk 1 */
  assert(session_seek(s, 1, &ack) == ERR_NONE);
  assert(ack == 1U);
  wait_next_item(s, &item);
  verify_chunk_content(&item, 1, PCM_CHUNK_SIZE);

  /* Seek again to chunk 10 and let it hit EOF -> ITEM_END (seq=11) */
  assert(session_seek(s, 10, &ack) == ERR_NONE);
  wait_next_item(s, &item);
  verify_chunk_content(&item, 10, PCM_CHUNK_SIZE);

  wait_next_item(s, &item);
  assert(item.kind == ITEM_END);
  assert(item.seq == 11U);
  /* We popped header (44) + 5 full chunks (1, 5, 10, 1, 10) = 44 + 5 * 8820 */
  assert(item.param == WAV_HEADER_SIZE + 5U * PCM_CHUNK_SIZE);
  assert(session_state(s) == SESS_IDLE);

  session_destroy(s);
}

/* ========================================================================== *
 * TEST 4: Pause/Resume Continuity & Seek While Paused                        *
 * ========================================================================== */
static void test_pause_resume_and_seek_while_paused(void) {
  stream_session_t *s = session_create(TEST_MEDIA_DIR);
  assert(s != NULL);
  uint32_t ack = 0;
  session_item_t item;

  assert(session_play(s, "ten_chunks.wav", 14, 0, &ack) == ERR_NONE);
  wait_next_item(s, &item);
  assert(item.kind == ITEM_HEADER);

  /* Read chunks 1..4 */
  for (uint32_t seq = 1; seq <= 4; seq++) {
    wait_next_item(s, &item);
    verify_chunk_content(&item, seq, PCM_CHUNK_SIZE);
  }

  /* Pause after chunk 4 -> ack_param must be 5 */
  assert(session_pause(s, &ack) == ERR_NONE);
  assert(ack == 5U);
  assert(session_state(s) == SESS_PAUSED);

  /* Wait 20 ms while paused so producer fills the ring buffer in background */
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 20000000L};
  nanosleep(&ts, NULL);
  assert(session_next(s, &item) == 0);

  /* Resume -> must continue at chunk 5 with zero lost or duplicated chunks */
  assert(session_play(s, "", 0, 0, &ack) == ERR_NONE);
  assert(ack == 5U);
  wait_next_item(s, &item);
  verify_chunk_content(&item, 5, PCM_CHUNK_SIZE);
  wait_next_item(s, &item);
  verify_chunk_content(&item, 6, PCM_CHUNK_SIZE);

  /* Pause again (next is 7), then SEEK to chunk 9 while paused */
  assert(session_pause(s, &ack) == ERR_NONE);
  assert(ack == 7U);
  assert(session_seek(s, 9, &ack) == ERR_NONE);
  assert(ack == 9U);
  /* Must remain in SESS_PAUSED after seek-while-paused! */
  assert(session_state(s) == SESS_PAUSED);
  assert(session_next(s, &item) == 0);

  /* Now resume -> ack must be 9, and chunks 9 and 10 arrive followed by END */
  assert(session_play(s, "", 0, 0, &ack) == ERR_NONE);
  assert(ack == 9U);
  wait_next_item(s, &item);
  verify_chunk_content(&item, 9, PCM_CHUNK_SIZE);
  wait_next_item(s, &item);
  verify_chunk_content(&item, 10, PCM_CHUNK_SIZE);
  wait_next_item(s, &item);
  assert(item.kind == ITEM_END && item.seq == 11U);
  assert(session_state(s) == SESS_IDLE);

  session_destroy(s);
}

/* ========================================================================== *
 * TEST 5: Partial-Chunk Tail Files (5,448-byte tail & 3,528-byte tail)       *
 *         and END.param Exact Byte Accounting                                *
 * ========================================================================== */
static void verify_tail_file_stream(const char *filename,
                                    uint32_t total_pcm_bytes,
                                    uint32_t expected_total_chunks,
                                    uint32_t expected_last_chunk_len) {
  stream_session_t *s = session_create(TEST_MEDIA_DIR);
  assert(s != NULL);
  uint32_t ack = 0;
  session_item_t item;

  assert(session_play(s, filename, (uint32_t)strlen(filename), 0, &ack) ==
         ERR_NONE);
  assert(ack == 1U);

  wait_next_item(s, &item);
  assert(item.kind == ITEM_HEADER && item.seq == 0 &&
         item.len == WAV_HEADER_SIZE);

  uint32_t total_popped_bytes = item.len;

  for (uint32_t seq = 1; seq <= expected_total_chunks; seq++) {
    wait_next_item(s, &item);
    uint32_t exp_len = (seq < expected_total_chunks) ? PCM_CHUNK_SIZE
                                                     : expected_last_chunk_len;
    verify_chunk_content(&item, seq, exp_len);
    total_popped_bytes += item.len;
  }

  wait_next_item(s, &item);
  assert(item.kind == ITEM_END);
  assert(item.seq == expected_total_chunks + 1U);
  assert(item.param == WAV_HEADER_SIZE + total_pcm_bytes);
  assert(item.param == total_popped_bytes);
  assert(session_state(s) == SESS_IDLE);

  session_destroy(s);
}

static void test_partial_chunk_eof_and_end_param(void) {
  /* Case A: 47 * 8820 + 5448 = 419,988 PCM bytes -> 48 chunks, last = 5,448 B
   */
  verify_tail_file_stream("tail_5448.wav", 47U * PCM_CHUNK_SIZE + 5448U, 48U,
                          5448U);

  /* Case B: 418,068 PCM bytes (2.37s) = 47 * 8820 + 3528 -> 48 chunks, last =
   * 3,528 B */
  verify_tail_file_stream("tail_3528.wav", 418068U, 48U, 3528U);
}

int main(void) {
  printf(
      "====================================================================\n");
  printf("PodFlow Session 2+3 — Stream Session Test Suite (Member 2)\n");
  printf(
      "====================================================================\n");

  mkdir(TEST_MEDIA_DIR, 0755);
  create_test_wav("ten_chunks.wav", 10U * PCM_CHUNK_SIZE);
  create_test_wav("tail_5448.wav", 47U * PCM_CHUNK_SIZE + 5448U);
  create_test_wav("tail_3528.wav", 418068U);
  create_corrupt_wav("corrupt.wav");

  RUN_TEST(test_security_and_validation_errors);
  RUN_TEST(test_all_state_transitions);
  RUN_TEST(test_seek_offset_math_and_bounds);
  RUN_TEST(test_pause_resume_and_seek_while_paused);
  RUN_TEST(test_partial_chunk_eof_and_end_param);

  /* Cleanup temporary test media files */
  unlink(TEST_MEDIA_DIR "/ten_chunks.wav");
  unlink(TEST_MEDIA_DIR "/tail_5448.wav");
  unlink(TEST_MEDIA_DIR "/tail_3528.wav");
  unlink(TEST_MEDIA_DIR "/corrupt.wav");
  rmdir(TEST_MEDIA_DIR);

  printf(
      "====================================================================\n");
  printf("ALL 5 STREAM SESSION TEST SUITES PASSED SUCCESSFULLY.\n");
  printf(
      "====================================================================\n");
  return 0;
}