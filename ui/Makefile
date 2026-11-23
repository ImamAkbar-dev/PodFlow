# Makefile khusus UI. Makefile utama (inti) tidak disentuh.
#   make -C ui deps   pasang aiohttp ke ui/.venv (sekali saja)
#   make -C ui ui     server C + gateway sekaligus (demo)   -> http://localhost:8080
#   make -C ui run    gateway saja (server C sudah jalan sendiri)
#   make -C ui test   tes UI
.PHONY: deps run test ui

PORT        ?= 8080
SERVER_HOST ?= 127.0.0.1
SERVER_PORT ?= 8888
PY := $(if $(wildcard .venv/bin/python),.venv/bin/python,python3)

deps:
	python3 -m venv .venv
	.venv/bin/pip install -r requirements.txt

run:
	$(PY) gateway/gateway.py --port $(PORT) --server-host $(SERVER_HOST) --server-port $(SERVER_PORT)

test:
	$(PY) -m unittest discover -s tests -v

ui:
	PORT=$(PORT) SERVER_PORT=$(SERVER_PORT) bash scripts/run_demo.sh
