# One command for the whole comparison:   make bench   (about 15 minutes; `make quick` is 3 minutes)
# Then send back the file it prints at the end (benchmarks/results/*.txt).
#
#   make venvs    Python 3.14 (with GIL) and 3.14t (free-threaded, no GIL) virtualenvs with this httpp, uvicorn and uvloop (needs network)
#   make oha      the load generator, one static binary (needs network)
#   make cpp      the C++ server of the suite
#   make check    prints what could disturb the numbers (governor, battery, turbo)
#   make bench    everything above, then the measurement
#   make clean    removes venvs, binaries and build files (results stay)
#
# Variables: SECONDS=5 ROUNDS=3 CONC=64 IO_THREADS=1,2,4 SERVER_CPUS=0-7 LOAD_CPUS=8-11 PYS="3.14 3.14t"

ROOT      := $(abspath ..)
HERE      := $(abspath .)
VENVS     := $(HERE)/.venvs
BIN       := $(HERE)/bin
PYS       ?= 3.14 3.14t
SECONDS   ?= 5
ROUNDS    ?= 3
CONC      ?= 64
IO_THREADS ?= 1,2,4
SERVER_CPUS ?=
LOAD_CPUS ?=
ONLY      ?= hello,cpu
STAMP     := $(shell date +%Y%m%d-%H%M%S)
OUT       ?= $(HERE)/results/$(STAMP).txt
OHA_URL   ?= https://github.com/hatoo/oha/releases/latest/download/oha-linux-amd64
ARCH      := $(shell uname -m)

# 3.13 -> py313, 3.14t -> py314t
venv_dir   = $(VENVS)/py$(subst .,,$(1))
VENV_PYS   = $(foreach v,$(PYS),$(call venv_dir,$(v))/bin/python)
PY_ARGS    = $(subst $(space),$(comma),$(foreach v,$(PYS),$(v)=$(call venv_dir,$(v))/bin/python))
comma := ,
space := $(empty) $(empty)

.PHONY: bench quick venvs oha cpp check clean uv

bench: venvs oha cpp check
	@mkdir -p results
	python3 suite.py --pys "$(PY_ARGS)" --cpp $(BIN)/cpp_bench --oha $(BIN)/oha --seconds $(SECONDS) --rounds $(ROUNDS) \
		--conc $(CONC) --io-threads $(IO_THREADS) --only $(ONLY) --server-cpus "$(SERVER_CPUS)" --load-cpus "$(LOAD_CPUS)" --out $(OUT) 2>&1 | tee $(OUT).log
	@echo; echo "Send me this file: $(OUT)"

quick:
	$(MAKE) bench SECONDS=3 ROUNDS=1 IO_THREADS=1,4

uv:
	@command -v uv >/dev/null || { echo "installing uv (a single binary, in ~/.local/bin)"; curl -LsSf https://astral.sh/uv/install.sh | sh; }

venvs: uv
	@export PATH=$$HOME/.local/bin:$$PATH; for v in $(PYS); do \
		d=$(VENVS)/py$$(echo $$v | tr -d .); \
		case $$v in *t) want=nogil; req=$$v ;; *) want=gil; req=$$v+gil ;; esac; \
		if [ -x $$d/bin/python ]; then \
			have=$$($$d/bin/python -c "import sys;print('gil' if getattr(sys,'_is_gil_enabled',lambda:True)() else 'nogil')"); \
			[ "$$have" = "$$want" ] || { echo "== $$d has the wrong build ($$have, want $$want): recreating"; rm -rf $$d; }; \
		fi; \
		if [ ! -x $$d/bin/python ]; then echo "== venv $$v ($$req)"; uv venv --python $$req $$d || exit 1; fi; \
		echo "== installing httpp (built from this tree) and uvicorn into $$d"; \
		uv pip install --python $$d/bin/python --reinstall $(ROOT) uvicorn httptools || exit 1; \
		uv pip install --python $$d/bin/python uvloop || echo "(no uvloop for $$v: uvicorn uses asyncio there)"; \
	done

oha:
	@mkdir -p $(BIN)
	@if [ ! -x $(BIN)/oha ]; then \
		if command -v oha >/dev/null; then ln -sf $$(command -v oha) $(BIN)/oha; \
		else echo "downloading oha"; curl -fL -o $(BIN)/oha $(OHA_URL) && chmod +x $(BIN)/oha \
			|| { echo "could not download oha: install it (cargo install oha) and rerun"; exit 1; }; fi; \
	fi; $(BIN)/oha --version

# The C++ server links the library built from this tree; cmake and ninja come from pip, no system packages.
cpp: venvs
	@mkdir -p $(BIN)
	PATH=$$HOME/.local/bin:$$PATH uv pip install --python $(word 1,$(VENV_PYS)) cmake ninja
	$(dir $(word 1,$(VENV_PYS)))cmake -S $(ROOT) -B $(HERE)/.build -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DHTTPP_BUILD_TESTS=OFF -DHTTPP_BUILD_PYTHON=OFF -DCMAKE_MAKE_PROGRAM=$(dir $(word 1,$(VENV_PYS)))ninja
	$(dir $(word 1,$(VENV_PYS)))cmake --build $(HERE)/.build --target httpp_core
	$(or $(CXX),g++) -O2 -std=c++17 -I$(ROOT)/include cpp/cpp_bench.cpp -o $(BIN)/cpp_bench \
		-L$(HERE)/.build -lhttpp_core -Wl,-rpath,$(HERE)/.build -pthread

check:
	@echo "== things that disturb the numbers"; \
	g=$$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null); echo "governor: $${g:-unknown}"; \
	[ "$$g" = "performance" ] || echo "  -> not performance. For steadier numbers:  sudo cpupower frequency-set -g performance  (or run it as is and tell me)"; \
	for p in /sys/class/power_supply/A*/online; do [ -r $$p ] && { [ "$$(cat $$p)" = 1 ] && echo "power: plugged in" || echo "power: ON BATTERY -> plug in the charger first"; }; done; \
	echo "close browsers and other heavy programs while it runs."

clean:
	rm -rf $(VENVS) $(BIN) $(HERE)/.build
