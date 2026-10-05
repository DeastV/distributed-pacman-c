# Root Makefile - Builds both server and client from subdirectories

.PHONY: all server client clean run-server run-client

all: server client

server:
	@echo "=== Building Server ==="
	$(MAKE) -C server

client:
	@echo "=== Building Client ==="
	$(MAKE) -C client

clean:
	@echo "=== Cleaning Server ==="
	$(MAKE) -C server clean
	@echo "=== Cleaning Client ==="
	$(MAKE) -C client clean
	@echo "=== Cleaning complete ==="

# Helper targets for running
run-server: server
	@cd server && ./bin/Pacmanist files 2 /tmp/server_fifo

run-client: client
	@cd client && ./bin/client 1 /tmp/server_fifo