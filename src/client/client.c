#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>

#include "protocol.h"
#include "io_utils.h"

#define MAX_PAYLOAD_BUF 65536

/* LCG Pseudo-random generator untuk membuat payload echo deterministik */
static uint8_t generate_byte(uint32_t *state) {
    *state = (*state * 1103515245 + 12345) & 0x7fffffff;
    return (uint8_t)(*state & 0xFF);
}

/* Membuka koneksi socket TCP ke Host & Port */
static int connect_to_server(const char *host, const char *port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(host, port, &hints, &res);
    if (status != 0) {
        fprintf(stderr, "[-] getaddrinfo error: %s\n", gai_strerror(status));
        return -1;
    }

    int sockfd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1) continue;

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            break; /* Sukses terhubung */
        }
        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(res);
    return sockfd;
}

/* Melakukan Handshake HELLO -> WELCOME */
static int perform_handshake(int sockfd) {
    const char *client_name = "m3-client";
    if (send_frame(sockfd, MSG_HELLO, 0, 0, client_name, strlen(client_name)) < 0) {
        fprintf(stderr, "[-] Gagal mengirim frame HELLO\n");
        return -1;
    }

    frame_hdr_t hdr;
    uint8_t payload[MAX_PAYLOAD_BUF];
    int res = recv_frame(sockfd, &hdr, payload, sizeof(payload));
    if (res <= 0 || hdr.msg_type != MSG_WELCOME) {
        fprintf(stderr, "[-] Handshake gagal: WELCOME tidak diterima\n");
        return -1;
    }

    return 0;
}

/* MODE 1: ECHO TEST */
static int run_echo_mode(const char *host, const char *port, int count) {
    int sockfd = connect_to_server(host, port);
    if (sockfd < 0) return 1;

    if (perform_handshake(sockfd) < 0) {
        close(sockfd);
        return 1;
    }

    const uint32_t payload_sizes[] = {0, 1, 15, 16, 17, 1000, 8820, 65536};
    size_t num_sizes = sizeof(payload_sizes) / sizeof(payload_sizes[0]);

    uint8_t send_buf[MAX_PAYLOAD_BUF];
    uint8_t recv_buf[MAX_PAYLOAD_BUF];

    for (int i = 0; i < count; i++) {
        uint32_t p_size = payload_sizes[i % num_sizes];
        uint32_t lcg_state = (uint32_t)i + 1;

        for (uint32_t b = 0; b < p_size; b++) {
            send_buf[b] = generate_byte(&lcg_state);
        }

        uint32_t seq = (uint32_t)i;
        uint32_t param = (uint32_t)(i * 7);

        if (send_frame(sockfd, MSG_ECHO, seq, param, send_buf, p_size) < 0) {
            fprintf(stderr, "[-] Gagal mengirim ECHO frame %d\n", i);
            close(sockfd);
            return 1;
        }

        frame_hdr_t hdr;
        int ret = recv_frame(sockfd, &hdr, recv_buf, sizeof(recv_buf));
        if (ret <= 0) {
            fprintf(stderr, "[-] Gagal menerima balasan ECHO frame %d\n", i);
            close(sockfd);
            return 1;
        }

        /* Verifikasi Integritas Echo */
        if (hdr.msg_type != MSG_ECHO || hdr.seq_num != seq || 
            hdr.param != param || hdr.payload_len != p_size ||
            memcmp(send_buf, recv_buf, p_size) != 0) {
            fprintf(stderr, "[-] MISMATCH pada ECHO frame %d!\n", i);
            close(sockfd);
            return 1;
        }
    }

    printf("ECHO OK: %d/%d\n", count, count);
    close(sockfd);
    return 0;
}

/* MODE 2: GET / STREAM MODE */
static int run_get_mode(const char *host, const char *port, const char *remote_file, const char *out_file) {
    int sockfd = connect_to_server(host, port);
    if (sockfd < 0) return 1;

    if (perform_handshake(sockfd) < 0) {
        close(sockfd);
        return 1;
    }

    /* Mengirim perintah PLAY */
    if (send_frame(sockfd, MSG_PLAY, 0, 0, remote_file, strlen(remote_file)) < 0) {
        fprintf(stderr, "[-] Gagal mengirim perintah PLAY\n");
        close(sockfd);
        return 1;
    }

    FILE *fp = fopen(out_file, "wb");
    if (!fp) {
        perror("[-] Gagal membuka file output");
        close(sockfd);
        return 1;
    }

    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    uint32_t expected_seq = 0;
    uint64_t total_bytes_received = 0;
    uint32_t total_chunks = 0;

    uint8_t payload[MAX_PAYLOAD_BUF];
    frame_hdr_t hdr;

    while (1) {
        int ret = recv_frame(sockfd, &hdr, payload, sizeof(payload));
        if (ret <= 0) {
            fprintf(stderr, "[-] Error I/O atau koneksi terputus prematur\n");
            fclose(fp);
            close(sockfd);
            return 1;
        }

        /* Jika Server Mengembalikan Error */
        if (hdr.msg_type == MSG_ERROR) {
            fprintf(stderr, "[-] Server Error Code %u: %.*s\n", 
                    hdr.param, (int)hdr.payload_len, (char*)payload);
            fclose(fp);
            close(sockfd);
            return 1;
        }

        /* Memproses Frame DATA */
        if (hdr.msg_type == MSG_DATA) {
            if (hdr.seq_num != expected_seq) {
                fprintf(stderr, "[-] Sequence mismatch! Expected %u, got %u\n", expected_seq, hdr.seq_num);
                fclose(fp);
                close(sockfd);
                return 1;
            }

            if (fwrite(payload, 1, hdr.payload_len, fp) != hdr.payload_len) {
                fprintf(stderr, "[-] Disk write error!\n");
                fclose(fp);
                close(sockfd);
                return 1;
            }

            total_bytes_received += hdr.payload_len;
            total_chunks++;
            expected_seq++;
        } 
        /* Memproses Frame END (Transfer Selesai) */
        else if (hdr.msg_type == MSG_END) {
            clock_gettime(CLOCK_MONOTONIC, &end_time);
            double elapsed = (end_time.tv_sec - start_time.tv_sec) + 
                             (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

            if (hdr.param != total_bytes_received) {
                fprintf(stderr, "[-] Byte count mismatch! END.param=%u, WriteTotal=%lu\n", 
                        hdr.param, (unsigned long)total_bytes_received);
                fclose(fp);
                close(sockfd);
                return 1;
            }

            printf("[+] STREAM COMPLETED: %lu bytes (%u chunks) in %.3f s (%.2f MB/s)\n",
                   (unsigned long)total_bytes_received, total_chunks, elapsed, 
                   (double)total_bytes_received / (1024.0 * 1024.0 * elapsed));

            fclose(fp);
            close(sockfd);
            return 0;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s echo <host> <port> <count>\n", argv[0]);
        fprintf(stderr, "  %s get  <host> <port> <file> <out>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "echo") == 0) {
        if (argc != 5) return 1;
        int count = atoi(argv[4]);
        return run_echo_mode(argv[2], argv[3], count);
    } else if (strcmp(argv[1], "get") == 0) {
        if (argc != 6) return 1;
        return run_get_mode(argv[2], argv[3], argv[4], argv[5]);
    } else {
        fprintf(stderr, "[-] Unknown mode: %s\n", argv[1]);
        return 1;
    }
}