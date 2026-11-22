#define _POSIX_C_SOURCE 200809L
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "protocol.h"
#include "io_utils.h"

#ifndef ERR_SERVER_BUSY
#define ERR_SERVER_BUSY 503
#endif

#define MAX_PAYLOAD_BUF 65536
#define MAX_SCRIPT_CMDS 64

typedef enum {
    CMD_PAUSE,
    CMD_PLAY,
    CMD_SEEK,
    CMD_STOP
} script_cmd_type_t;

typedef struct {
    double trigger_sec;
    script_cmd_type_t type;
    uint32_t arg;
    bool executed;
} script_action_t;

/* Ambil waktu CLOCK_MONOTONIC dalam nanodetik */
static uint64_t now_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* LCG deterministik untuk payload mode echo */
static uint8_t next_lcg_byte(uint32_t *state) {
    *state = (*state * 1103515245U + 12345U) & 0x7fffffffU;
    return (uint8_t)(*state & 0xFFU);
}

/* Tulis baris event ke file CSV jika --csv aktif */
static void log_csv(FILE *csv_fp, const char *kind, uint32_t seq, uint64_t t_ns, uint32_t len, uint32_t param) {
    if (!csv_fp) return;
    fprintf(csv_fp, "%s,%u,%lu,%u,%u\n", kind, seq, (unsigned long)t_ns, len, param);
}

/* Hubungkan socket TCP ke server */
static int connect_to_server(const char *host, const char *port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(host, port, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "[-] getaddrinfo(%s:%s): %s\n", host, port, gai_strerror(err));
        return -1;
    }

    int sockfd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd < 0) continue;

        int flag = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(res);
    if (sockfd < 0) {
        fprintf(stderr, "[-] Could not connect to %s:%s\n", host, port);
    }
    return sockfd;
}

/* Handshake HELLO -> WELCOME (menerima payload WELCOME apapun) */
static int do_handshake(int sockfd, FILE *csv_fp) {
    const char *hello_name = "podflow-m3-client";
    if (send_frame(sockfd, MSG_HELLO, 0, 0, hello_name, (uint32_t)strlen(hello_name)) < 0) {
        fprintf(stderr, "[-] Failed to send HELLO\n");
        return -1;
    }

    frame_hdr_t hdr;
    uint8_t payload[MAX_PAYLOAD_BUF];
    int rc = recv_frame(sockfd, &hdr, payload, sizeof(payload));
    if (rc <= 0) {
        fprintf(stderr, "[-] Failed to receive WELCOME (rc=%d)\n", rc);
        return -1;
    }
    if (hdr.msg_type == MSG_ERROR) {
        log_csv(csv_fp, "ERROR", hdr.seq_num, now_mono_ns(), hdr.payload_len, hdr.param);
        fprintf(stderr, "[-] Server rejected handshake with ERROR %u: %.*s\n",
                hdr.param, (int)hdr.payload_len, (char *)payload);
        return -1;
    }
    if (hdr.msg_type != MSG_WELCOME) {
        fprintf(stderr, "[-] Expected WELCOME (0x02), got 0x%02x\n", hdr.msg_type);
        return -1;
    }
    return 0;
}

/* Parse string --script "t:cmd,t:cmd,..." */
static int parse_script(const char *script_str, script_action_t *actions, int max_actions) {
    if (!script_str || strlen(script_str) == 0) return 0;

    char *copy = strdup(script_str);
    if (!copy) return -1;

    int count = 0;
    char *saveptr = NULL;
    char *token = strtok_r(copy, ",", &saveptr);

    while (token != NULL) {
        if (count >= max_actions) {
            fprintf(stderr, "[-] Too many script actions (max %d)\n", max_actions);
            free(copy);
            return -1;
        }

        char *colon = strchr(token, ':');
        if (!colon) {
            fprintf(stderr, "[-] Invalid script token '%s' (expected t:cmd)\n", token);
            free(copy);
            return -1;
        }

        *colon = '\0';
        double t_val = atof(token);
        const char *cmd_str = colon + 1;

        actions[count].trigger_sec = t_val;
        actions[count].executed = false;
        actions[count].arg = 0;

        if (strcmp(cmd_str, "pause") == 0) {
            actions[count].type = CMD_PAUSE;
        } else if (strcmp(cmd_str, "play") == 0) {
            actions[count].type = CMD_PLAY;
        } else if (strcmp(cmd_str, "stop") == 0) {
            actions[count].type = CMD_STOP;
        } else if (strncmp(cmd_str, "seek=", 5) == 0) {
            actions[count].type = CMD_SEEK;
            actions[count].arg = (uint32_t)strtoul(cmd_str + 5, NULL, 10);
        } else {
            fprintf(stderr, "[-] Unknown script command '%s'\n", cmd_str);
            free(copy);
            return -1;
        }

        count++;
        token = strtok_r(NULL, ",", &saveptr);
    }

    free(copy);
    return count;
}

/* MODE 1: ECHO */
static int run_echo_mode(const char *host, const char *port, int count) {
    if (count <= 0) {
        fprintf(stderr, "[-] Echo count must be > 0\n");
        return 1;
    }

    int sockfd = connect_to_server(host, port);
    if (sockfd < 0) return 1;

    if (do_handshake(sockfd, NULL) < 0) {
        close(sockfd);
        return 1;
    }

    const uint32_t sizes[] = {0, 1, 15, 16, 17, 1000, 8820, 65536};
    const size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    uint8_t *tx_buf = malloc(MAX_PAYLOAD_BUF);
    uint8_t *rx_buf = malloc(MAX_PAYLOAD_BUF);
    if (!tx_buf || !rx_buf) {
        perror("malloc");
        free(tx_buf);
        free(rx_buf);
        close(sockfd);
        return 1;
    }

    for (int i = 0; i < count; i++) {
        uint32_t len = sizes[(size_t)i % num_sizes];
        uint32_t seed = (uint32_t)(i + 1);
        for (uint32_t b = 0; b < len; b++) {
            tx_buf[b] = next_lcg_byte(&seed);
        }

        uint32_t seq = (uint32_t)i;
        uint32_t param = (uint32_t)(i * 13);

        if (send_frame(sockfd, MSG_ECHO, seq, param, tx_buf, len) < 0) {
            fprintf(stderr, "[-] send_frame failed at ECHO %d\n", i);
            free(tx_buf);
            free(rx_buf);
            close(sockfd);
            return 1;
        }

        frame_hdr_t hdr;
        int rc = recv_frame(sockfd, &hdr, rx_buf, MAX_PAYLOAD_BUF);
        if (rc <= 0) {
            fprintf(stderr, "[-] recv_frame failed at ECHO %d (rc=%d)\n", i, rc);
            free(tx_buf);
            free(rx_buf);
            close(sockfd);
            return 1;
        }

        if (hdr.msg_type != MSG_ECHO || hdr.seq_num != seq ||
            hdr.param != param || hdr.payload_len != len ||
            (len > 0 && memcmp(tx_buf, rx_buf, len) != 0)) {
            fprintf(stderr, "[-] ECHO mismatch at frame %d!\n", i);
            free(tx_buf);
            free(rx_buf);
            close(sockfd);
            return 1;
        }
    }

    printf("ECHO OK: %d/%d\n", count, count);
    free(tx_buf);
    free(rx_buf);
    close(sockfd);
    return 0;
}

/* MODE 2: PLAY / GET */
static int run_play_mode(const char *host, const char *port, const char *remote_file,
                         const char *out_path, const char *script_str, const char *csv_path) {
    script_action_t actions[MAX_SCRIPT_CMDS];
    int num_actions = parse_script(script_str, actions, MAX_SCRIPT_CMDS);
    if (num_actions < 0) return 1;

    FILE *csv_fp = NULL;
    if (csv_path) {
        csv_fp = fopen(csv_path, "w");
        if (!csv_fp) {
            perror("[-] Failed to open CSV file");
            return 1;
        }
        fprintf(csv_fp, "kind,seq,t_mono_ns,len,param\n");
    }

    int sockfd = connect_to_server(host, port);
    if (sockfd < 0) {
        if (csv_fp) fclose(csv_fp);
        return 1;
    }

    if (do_handshake(sockfd, csv_fp) < 0) {
        close(sockfd);
        if (csv_fp) fclose(csv_fp);
        return 1;
    }

    FILE *out_fp = fopen(out_path, "wb");
    if (!out_fp) {
        perror("[-] Failed to open output WAV file");
        close(sockfd);
        if (csv_fp) fclose(csv_fp);
        return 1;
    }

    /* Kirim command awal PLAY (start_chunk = 0 -> dari awal) */
    uint32_t cmd_seq = 1;
    log_csv(csv_fp, "CMD", cmd_seq, now_mono_ns(), (uint32_t)strlen(remote_file), 0);
    if (send_frame(sockfd, MSG_PLAY, cmd_seq++, 0, remote_file, (uint32_t)strlen(remote_file)) < 0) {
        fprintf(stderr, "[-] Failed to send initial PLAY\n");
        fclose(out_fp);
        close(sockfd);
        if (csv_fp) fclose(csv_fp);
        return 1;
    }

    bool waiting_for_ack = true;
    uint8_t last_cmd_type = MSG_PLAY;
    bool got_first_data = false;
    uint64_t first_data_ns = 0;

    uint32_t expected_seq = 0;
    uint64_t total_payload_bytes = 0;
    uint32_t total_data_frames = 0;
    uint32_t error_count = 0;

    uint8_t payload[MAX_PAYLOAD_BUF];
    frame_hdr_t hdr;
    int exit_code = 0;

    while (1) {
        /* Hitung timeout menuju aksi --script berikutnya */
        int poll_timeout_ms = -1;
        if (got_first_data) {
            uint64_t now = now_mono_ns();
            double elapsed_sec = (double)(now - first_data_ns) / 1e9;
            double min_rem_sec = -1.0;

            for (int i = 0; i < num_actions; i++) {
                if (!actions[i].executed) {
                    double rem = actions[i].trigger_sec - elapsed_sec;
                    if (rem <= 0.0) {
                        min_rem_sec = 0.0;
                        break;
                    }
                    if (min_rem_sec < 0.0 || rem < min_rem_sec) {
                        min_rem_sec = rem;
                    }
                }
            }

            if (min_rem_sec >= 0.0) {
                poll_timeout_ms = (int)(min_rem_sec * 1000.0);
            }
        }

        struct pollfd pfd = { .fd = sockfd, .events = POLLIN, .revents = 0 };
        int prc = poll(&pfd, 1, poll_timeout_ms);
        if (prc < 0) {
            if (errno == EINTR) continue;
            perror("[-] poll error");
            exit_code = 1;
            break;
        }

        /* Jalankan aksi --script yang sudah waktunya */
        if (got_first_data) {
            uint64_t now = now_mono_ns();
            double elapsed_sec = (double)(now - first_data_ns) / 1e9;

            for (int i = 0; i < num_actions; i++) {
                if (!actions[i].executed && elapsed_sec >= actions[i].trigger_sec) {
                    actions[i].executed = true;
                    uint8_t mtype = 0;
                    uint32_t param = 0;

                    if (actions[i].type == CMD_PAUSE) {
                        mtype = MSG_PAUSE;
                    } else if (actions[i].type == CMD_PLAY) {
                        mtype = MSG_PLAY;
                    } else if (actions[i].type == CMD_SEEK) {
                        mtype = MSG_SEEK;
                        param = actions[i].arg;
                    } else if (actions[i].type == CMD_STOP) {
                        mtype = MSG_STOP;
                    }

                    log_csv(csv_fp, "CMD", cmd_seq, now, 0, param);
                    if (send_frame(sockfd, mtype, cmd_seq++, param, NULL, 0) < 0) {
                        fprintf(stderr, "[-] Failed to send scripted command\n");
                        exit_code = 1;
                        goto cleanup_exit;
                    }

                    waiting_for_ack = true;
                    last_cmd_type = mtype;
                    break; /* Kirim satu command per siklus poll */
                }
            }
        }

        if (prc == 0) {
            continue; /* Timeout poll tercapai untuk menjalankan script */
        }

        if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
            int rc = recv_frame(sockfd, &hdr, payload, sizeof(payload));
            uint64_t recv_ns = now_mono_ns();

            if (rc == 0) {
                fprintf(stderr, "[-] Unexpected EOF from server before END/STOP\n");
                exit_code = 1;
                break;
            }
            if (rc < 0) {
                fprintf(stderr, "[-] Frame receive error (rc=%d)\n", rc);
                exit_code = 1;
                break;
            }

            if (hdr.msg_type == MSG_ACK) {
                log_csv(csv_fp, "ACK", hdr.seq_num, recv_ns, hdr.payload_len, hdr.param);
                waiting_for_ack = false;

                if (last_cmd_type == MSG_PLAY && hdr.param > 0) {
                    /* PLAY awal atau resume: expected_seq tetap (0 untuk awal, next chunk untuk resume) */
                } else if (last_cmd_type == MSG_SEEK) {
                    /* Setelah SEEK ACK, DATA berikutnya akan dimulai dari seq = hdr.param */
                    expected_seq = hdr.param;
                } else if (last_cmd_type == MSG_STOP) {
                    /* Uji koneksi tetap hidup setelah STOP dengan ECHO */
                    const char *ping = "post-stop-check";
                    if (send_frame(sockfd, MSG_ECHO, 999, 42, ping, (uint32_t)strlen(ping)) < 0) {
                        fprintf(stderr, "[-] Post-STOP ECHO send failed\n");
                        exit_code = 1;
                        break;
                    }
                    frame_hdr_t ehdr;
                    uint8_t epay[128];
                    if (recv_frame(sockfd, &ehdr, epay, sizeof(epay)) <= 0 || ehdr.msg_type != MSG_ECHO) {
                        fprintf(stderr, "[-] Post-STOP ECHO reply failed\n");
                        exit_code = 1;
                        break;
                    }
                    printf("[+] STREAM STOPPED cleanly at seq=%u (post-STOP ECHO verified)\n", hdr.param);
                    exit_code = 0;
                    break;
                }
            } else if (hdr.msg_type == MSG_ERROR) {
                log_csv(csv_fp, "ERROR", hdr.seq_num, recv_ns, hdr.payload_len, hdr.param);
                error_count++;

                /* Jika tidak ada --script, error apapun dianggap kegagalan (misal FILE_NOT_FOUND 200) */
                if (num_actions == 0) {
                    fprintf(stderr, "[-] Server returned ERROR %u: %.*s\n",
                            hdr.param, (int)hdr.payload_len, (char *)payload);
                    exit_code = 1;
                    break;
                }

                /* Jika sedang memakai --script, command ditolak (misal 202/300):
                   Reset waiting_for_ack agar DATA stream tetap berlanjut! */
                waiting_for_ack = false;

                /* Cek apakah masih ada sisa aksi script setelah error ini; jika tidak ada & stream sudah STOP, keluar */
                bool pending_actions = false;
                for (int i = 0; i < num_actions; i++) {
                    if (!actions[i].executed) pending_actions = true;
                }
                if (!pending_actions && last_cmd_type == MSG_STOP) {
                    exit_code = 0;
                    break;
                }
            } else if (hdr.msg_type == MSG_DATA) {
                /* Abaikan stale DATA yang masih di perjalanan sebelum ACK tiba */
                if (waiting_for_ack) {
                    continue;
                }

                log_csv(csv_fp, "DATA", hdr.seq_num, recv_ns, hdr.payload_len, hdr.param);

                if (!got_first_data) {
                    got_first_data = true;
                    first_data_ns = recv_ns;
                }

                if (hdr.seq_num != expected_seq) {
                    fprintf(stderr, "[-] Sequence mismatch: expected %u, got %u\n",
                            expected_seq, hdr.seq_num);
                    exit_code = 1;
                    break;
                }

                /* Posisikan file offset jika menerima chunk PCM (seq >= 1) */
                if (hdr.seq_num == 0) {
                    fseek(out_fp, 0, SEEK_SET);
                } else {
                    long target_offset = 44L + (long)(hdr.seq_num - 1) * 8820L;
                    fseek(out_fp, target_offset, SEEK_SET);
                }

                if (hdr.payload_len > 0) {
                    if (fwrite(payload, 1, hdr.payload_len, out_fp) != hdr.payload_len) {
                        perror("[-] fwrite failed");
                        exit_code = 1;
                        break;
                    }
                }

                total_payload_bytes += hdr.payload_len;
                total_data_frames++;
                expected_seq++;
            } else if (hdr.msg_type == MSG_END) {
                log_csv(csv_fp, "END", hdr.seq_num, recv_ns, hdr.payload_len, hdr.param);

                if (hdr.param != total_payload_bytes) {
                    fprintf(stderr, "[-] END byte mismatch: END.param=%u, received=%lu\n",
                            hdr.param, (unsigned long)total_payload_bytes);
                    exit_code = 1;
                    break;
                }

                double total_sec = got_first_data ? (double)(recv_ns - first_data_ns) / 1e9 : 0.0;
                printf("[+] STREAM END: %lu bytes (%u DATA frames) in %.3f s (errors=%u)\n",
                       (unsigned long)total_payload_bytes, total_data_frames, total_sec, error_count);
                exit_code = 0;
                break;
            } else {
                fprintf(stderr, "[-] Unexpected msg_type 0x%02x\n", hdr.msg_type);
                exit_code = 1;
                break;
            }
        }
    }

cleanup_exit:
    fflush(out_fp);
    fclose(out_fp);
    close(sockfd);
    if (csv_fp) {
        fflush(csv_fp);
        fclose(csv_fp);
    }
    return exit_code;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s echo <host> <port> <count>\n", argv[0]);
        fprintf(stderr, "  %s get  <host> <port> <file> <out> [--csv path]\n", argv[0]);
        fprintf(stderr, "  %s play <host> <port> <file> <out> [--script \"t:cmd,...\"] [--csv path]\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "echo") == 0) {
        if (argc != 5) {
            fprintf(stderr, "Usage: %s echo <host> <port> <count>\n", argv[0]);
            return 1;
        }
        return run_echo_mode(argv[2], argv[3], atoi(argv[4]));
    } else if (strcmp(argv[1], "get") == 0 || strcmp(argv[1], "play") == 0) {
        if (argc < 6) {
            fprintf(stderr, "Usage: %s %s <host> <port> <file> <out> [--script ...] [--csv ...]\n",
                    argv[0], argv[1]);
            return 1;
        }

        const char *host = argv[2];
        const char *port = argv[3];
        const char *remote_file = argv[4];
        const char *out_path = argv[5];
        const char *script_str = NULL;
        const char *csv_path = NULL;

        for (int i = 6; i < argc; i++) {
            if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
                script_str = argv[++i];
            } else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
                csv_path = argv[++i];
            } else {
                fprintf(stderr, "[-] Unknown or incomplete option: %s\n", argv[i]);
                return 1;
            }
        }

        return run_play_mode(host, port, remote_file, out_path, script_str, csv_path);
    } else {
        fprintf(stderr, "[-] Unknown mode: %s\n", argv[1]);
        return 1;
    }
}
