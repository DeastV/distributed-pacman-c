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

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2

// --- VARIÁVEIS GLOBAIS DO SERVIDOR ---
int client_req_fd = -1;
int client_notif_fd = -1;
// -------------------------------------

// Função auxiliar para enviar o tabuleiro ao cliente via pipe
// Função auxiliar para enviar o tabuleiro ao cliente via pipe
void send_board_to_client(board_t *board, int outcome) {
    if (client_notif_fd == -1) return;

    char op = OP_CODE_BOARD;
    int victory = (outcome == NEXT_LEVEL);
    int game_over = (outcome == QUIT_GAME);
    
    // Serializar a matriz
    char *serialized_data = malloc(board->width * board->height);
    if (!serialized_data) return;

    for (int i = 0; i < board->width * board->height; i++) {
        // Calcular coordenadas X, Y atuais
        int x = i % board->width;
        int y = i / board->width;
        
        char c = board->board[i].content;

        // Lógica de tradução para o Cliente
        if (c == 'M') {
            // Verificar se este fantasma específico está 'charged'
            for (int g = 0; g < board->n_ghosts; g++) {
                if (board->ghosts[g].pos_x == x && board->ghosts[g].pos_y == y) {
                    if (board->ghosts[g].charged) {
                        c = 'G'; // Envia 'G' para o cliente saber que deve mudar a cor
                    }
                    break;
                }
            }
        }
        else if (c == ' ') {
            if (board->board[i].has_portal) c = '@'; 
            else if (board->board[i].has_dot) c = '.'; 
        }
        serialized_data[i] = c;
    }

    // Protocolo de envio
    write(client_notif_fd, &op, 1);
    write(client_notif_fd, &board->width, sizeof(int));
    write(client_notif_fd, &board->height, sizeof(int));
    write(client_notif_fd, &board->tempo, sizeof(int));
    write(client_notif_fd, &victory, sizeof(int));
    write(client_notif_fd, &game_over, sizeof(int));
    
    int points = (board->n_pacmans > 0) ? board->pacmans[0].points : 0;
    write(client_notif_fd, &points, sizeof(int));
    write(client_notif_fd, serialized_data, board->width * board->height);

    free(serialized_data);
}

// Safely updates the game outcome
static void set_outcome(game_state_t *state, int outcome) {
    if (state->outcome == CONTINUE_PLAY) {
        state->outcome = outcome;
    }
    state->running = 0;
    pthread_cond_broadcast(&state->input_cond);
}

// Render Thread MODIFICADA: Envia para o cliente e lê do pipe
static void *render_thread(void *arg) {
    game_state_t *state = (game_state_t *)arg;

    // Configura pipe de leitura como não-bloqueante para não parar o jogo
    if (client_req_fd != -1) {
        int flags = fcntl(client_req_fd, F_GETFL, 0);
        fcntl(client_req_fd, F_SETFL, flags | O_NONBLOCK);
    }

    while (1) {
        pthread_mutex_lock(&state->mutex);
        int running = state->running;
        int outcome = state->outcome;
        board_t *board = state->board;

        // 1. Enviar estado ao cliente (Substitui draw_board)
        send_board_to_client(board, outcome);
        
        pthread_mutex_unlock(&state->mutex);

        if (!running) break;

        // 2. Ler input do Cliente via Pipe
        if (client_req_fd != -1) {
            char op_code;
            char command;
            int n = read(client_req_fd, &op_code, 1);
            
            if (n > 0) {
                if (op_code == OP_CODE_PLAY) {
                    if (read(client_req_fd, &command, 1) > 0) {
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

        if (board->tempo != 0) {
            sleep_ms(board->tempo);
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

// Pacman Thread (Inalterada na lógica, exceto remoção do Save 'G')
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
            // Wait for input from Render Thread (via pipe)
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

        // 'G' (Save) removido na parte 2 conforme enunciado

        pthread_mutex_lock(&state->mutex);
        int is_running = state->running;
        pthread_mutex_unlock(&state->mutex);
        if (!is_running) break;
        
        int result = move_pacman(board, 0, cmd_ptr); 
        
        if (result == REACHED_PORTAL || result == DEAD_PACMAN) {
            pthread_mutex_lock(&state->mutex);
            if (result == REACHED_PORTAL) {
                set_outcome(state, NEXT_LEVEL);
            } else if (result == DEAD_PACMAN) {
                set_outcome(state, QUIT_GAME);
            }
            pthread_mutex_unlock(&state->mutex);
        }

        if (board->tempo != 0) {
            sleep_ms(board->tempo);
        }
    }
    return NULL;
}

// Ghost Thread (Inalterada)
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

        if (board->tempo != 0) {
            sleep_ms(board->tempo);
        }
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

// NOVA FUNÇÃO: Encapsula a lógica de threads (extraída do main antigo)
int run_game(board_t *board) {
    game_state_t state = {
        .board = board,
        .running = 1,
        .outcome = CONTINUE_PLAY,
        .pending_input = '\0',
        .save_request = 0
    };

    pthread_mutex_init(&state.mutex, NULL);
    pthread_cond_init(&state.input_cond, NULL);

    pthread_t render_tid;
    pthread_t pacman_tid;
    pthread_t ghost_tids[MAX_GHOSTS];
    ghost_thread_args_t ghost_args[MAX_GHOSTS];

    // Criar Threads
    pthread_create(&render_tid, NULL, render_thread, &state);
    pthread_create(&pacman_tid, NULL, pacman_thread, &state);

    for (int g = 0; g < board->n_ghosts; g++) {
        ghost_args[g].state = &state;
        ghost_args[g].ghost_index = g;
        pthread_create(&ghost_tids[g], NULL, ghost_thread, &ghost_args[g]);
    }

    // Esperar Threads
    pthread_join(pacman_tid, NULL);
    for (int g = 0; g < board->n_ghosts; g++) {
        pthread_join(ghost_tids[g], NULL);
    }
    pthread_join(render_tid, NULL);

    int outcome = state.outcome;

    pthread_mutex_destroy(&state.mutex);
    pthread_cond_destroy(&state.input_cond);

    return outcome;
}

void cleanup_server(const char* fifo_name) {
    if (client_req_fd != -1) close(client_req_fd);
    if (client_notif_fd != -1) close(client_notif_fd);
    unlink(fifo_name);
}

// Main SERVER Loop
int main(int argc, char** argv) {
    if (argc != 4) {    
        fprintf(stderr, "Uso: %s <dir_niveis> <max_jogos> <fifo_registo>\n", argv[0]);
        return 1;
    }

    char *levels_dir = argv[1];
    // max_games ignorado na etapa 1
    char *server_fifo_name = argv[3];

    if (chdir(levels_dir) != 0) {
        perror("Erro ao mudar de diretoria");
        return 1;
    }

    // 1. Criar FIFO Registo
    if (mkfifo(server_fifo_name, 0666) == -1 && errno != EEXIST) {
        perror("Erro mkfifo server");
        return 1;
    }

    printf("[SERVIDOR] A aguardar cliente em %s...\n", server_fifo_name);
    
    // 2. Conectar Cliente
    int server_fd = open(server_fifo_name, O_RDONLY);
    if (server_fd == -1) { perror("Erro open"); return 1; }

    char op, req_pipe[40], notif_pipe[40];
    read(server_fd, &op, 1);
    read(server_fd, req_pipe, 40);
    read(server_fd, notif_pipe, 40);
    close(server_fd);

    if (op == OP_CODE_CONNECT) {
        printf("[SERVIDOR] Cliente conectado. Pipes: %s, %s\n", req_pipe, notif_pipe);
        client_notif_fd = open(notif_pipe, O_WRONLY); 
        client_req_fd = open(req_pipe, O_RDONLY);
        
        char res_op = OP_CODE_CONNECT, res_val = 0;
        write(client_notif_fd, &res_op, 1);
        write(client_notif_fd, &res_val, 1);
    } else {
        cleanup_server(server_fifo_name);
        return 1;
    }

    srand((unsigned int)time(NULL));
    open_debug_file("server_debug.log");
    
    // 3. Carregar e Jogar Níveis
    char lista_niveis[MAX_LEVELS][MAX_FILENAME];
    int n_niveis = find_levels(".", lista_niveis);
    int points = 0;

    for (int i = 0; i < n_niveis; i++) {
        // Verificar se cliente ainda está ligado
        if (fcntl(client_req_fd, F_GETFD) == -1) break;

        printf("[SERVIDOR] Nível %d: %s\n", i+1, lista_niveis[i]);
        
        board_t game_board = {0};
        // Usar load_level_filename em vez de load_level pointer
        if (load_level_filename(&game_board, lista_niveis[i], points) != 0) {
            fprintf(stderr, "Erro ao carregar %s\n", lista_niveis[i]);
            continue;
        }
        strncpy(game_board.level_name, lista_niveis[i], 255);

        // Executar o jogo
        int outcome = run_game(&game_board);

        points = (game_board.n_pacmans > 0) ? game_board.pacmans[0].points : points;
        
        unload_level(&game_board);

        if (outcome == QUIT_GAME) break;
        
        // Pequena pausa entre níveis para o cliente processar
        sleep_ms(1000);
    }

    close_debug_file();
    cleanup_server(server_fifo_name);
    printf("[SERVIDOR] Fim.\n");
    return 0;
}   