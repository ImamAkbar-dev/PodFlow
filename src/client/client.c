#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "io_utils.h"
#include "protocol.h"
#include "io_utils.h"

#ifndef ERR_SERVER_BUSY
#define ERR_SERVER_BUSY 503
#endif

#define MAX_PAYLOAD_BUF 65536
#define MAX_SCRIPT_CMDS 64
#define WAV_HDR_BYTES   44
#define CHUNK_PCM_BYTES 8820

typedef enum {
    ACT_PAUSE = 1,
    ACT_PLAY,
    ACT_SEEK,
    ACT_STOP
} script_act_t;

typedef struct {
    double trigger_sec;     /* Seconds after first DATA frame */
    script_act_t action;
    uint32_t param;         /* Target chunk for SEEK */
    bool executed;
} script_cmd_t;

/* Get CLOCK_MONOTONIC timestamp in nanoseconds */
static uint64_t now_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Deterministic LCG for echo payload generation */
static uint8_t generate_byte(uint32_t *state) {
    *state = (*state * 1103515245U + 12345U) & 0x7fffffffU;
    return (uint8_t)(*state & 0xFFU);
}

/* Resolve host/port and open TCP socket */
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

/* Perform HELLO -> WELCOME handshake (accepts any WELCOME payload) */
static int perform_handshake(int sockfd) {
    const char *client_name = "podflow-m3-client";
    if (send_frame(sockfd, MSG_HELLO, 0, 0, client_name, (uint32_t)strlen(client_name)) < 0) {
        fprintf(stderr, "[-] Failed to send HELLO\n");
        return -1;
    }

    frame_hdr_t hdr;
    uint8_t payload[MAX_PAYLOAD_BUF];
    int res = recv_frame(sockfd, &hdr, payload, sizeof(payload));
    if (res <= 0 || hdr.msg_type != MSG_WELCOME) {
        fprintf(stderr, "[-] Handshake failed: expected WELCOME, got type=%u (res=%d)\n",
                hdr.msg_type, res);
        return -1;
    }
    return 0;
}

/* Log an event to CSV if enabled: kind,seq,t_mono_ns,len,param */
static void log_csv(FILE *csv_fp, const char *kind, uint32_t seq,
                    uint64_t t_ns, uint32_t len, uint32_t param) {
    if (!csv_fp) return;
    fprintf(csv_fp, "%s,%u,%llu,%u,%u\n",
            kind, seq, (unsigned long long)t_ns, len, param);
}

/* Parse --script "t:cmd,t:cmd,..." */
static int parse_script(const char *script_str, script_cmd_t *cmds, int *num_cmds) {
    *num_cmds = 0;
    if (!script_str || strlen(script_str) == 0) return 0;

    char *copy = strdup(script_str);
    if (!copy) return -1;

    char *saveptr = NULL;
    char *token = strtok_r(copy, ",", &saveptr);
    while (token != NULL) {
        if (*num_cmds >= MAX_SCRIPT_CMDS) {
            fprintf(stderr, "[-] Too many script commands (max %d)\n", MAX_SCRIPT_CMDS);
            free(copy);
            return -1;
        }

        char *colon = strchr(token, ':');
        if (!colon) {
            fprintf(stderr, "[-] Invalid script token (missing ':'): %s\n", token);
            free(copy);
            return -1;
        }

        *colon = '\0';
        char *time_str = token;
        char *cmd_str = colon + 1;

        char *endptr = NULL;
        double t_val = strtod(time_str, &endptr);
        if (endptr == time_str || t_val < 0.0) {
            fprintf(stderr, "[-] Invalid timestamp in script: %s\n", time_str);
            free(copy);
            return -1;
        }

        script_cmd_t *c = &cmds[*num_cmds];
        c->trigger_sec = t_val;
        c->param = 0;
        c->executed = false;

        if (strcmp(cmd_str, "pause") == 0) {
            c->action = ACT_PAUSE;
        } else if (strcmp(cmd_str, "play") == 0) {
            c->action = ACT_PLAY;
        } else if (strcmp(cmd_str, "stop") == 0) {
            c->action = ACT_STOP;
        } else if (strncmp(cmd_str, "seek=", 5) == 0) {
            c->action = ACT_SEEK;
            c->param = (uint32_t)strtoul(cmd_str + 5, NULL, 10);
        } else {
            fprintf(stderr, "[-] Unknown script command: %s\n", cmd_str);
            free(copy);
            return -1;
        }

        (*num_cmds)++;
        token = strtok_r(NULL, ",", &saveptr);
    }

    free(copy);
    return 0;
}

/* ============================================================================
 * MODE 1: ECHO TEST
 * ============================================================================ */
static int run_echo_mode(const char *host, const char *port, int count) {
    if (count <= 0) {
        fprintf(stderr, "[-] Echo count must be > 0\n");
        return 1;
    }

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
        uint32_t p_size = payload_sizes[(size_t)i % num_sizes];
        uint32_t lcg_state = (uint32_t)i + 1U;

        for (uint32_t b = 0; b < p_size; b++) {
            send_buf[b] = generate_byte(&lcg_state);
        }

        uint32_t seq = (uint32_t)i;
        uint32_t param = (uint32_t)(i * 7);

        if (send_frame(sockfd, MSG_ECHO, seq, param, send_buf, p_size) < 0) {
            fprintf(stderr, "[-] Failed to send ECHO frame %d\n", i);
            close(sockfd);
            return 1;
        }

        frame_hdr_t hdr;
        int ret = recv_frame(sockfd, &hdr, recv_buf, sizeof(recv_buf));
        if (ret <= 0) {
            fprintf(stderr, "[-] Failed to receive ECHO reply %d (ret=%d)\n", i, ret);
            close(sockfd);
            return 1;
        }

        if (hdr.msg_type != MSG_ECHO || hdr.seq_num != seq ||
            hdr.param != param || hdr.payload_len != p_size ||
            (p_size > 0 && memcmp(send_buf, recv_buf, p_size) != 0)) {
            fprintf(stderr, "[-] ECHO mismatch at frame %d\n", i);
            close(sockfd);
            return 1;
        }
    }

    printf("ECHO OK: %d/%d\n", count, count);
    close(sockfd);
    return 0;
}

/* ============================================================================
 * MODE 2: PLAY / GET STREAMING MODE
 * ============================================================================ */
static int run_play_mode(const char *host, const char *port,
                         const char *remote_file, const char *out_file,
                         const char *script_str, const char *csv_path) {
    script_cmd_t cmds[MAX_SCRIPT_CMDS];
    int num_cmds = 0;
    if (parse_script(script_str, cmds, &num_cmds) < 0) {
        return 1;
    }

    FILE *csv_fp = NULL;
    if (csv_path) {
        csv_fp = fopen(csv_path, "w");
        if (!csv_fp) {
            perror("[-] Failed to open CSV output file");
            return 1;
        }
        fprintf(csv_fp, "kind,seq,t_mono_ns,len,param\n");
    }

    int sockfd = connect_to_server(host, port);
    if (sockfd < 0) {
        if (csv_fp) fclose(csv_fp);
        return 1;
    }

    if (perform_handshake(sockfd) < 0) {
        close(sockfd);
        if (csv_fp) fclose(csv_fp);
        return 1;
    }

    FILE *out_fp = fopen(out_file, "wb");
    if (!out_fp) {
        perror("[-] Failed to open audio output file");
        close(sockfd);
        if (csv_fp) fclose(csv_fp);
        return 1;
    }

    /* Send initial PLAY request (param=0 -> start from beginning) */
    uint32_t cmd_seq = 1;
    uint64_t t_send = now_mono_ns();
    if (send_frame(sockfd, MSG_PLAY, cmd_seq, 0, remote_file, (uint32_t)strlen(remote_file)) < 0) {
        fprintf(stderr, "[-] Failed to send initial PLAY command\n");
        fclose(out_fp);
        close(sockfd);
        if (csv_fp) fclose(csv_fp);
        return 1;
    }
    log_csv(csv_fp, "CMD", cmd_seq, t_send, (uint32_t)strlen(remote_file), MSG_PLAY);

    /* State tracking */
    bool waiting_ack = true;
    uint8_t pending_msg_type = MSG_PLAY;
    bool got_first_data = false;
    uint64_t t_first_data_ns = 0;

    uint32_t expected_seq = 0;
    bool seek_occurred = false;
    uint64_t total_valid_bytes = 0;
    uint32_t total_data_frames = 0;
    uint32_t error_count = 0;
    bool stopped_cleanly = false;

    uint8_t payload[MAX_PAYLOAD_BUF];
    frame_hdr_t hdr;

    while (1) {
        /* Determine poll timeout until the next unexecuted script command */
        int poll_timeout_ms = -1; /* Infinite wait by default */
        int next_cmd_idx = -1;

        if (got_first_data) {
            uint64_t now_ns = now_mono_ns();
            double elapsed_sec = (double)(now_ns - t_first_data_ns) / 1e9;

            for (int i = 0; i < num_cmds; i++) {
                if (!cmds[i].executed) {
                    next_cmd_idx = i;
                    double rem_sec = cmds[i].trigger_sec - elapsed_sec;
                    if (rem_sec <= 0.0) {
                        poll_timeout_ms = 0;
                    } else {
                        poll_timeout_ms = (int)(rem_sec * 1000.0);
                    }
                    break;
                }
            }
        }

        struct pollfd pfd;
        pfd.fd = sockfd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pret = poll(&pfd, 1, poll_timeout_ms);
        if (pret < 0) {
            if (errno == EINTR) continue;
            perror("[-] poll() failed");
            goto fail;
        }

        /* Trigger scripted command if timer expired */
        if (pret == 0 && next_cmd_idx != -1 && !cmds[next_cmd_idx].executed) {
            script_cmd_t *sc = &cmds[next_cmd_idx];
            sc->executed = true;
            cmd_seq++;
            uint64_t t_cmd = now_mono_ns();

            uint8_t mtype = 0;
            uint32_t mparam = 0;
            if (sc->action == ACT_PAUSE) {
                mtype = MSG_PAUSE;
            } else if (sc->action == ACT_PLAY) {
                mtype = MSG_PLAY; /* empty payload = resume */
            } else if (sc->action == ACT_SEEK) {
                mtype = MSG_SEEK;
                mparam = sc->param;
            } else if (sc->action == ACT_STOP) {
                mtype = MSG_STOP;
            }

            if (send_frame(sockfd, mtype, cmd_seq, mparam, NULL, 0) < 0) {
                fprintf(stderr, "[-] Failed to send scripted command type=%u\n", mtype);
                goto fail;
            }
            log_csv(csv_fp, "CMD", cmd_seq, t_cmd, 0, mtype);
            waiting_ack = true;
            pending_msg_type = mtype;
            continue;
        }

        /* Handle incoming frame from server */
        if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
            int rret = recv_frame(sockfd, &hdr, payload, sizeof(payload));
            uint64_t t_recv = now_mono_ns();

            if (rret == 0) {
                /* Clean EOF */
                if (stopped_cleanly) break;
                fprintf(stderr, "[-] Unexpected clean EOF from server before END\n");
                goto fail;
            } else if (rret < 0) {
                fprintf(stderr, "[-] recv_frame error: %d\n", rret);
                goto fail;
            }

            if (hdr.msg_type == MSG_ACK) {
                log_csv(csv_fp, "ACK", hdr.seq_num, t_recv, hdr.payload_len, hdr.param);
                waiting_ack = false;

                if (pending_msg_type == MSG_PLAY && hdr.param > 1 && expected_seq == 0) {
                    /* Initial play with start_chunk > 1 */
                    expected_seq = hdr.param;
                } else if (pending_msg_type == MSG_SEEK) {
                    /* Server flushed ring buffer; next DATA will be target chunk */
                    expected_seq = hdr.param;
                    seek_occurred = true;
                } else if (pending_msg_type == MSG_STOP) {
                    /* Server acknowledged STOP; no END frame will follow */
                    stopped_cleanly = true;
                    break;
                }
            } else if (hdr.msg_type == MSG_ERROR) {
                log_csv(csv_fp, "ERROR", hdr.seq_num, t_recv, hdr.payload_len, hdr.param);
                fprintf(stderr, "[-] Server returned ERROR %u: %.*s\n",
                        hdr.param, (int)hdr.payload_len, (char *)payload);
                waiting_ack = false;
                error_count++;

                /* Fatal errors on initial PLAY or protocol violations */
                if (hdr.param == ERR_FILE_NOT_FOUND || hdr.param == ERR_INVALID_WAV ||
                    hdr.param == ERR_BAD_MAGIC || hdr.param == ERR_BAD_VERSION ||
                    hdr.param == ERR_PAYLOAD_TOO_LARGE || hdr.param == ERR_SERVER_BUSY) {
                    goto fail;
                }
                /* Non-fatal script error (e.g., 202 SEEK_OUT_OF_RANGE, 300 INVALID_STATE):
                 * if no script was provided, treat any error as failure */
                if (num_cmds == 0) {
                    goto fail;
                }
            } else if (hdr.msg_type == MSG_DATA) {
                /* Stale-DATA rule: ignore DATA arriving while waiting for command ACK */
                if (waiting_ack && (pending_msg_type == MSG_SEEK ||
                                    pending_msg_type == MSG_STOP ||
                                    pending_msg_type == MSG_PAUSE)) {
                    continue;
                }

                if (!got_first_data) {
                    got_first_data = true;
                    t_first_data_ns = t_recv;
                }

                log_csv(csv_fp, "DATA", hdr.seq_num, t_recv, hdr.payload_len, hdr.param);

                if (hdr.seq_num != expected_seq) {
                    fprintf(stderr, "[-] Sequence mismatch! Expected %u, got %u\n",
                            expected_seq, hdr.seq_num);
                    goto fail;
                }

                if (seek_occurred && hdr.seq_num > 0) {
                    long target_offset = (long)WAV_HDR_BYTES + (long)(hdr.seq_num - 1U) * CHUNK_PCM_BYTES;
                    if (fseek(out_fp, target_offset, SEEK_SET) != 0) {
                        perror("[-] fseek failed on output file");
                        goto fail;
                    }
                }

                if (hdr.payload_len > 0) {
                    if (fwrite(payload, 1, hdr.payload_len, out_fp) != hdr.payload_len) {
                        perror("[-] fwrite failed on output file");
                        goto fail;
                    }
                }

                total_valid_bytes += hdr.payload_len;
                total_data_frames++;
                expected_seq++;
            } else if (hdr.msg_type == MSG_END) {
                log_csv(csv_fp, "END", hdr.seq_num, t_recv, hdr.payload_len, hdr.param);

                /* Verify total bytes only when stream ran straight without seek jumps */
                if (!seek_occurred && (uint64_t)hdr.param != total_valid_bytes) {
                    fprintf(stderr, "[-] END byte mismatch! END.param=%u, client received=%llu\n",
                            hdr.param, (unsigned long long)total_valid_bytes);
                    goto fail;
                }

                double total_sec = got_first_data ? (double)(t_recv - t_first_data_ns) / 1e9 : 0.0;
                printf("[+] STREAM END: %llu bytes (%u DATA frames) in %.3f s (errors=%u)\n",
                       (unsigned long long)total_valid_bytes, total_data_frames,
                       total_sec, error_count);
                break;
            }
        }
    }

    /* Post-stop verification: ensure connection is still alive using ECHO */
    if (stopped_cleanly) {
        const char *ping = "post-stop-ping";
        if (send_frame(sockfd, MSG_ECHO, 999, 0, ping, (uint32_t)strlen(ping)) < 0 ||
            recv_frame(sockfd, &hdr, payload, sizeof(payload)) <= 0 ||
            hdr.msg_type != MSG_ECHO) {
            fprintf(stderr, "[-] Post-STOP connection check failed\n");
            goto fail;
        }
        printf("[+] STREAM STOPPED cleanly at seq=%u; connection verified alive.\n", expected_seq);
    }

    fflush(out_fp);
    fclose(out_fp);
    close(sockfd);
    if (csv_fp) fclose(csv_fp);
    return 0;

fail:
    fclose(out_fp);
    close(sockfd);
    if (csv_fp) fclose(csv_fp);
    return 1;
}

/* ============================================================================
 * CLI ENTRY POINT
 * ============================================================================ */
static void print_usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s echo <host> <port> <count>\n", prog);
    fprintf(stderr, "  %s get  <host> <port> <file> <out> [--csv path]\n", prog);
    fprintf(stderr, "  %s play <host> <port> <file> <out> [--script \"t:cmd,...\"] [--csv path]\n", prog);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    const char *mode = argv[1];

    if (strcmp(mode, "echo") == 0) {
        if (argc != 5) {
            print_usage(argv[0]);
            return 1;
        }
        return run_echo_mode(argv[2], argv[3], atoi(argv[4]));
    } else if (strcmp(mode, "get") == 0 || strcmp(mode, "play") == 0) {
        if (argc < 6) {
            print_usage(argv[0]);
            return 1;
        }
        const char *host = argv[2];
        const char *port = argv[3];
        const char *remote_file = argv[4];
        const char *out_file = argv[5];
        const char *script_str = NULL;
        const char *csv_path = NULL;

        for (int i = 6; i < argc; i++) {
            if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
                script_str = argv[++i];
            } else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
                csv_path = argv[++i];
            } else {
                fprintf(stderr, "[-] Unknown or incomplete flag: %s\n", argv[i]);
                print_usage(argv[0]);
                return 1;
            }
        }

        return run_play_mode(host, port, remote_file, out_file, script_str, csv_path);
    } else {
        fprintf(stderr, "[-] Unknown mode: %s\n", mode);
        print_usage(argv[0]);
        return 1;
    }
}