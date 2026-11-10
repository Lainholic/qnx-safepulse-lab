# BlackBerry QNX SafePulse Lab — Native QNX 8.0 Makefile
# Usage: make all (after running qnxsdp-env.bat / qnxsdp-env.sh)

CC      ?= qcc
TARGET  ?= -Vgcc_ntox86_64
CFLAGS  ?= $(TARGET) -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Iinclude
LDFLAGS ?= -lm

BINS = \
	test_linter_qnx \
	safepulse_resmgr \
	safepulse_watcher \
	test_resmgr_client \
	safepulse_bridge \
	ecu/ecu_steering \
	ecu/ecu_braking \
	ecu/ecu_powertrain \
	test_stress_1000hz \
	test_autosar_e2e \
	test_durability_1m \
	benchmark_wcet \
	test_ffi_isolation

.PHONY: all clean

all: $(BINS)

test_linter_qnx: test_linter.c resmgr/ringbuffer.c linter/linter_engine.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

safepulse_resmgr: resmgr/safepulse_resmgr.c resmgr/ringbuffer.c linter/linter_engine.c linter/autosar_e2e.c resmgr/safe_state_fsm.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

safepulse_watcher: safepulse_watcher.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test_resmgr_client: test_resmgr_client.c resmgr/ringbuffer.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

safepulse_bridge: safepulse_bridge.c resmgr/ringbuffer.c
	$(CC) $(CFLAGS) $^ -o $@ -lsocket $(LDFLAGS)

ecu/ecu_steering: ecu/ecu_steering.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

ecu/ecu_braking: ecu/ecu_braking.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

ecu/ecu_powertrain: ecu/ecu_powertrain.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test_stress_1000hz: test_stress_1000hz.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test_autosar_e2e: test_autosar_e2e.c linter/autosar_e2e.c resmgr/safe_state_fsm.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test_durability_1m: test_durability_1m.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

benchmark_wcet: benchmark_wcet.c resmgr/ringbuffer.c linter/linter_engine.c linter/autosar_e2e.c resmgr/safe_state_fsm.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test_ffi_isolation: test_ffi_isolation.c resmgr/ringbuffer.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(BINS) *.o resmgr/*.o linter/*.o ecu/*.o
