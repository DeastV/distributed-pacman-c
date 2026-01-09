#include "board.h"
#include "display.h"
#include "protocol.h" 
#include <stdlib.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <errno.h>
#include <semaphore.h>
#include <signal.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2

#define BUFFER_SIZE 10

typedef struct {
    char req_pipe_path[40];
    char notif_pipe_path[40];
} connection_request_t;

connection_request_t request_buffer[BUFFER_SIZE];
int buf_in = 0;
int buf_out = 0;

sem_t sem_empty;
sem_t sem_full;
pthread_mutex_t buf_mutex;

char global_levels_dir[256];

typedef struct {
    game_state_t *state;
    int req_fd;
    int notif_fd;
} render_context_t;

char* capture_snapshot(board_t *board, int outcome, int *out_size) {
    char op = OP_CODE_BOARD;
    int victory = (outcome == NEXT_LEVEL);
    int game_over = (outcome == QUIT_GAME);
    int points = (board->n_pacmans > 0) ? board->pacmans[0].points : 0;
    
    int header_size = 1 + (6 * sizeof(int));
    int grid_size = board->width * board->height;
    int total_size = header_size + grid_size;

    char *buffer = malloc(total_size);
    if (!buffer) return NULL;

    int offset = 0;
    
    memcpy(buffer + offset, &op, 1); offset += 1;
    memcpy(buffer + offset, &board->width, sizeof(int)); offset += sizeof(int);
    memcpy(buffer + offset, &board->height, sizeof(int)); offset += sizeof(int);
    memcpy(buffer + offset, &board->tempo, sizeof(int)); offset += sizeof(int);
    memcpy(buffer + offset, &victory, sizeof(int)); offset += sizeof(int);
    memcpy(buffer + offset, &game_over, sizeof(int)); offset += sizeof(int);
    memcpy(buffer + offset, &points, sizeof(int)); offset += sizeof(int);

    for (int i = 0; i < grid_size; i++) {
        int x = i % board->width;
        int y = i / board->width;
        char c = board->board[i].content;

        if (c == 'M') {
            for (int g = 0; g < board->n_ghosts; g++) {
                if (board->ghosts[g].pos_x == x && board->ghosts[g].pos_y == y) {
                    if (board->ghosts[g].charged) c = 'G';
                    break;
                }
            }
        }
        else if (c == ' ') {
            if (board->board[i].has_portal) c = '@';
            else if (board->board[i].has_dot) c = '.';
        }
        buffer[offset++] = c;
    }

    *out_size = total_size;
    return buffer;
}

static void set_outcome(game_state_t *state, int outcome) {
    if (state->outcome == CONTINUE_PLAY) {
        state->outcome = outcome;
    }
    state->running = 0;
    pthread_cond_broadcast(&state->input_cond);
}

static void *render_thread(void *arg) {
    render_context_t *ctx = (render_context_t *)arg;
    game_state_t *state = ctx->state;
    int req_fd = ctx->req_fd;
    int notif_fd = ctx->notif_fd;

    if (req_fd != -1) {
        int flags = fcntl(req_fd, F_GETFL, 0);
        fcntl(req_fd, F_SETFL, flags | O_NONBLOCK);
    }

    while (1) {
        pthread_mutex_lock(&state->mutex);
        
        int running = state->running;
        int outcome = state->outcome;
        
        int snap_size = 0;
        char *snapshot = capture_snapshot(state->board, outcome, &snap_size);
        int sleep_time = state->board->tempo;

        pthread_mutex_unlock(&state->mutex);

        if (snapshot && notif_fd != -1) {
            write(notif_fd, snapshot, snap_size);
            free(snapshot);
        }

        if (!running) break;

        if (req_fd != -1) {
            char op_code;
            char command;
            int n = read(req_fd, &op_code, 1);
            
            if (n > 0) {
                if (op_code == OP_CODE_PLAY) {
                    if (read(req_fd, &command, 1) > 0) {
                        pthread_mutex_lock(&state->mutex);
                        state->pending_input = command;
                        pthread_cond_broadcast(&state->input_cond);
                        pthread_mutex_unlock(&state->mutex);
                    }
                } else if (op_code == OP_CODE_DISCONNECT) {
                    pthread_mutex_lock(&state->mutex);
                    set_outcome(state, QUIT_GAME);
                    pthread_mutex_unlock(&state->mutex);
                }
            }
        }

        if (sleep_time != 0) {
            sleep_ms(sleep_time);
        }
    }
    return NULL;
}

static command_t build_manual_command(char input) {
    command_t cmd;
    cmd.command = input;
    cmd.turns = 1;
    cmd.turns_left = 1;
    return cmd;
}

static void *pacman_thread(void *arg) {
    game_state_t *state = (game_state_t *)arg;
    board_t *board = state->board;
    command_t manual_cmd;

    while (1) {
        pthread_mutex_lock(&state->mutex);
        if (!state->running) {
            pthread_mutex_unlock(&state->mutex);
            break;
        }

        pacman_t *pacman = &board->pacmans[0];
        command_t *cmd_ptr;

        if (pacman->n_moves == 0) {
            while (state->pending_input == '\0' && state->running) {
                pthread_cond_wait(&state->input_cond, &state->mutex);
            }
            if (!state->running) {
                pthread_mutex_unlock(&state->mutex);
                break;
            }
            manual_cmd = build_manual_command(state->pending_input);
            state->pending_input = '\0';
            cmd_ptr = &manual_cmd;
        } else {
            int cmd_index = pacman->current_move % pacman->n_moves;
            cmd_ptr = &pacman->moves[cmd_index];
        }
        pthread_mutex_unlock(&state->mutex);

        if (cmd_ptr->command == 'Q') {
            pthread_mutex_lock(&state->mutex);
            set_outcome(state, QUIT_GAME);
            pthread_mutex_unlock(&state->mutex);
            continue;
        }

        pthread_mutex_lock(&state->mutex);
        int is_running = state->running;
        pthread_mutex_unlock(&state->mutex);
        if (!is_running) break;
        
        int result = move_pacman(board, 0, cmd_ptr);
        
        if (result == REACHED_PORTAL || result == DEAD_PACMAN) {
            pthread_mutex_lock(&state->mutex);
            if (result == REACHED_PORTAL) set_outcome(state, NEXT_LEVEL);
            else if (result == DEAD_PACMAN) set_outcome(state, QUIT_GAME);
            pthread_mutex_unlock(&state->mutex);
        }

        if (board->tempo != 0) sleep_ms(board->tempo);
    }
    return NULL;
}

static void *ghost_thread(void *arg) {
    ghost_thread_args_t *ghost_args = (ghost_thread_args_t *)arg;
    game_state_t *state = ghost_args->state;
    int ghost_index = ghost_args->ghost_index;
    board_t *board = state->board;

    while (1) {
        pthread_mutex_lock(&state->mutex);
        if (!state->running) {
            pthread_mutex_unlock(&state->mutex);
            break;
        }

        ghost_t *ghost = &board->ghosts[ghost_index];
        if (ghost->n_moves == 0) {
            pthread_mutex_unlock(&state->mutex);
            if (board->tempo != 0) sleep_ms(board->tempo);
            continue;
        }

        int cmd_index = ghost->current_move % ghost->n_moves;
        command_t *cmd_ptr = &ghost->moves[cmd_index];
        pthread_mutex_unlock(&state->mutex);

        pthread_mutex_lock(&state->mutex);
        int is_running = state->running;
        pthread_mutex_unlock(&state->mutex);
        if (!is_running) break;

        int result = move_ghost(board, ghost_index, cmd_ptr);
        
        if (result == DEAD_PACMAN) {
            pthread_mutex_lock(&state->mutex);
            set_outcome(state, QUIT_GAME);
            pthread_mutex_unlock(&state->mutex);
        }

        if (board->tempo != 0) sleep_ms(board->tempo);
    }
    return NULL;
}

int has_extension(const char *filename, const char *ext) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) return 0;
    return (strcmp(dot, ext) == 0);
}

int find_levels(const char *dirpath, char lista[MAX_LEVELS][MAX_FILENAME]) {
    DIR *dirp = opendir(dirpath);
    if (dirp == NULL) {
        if (strcmp(dirpath, ".") != 0) return find_levels(".", lista);
        perror("Error opening directory");
        return 0;
    }
    struct dirent *dp;
    int count = 0;
    while ((dp = readdir(dirp)) != NULL) {
        if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0) continue;
        if (has_extension(dp->d_name, ".lvl") && count < MAX_LEVELS) {
            strncpy(lista[count], dp->d_name, MAX_FILENAME - 1);
            lista[count][MAX_FILENAME - 1] = '\0';
            count++;
        }
    }
    closedir(dirp);
    return count;
}

int run_game(board_t *board, int req_fd, int notif_fd) {
    game_state_t state = {
        .board = board, .running = 1, .outcome = CONTINUE_PLAY,
        .pending_input = '\0', .save_request = 0
    };
    pthread_mutex_init(&state.mutex, NULL);
    pthread_cond_init(&state.input_cond, NULL);

    pthread_t render_tid, pacman_tid;
    pthread_t ghost_tids[MAX_GHOSTS];
    ghost_thread_args_t ghost_args[MAX_GHOSTS];

    render_context_t render_ctx = { .state = &state, .req_fd = req_fd, .notif_fd = notif_fd };

    pthread_create(&render_tid, NULL, render_thread, &render_ctx);
    pthread_create(&pacman_tid, NULL, pacman_thread, &state);

    for (int g = 0; g < board->n_ghosts; g++) {
        ghost_args[g].state = &state;
        ghost_args[g].ghost_index = g;
        pthread_create(&ghost_tids[g], NULL, ghost_thread, &ghost_args[g]);
    }

    pthread_join(pacman_tid, NULL);
    for (int g = 0; g < board->n_ghosts; g++) pthread_join(ghost_tids[g], NULL);
    pthread_join(render_tid, NULL);

    int outcome = state.outcome;
    pthread_mutex_destroy(&state.mutex);
    pthread_cond_destroy(&state.input_cond);
    return outcome;
}

void cleanup_server(const char* fifo_name) {
    unlink(fifo_name);
}

void *worker_thread(void *arg) {
    (void)arg;
    while (1) {
        sem_wait(&sem_full);
        pthread_mutex_lock(&buf_mutex);
        connection_request_t req = request_buffer[buf_out];
        buf_out = (buf_out + 1) % BUFFER_SIZE;
        pthread_mutex_unlock(&buf_mutex);
        sem_post(&sem_empty);

        int client_notif_fd = open(req.notif_pipe_path, O_WRONLY);
        int client_req_fd = open(req.req_pipe_path, O_RDONLY);

        if (client_notif_fd == -1 || client_req_fd == -1) {
            if (client_notif_fd != -1) close(client_notif_fd);
            if (client_req_fd != -1) close(client_req_fd);
            continue;
        }

        char res_op = OP_CODE_CONNECT, res_val = 0;
        write(client_notif_fd, &res_op, 1);
        write(client_notif_fd, &res_val, 1);

        char lista_niveis[MAX_LEVELS][MAX_FILENAME];
        int n_niveis = find_levels(".", lista_niveis);
        int points = 0;

        for (int i = 0; i < n_niveis; i++) {
            if (fcntl(client_req_fd, F_GETFD) == -1) break;
            board_t game_board = {0};
            if (load_level_filename(&game_board, lista_niveis[i], points) != 0) continue;
            strncpy(game_board.level_name, lista_niveis[i], 255);
            
            int outcome = run_game(&game_board, client_req_fd, client_notif_fd);
            points = (game_board.n_pacmans > 0) ? game_board.pacmans[0].points : points;
            unload_level(&game_board);

            if (outcome == QUIT_GAME) break;
            sleep_ms(1000);
        }
        close(client_notif_fd);
        close(client_req_fd);
    }
    return NULL;
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);

    if (argc != 4) {    
        fprintf(stderr, "Uso: %s <dir_niveis> <max_jogos> <fifo_registo>\n", argv[0]);
        return 1;
    }

    strncpy(global_levels_dir, argv[1], 255);
    int max_games = atoi(argv[2]);
    char *server_fifo_name = argv[3];

    if (chdir(global_levels_dir) != 0) {
        perror("Erro ao mudar de diretoria");
        return 1;
    }

    sem_init(&sem_empty, 0, BUFFER_SIZE);
    sem_init(&sem_full, 0, 0);
    pthread_mutex_init(&buf_mutex, NULL);

    pthread_t *workers = malloc(sizeof(pthread_t) * max_games);
    for (int i = 0; i < max_games; i++) {
        pthread_create(&workers[i], NULL, worker_thread, NULL);
    }

    if (mkfifo(server_fifo_name, 0666) == -1 && errno != EEXIST) {
        perror("Erro mkfifo server");
        return 1;
    }

    printf("[SERVIDOR] A aguardar clientes em %s (Max: %d)...\n", server_fifo_name, max_games);
    srand((unsigned int)time(NULL));
    open_debug_file("server_debug.log");

    while (1) {
        int server_fd = open(server_fifo_name, O_RDONLY);
        if (server_fd == -1) continue;

        char op, req_pipe[40], notif_pipe[40];
        if (read(server_fd, &op, 1) > 0) {
            read(server_fd, req_pipe, 40);
            read(server_fd, notif_pipe, 40);
            
            if (op == OP_CODE_CONNECT) {
                sem_wait(&sem_empty);
                pthread_mutex_lock(&buf_mutex);
                strncpy(request_buffer[buf_in].req_pipe_path, req_pipe, 40);
                strncpy(request_buffer[buf_in].notif_pipe_path, notif_pipe, 40);
                buf_in = (buf_in + 1) % BUFFER_SIZE;
                pthread_mutex_unlock(&buf_mutex);
                sem_post(&sem_full);
            }
        }
        close(server_fd);
    }

    close_debug_file();
    cleanup_server(server_fifo_name);
    free(workers);
    return 0;
}