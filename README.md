# PacmanIST — Multi-Client Distributed Game in C

[![Language](https://img.shields.io/badge/Language-C17-blue.svg)](https://en.wikipedia.org/wiki/C17_(C_standard_revision))
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20POSIX-orange.svg)](https://en.wikipedia.org/wiki/POSIX)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A distributed, concurrent implementation of the classic Pacman game built in C for POSIX/Linux systems. The project adopts a **Client-Server architecture** using **Named Pipes (FIFOs)** for inter-process communication and **POSIX Threads (`pthread`) with mutexes** for safe concurrent multi-client gameplay.

Developed as part of the **Operating Systems (Sistemas Operativos)** course at **Instituto Superior Técnico (IST), Universidade de Lisboa**.

> [!NOTE]
> This project represents **Part 2** (Distributed Multi-Client Architecture) of the Sistemas Operativos project. It builds directly upon the single-process multithreaded game engine developed in [Part 1 (Concurrent Engine)](https://github.com/DeastV/distributed-pacman-c-part1).

---

## Architecture Overview

```mermaid
flowchart TD
    subgraph Server["Game Server (server/bin/Pacmanist)"]
        Loop["Game Loop & Board Matrix"]
        Sync["POSIX Threads & Mutex Synchronization"]
    end

    subgraph Client1["Client 1 Terminal (ncurses)"]
        UI1["Display & Input"]
    end

    subgraph Client2["Client 2 Terminal (ncurses)"]
        UI2["Display & Input"]
    end

    UI1 -->|"Keypress IPC (Req Pipe)"| Loop
    Loop -->|"Board State Updates (Notif Pipe)"| UI1
    UI2 -->|"Keypress IPC (Req Pipe)"| Loop
    Loop -->|"Board State Updates (Notif Pipe)"| UI2
```

### Core Features

* **Client-Server Separation:** The game engine runs independently on the server, decoupled from player display terminals.
* **Inter-Process Communication (IPC):**
  * Handshake connection over a well-known server FIFO (`/tmp/server_fifo`).
  * Dedicated per-client bi-directional channels:
    * **Request Pipe (`req_pipe`):** Transmits player input commands (WASD) from client to server.
    * **Notification Pipe (`notif_pipe`):** Streams binary serialized board states, scores, and status flags from server to client.
* **Concurrency & Synchronization:**
  * Multi-threaded server handling independent client sessions simultaneously.
  * Mutex-protected critical sections ensuring thread-safe access to the shared game board and agent movement logic.
  * Deadlock avoidance and graceful disconnect detection (`SIGPIPE` / broken FIFO handling).
* **Terminal Interface:** Built with `ncurses` providing real-time rendering, smooth refresh loops, and keyboard event handling.

---

## Project Structure

```
.
├── Makefile                # Master Makefile (builds server and client)
├── .gitignore              # Ignores binaries, objects, and logs
├── README.md               # Project documentation
├── server/                 # Server component
│   ├── Makefile            # Server build configuration
│   ├── include/            # Header files (board.h, display.h, protocol.h)
│   ├── src/                # Game loop, board logic, concurrency
│   └── files/              # Level files (*.lvl) and monster paths (*.m)
└── client/                 # Client component
    ├── Makefile            # Client build configuration
    ├── include/            # Header files (api.h, parser.h, display.h, protocol.h)
    └── src/                # Client main, API abstraction, ncurses rendering
```

---

## Dependencies

* **GCC** (with C17 standard and POSIX compliance)
* **GNU Make**
* **NCurses library**

On Ubuntu / Debian:
```bash
sudo apt-get update
sudo apt-get install build-essential libncurses-dev
```

---

## Compilation

The project uses GNU Make for modular builds:

```bash
# Compile both the server and client
make all

# Or compile individually
make server
make client

# Clean all generated binaries and object files
make clean
```

---

## Running the Game

To play with multiple terminals, start the server first, then connect one or more clients.

### 1. Start the Server (Terminal 1)
```bash
make run-server
# Or manually:
# cd server && ./bin/Pacmanist files 2 /tmp/server_fifo
```
* Arguments:
  1. `files`: Directory containing levels (`.lvl`) and monster files (`.m`).
  2. `2`: Maximum number of simultaneous clients/players.
  3. `/tmp/server_fifo`: Server connection FIFO path.

### 2. Connect a Client (Terminal 2)
```bash
make run-client
# Or manually:
# cd client && ./bin/client 1 /tmp/server_fifo
```
* Arguments:
  1. `1`: Player/Client ID.
  2. `/tmp/server_fifo`: Target server FIFO path.

Use the `W`, `A`, `S`, `D` keys or arrow keys to navigate the Pacman. Press `Q` to quit.

---

## Known Limitations

* **Local Host IPC Boundary:** Communication between game server and client terminals is implemented via POSIX FIFOs (`mkfifo`), binding gameplay processes to the same host OS without network sockets.
* **Static Terminal Geometry:** The `ncurses` client rendering loop assumes static terminal dimensions; dynamic terminal window resizing during active play is not handled.

---

## Credits

* **David Vasques** ([@DeastV](https://github.com/DeastV)), **Guilherme Marques** ([@marques-jpg](https://github.com/marques-jpg))
* Collaborative group coursework developed for Sistemas Operativos at Instituto Superior Técnico, Universidade de Lisboa. Map definitions (`.lvl`) and monster movement scripts (`.m`) provided by the teaching staff.
