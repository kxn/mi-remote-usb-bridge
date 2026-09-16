# Host-side build & test driver (firmware has its own makefile).
# Usage:  make [target]   on Windows Git Bash, clang64 or mingw gcc.
ifeq ($(OS),Windows_NT)
ifeq ($(origin CC),default)
CC := /c/msys64/clang64/bin/clang.exe
endif
AR ?= /c/msys64/clang64/bin/llvm-ar.exe
LDFLAGS ?= -lws2_32
else
CC ?= cc
AR ?= ar
LDFLAGS ?=
endif

CFLAGS ?= -std=c11 -Wall -Wextra -O2
INC := -Iprotocol/include -Ifirmware/product -Ifirmware/adapters -Ifirmware/adapters/rc003 -Ifirmware/adapters/hogp -Iclient/c
SIM_SRC := sim/sim_main.c sim/sim_remote.c firmware/adapters/rc003/rc003_adapter.c firmware/adapters/rc003/rc003_atvv.c firmware/adapters/hogp/report_map.c

PROT_SRC := protocol/src/rbp_frame.c protocol/src/rbp_tlv.c firmware/product/faults.c
PROD_SRC := firmware/product/rbp_server.c firmware/product/device_model.c \
                firmware/adapters/hogp/report_map.c \
            firmware/adapters/rc003/rc003_atvv.c firmware/adapters/rc003/rc003_adapter.c

BUILD := build/host
HEADERS := $(wildcard protocol/include/rbp/*.h firmware/product/*.h firmware/adapters/*.h firmware/adapters/*/*.h client/c/*.h firmware/wch/*.h firmware/debug/*.h tests/mocks/*.h)

.PHONY: all test protocol vectors sim python-test e2e clean
.DEFAULT_GOAL := all

$(BUILD)/test_input: tests/test_input_c.c client/c/rbp_input.c client/c/rbp_input.h protocol/include/rbp/defs.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(filter %.c,$^)
.PHONY: input-test
input-test: $(BUILD)/test_input
	./$(BUILD)/test_input
test: input-test

all: test sim

$(BUILD)/test_protocol_c $(BUILD)/gen_vectors $(BUILD)/test_product $(BUILD)/test_ima $(BUILD)/test_hogp $(BUILD)/test_board $(BUILD)/test_usb $(BUILD)/test_atvv $(BUILD)/test_gatt $(BUILD)/sim_bridge $(BUILD)/sim_bridge_mcu: $(HEADERS) Makefile

protocol: $(BUILD)/test_protocol_c $(BUILD)/gen_vectors

$(BUILD)/test_protocol_c: tests/test_protocol_c.c $(PROT_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iprotocol/include -o $@ $(filter %.c,$^)

$(BUILD)/gen_vectors: tests/gen_vectors.c $(PROT_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iprotocol/include -o $@ $(filter %.c,$^)

vectors: $(BUILD)/gen_vectors
	./$(BUILD)/gen_vectors protocol/vectors/golden.json

$(BUILD)/test_product: tests/test_product_c.c tests/test_audio_product.inc $(PROT_SRC) $(PROD_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(filter %.c,$^)

$(BUILD)/test_ima: tests/test_ima_c.c client/c/ima_decoder.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(filter %.c,$^)

$(BUILD)/test_hogp: tests/test_hogp_c.c $(PROD_SRC) $(PROT_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(filter %.c,$^)

HOGP_TEST := $(BUILD)/test_hogp

MOCK_INC := -Itests/mocks -Ifirmware/wch/vendor/periph/inc
$(BUILD)/test_gatt: tests/test_gatt_c.c firmware/wch/wch_gatt.c firmware/wch/vendor/lib/CH58xBLE_LIB.h
	$(CC) $(CFLAGS) $(INC) -Ifirmware/wch/vendor/lib -o $@ tests/test_gatt_c.c firmware/product/faults.c
$(BUILD)/test_board: tests/test_board_c.c firmware/wch/board.c $(PROT_SRC)
	$(CC) $(CFLAGS) $(INC) $(MOCK_INC) -o $@ tests/test_board_c.c $(PROT_SRC)

$(BUILD)/test_usb: tests/test_usb_c.c firmware/wch/usb_cdc.c
	$(CC) $(CFLAGS) $(INC) $(MOCK_INC) -o $@ tests/test_usb_c.c

$(BUILD)/test_atvv: tests/test_atvv_c.c firmware/adapters/rc003/rc003_atvv.c client/c/ima_decoder.c firmware/product/faults.c
	$(CC) $(CFLAGS) $(INC) -o $@ $(filter %.c,$^)

test: protocol $(BUILD)/test_product $(BUILD)/test_ima $(HOGP_TEST) $(BUILD)/test_board $(BUILD)/test_usb $(BUILD)/test_atvv $(BUILD)/test_gatt
	./$(BUILD)/test_protocol_c && ./$(BUILD)/test_product && \
	./$(BUILD)/test_ima $(and $(HOGP_TEST),&& ./$(BUILD)/test_hogp)
	./$(BUILD)/test_board && ./$(BUILD)/test_usb && ./$(BUILD)/test_atvv
	./$(BUILD)/test_gatt
	./$(BUILD)/test_debug

$(BUILD)/test_debug: tests/test_debug_c.c firmware/debug/trace.c firmware/debug/trace.h Makefile
	$(CC) $(CFLAGS) -DRBP_DEBUG -o $@ $(filter %.c,$^)
test: $(BUILD)/test_debug

$(BUILD)/test_adapter_reference: tests/test_adapter_reference_c.c firmware/adapters/rc003/rc003_adapter.c firmware/adapters/rc003/rc003_atvv.c firmware/adapters/hogp/report_map.c client/c/ima_decoder.c firmware/product/device_model.c $(PROT_SRC) $(HEADERS) Makefile
	$(CC) $(CFLAGS) $(INC) -o $@ $(filter-out firmware/adapters/rc003/rc003_adapter.c,$(filter %.c,$^))

reference-test: $(BUILD)/test_adapter_reference
	./$(BUILD)/test_adapter_reference

$(BUILD)/test_pair_backend: tests/test_pair_backend_c.c firmware/wch/wch_central.c $(HEADERS)
	$(CC) $(CFLAGS) -Wno-unused-parameter $(INC) -Ifirmware/wch/vendor/lib -o $@ tests/test_pair_backend_c.c firmware/product/faults.c

pair-test: $(BUILD)/test_pair_backend
	./$(BUILD)/test_pair_backend
test: pair-test

$(BUILD)/sim_bridge_debug: $(SIM_SRC) $(PROT_SRC) $(PROD_SRC) firmware/debug/trace.c $(HEADERS) Makefile
	$(CC) $(CFLAGS) $(INC) -DRBP_DEBUG -DRBP_AUDIO_POOL_SIZE=448u -DRBP_OUT_RING_SIZE=768u -DRBP_CTL_SLOTS=6u -DRBP_CTL_BYTES=1536u -DRBP_MEDIA_DEPTH=8u -o $@ $(filter %.c,$^) $(LDFLAGS)
sim-debug: $(BUILD)/sim_bridge_debug

$(BUILD)/sim_bridge: $(SIM_SRC) $(PROT_SRC) $(PROD_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(filter %.c,$^) $(LDFLAGS)

sim: $(BUILD)/sim_bridge

# Same queue capacities as the MCU; SDK/hardware are still mocked.
$(BUILD)/sim_bridge_mcu: $(SIM_SRC) $(PROT_SRC) $(PROD_SRC)
	$(CC) $(CFLAGS) $(INC) -DRBP_OUT_RING_SIZE=768u -DRBP_AUDIO_POOL_SIZE=960u -DRBP_CTL_SLOTS=6u -DRBP_CTL_BYTES=1536u -DRBP_MEDIA_DEPTH=8u -o $@ $(filter %.c,$^) $(LDFLAGS)

sim-mcu: $(BUILD)/sim_bridge_mcu

python-test:
	python tests/test_rbp_python.py
	python tests/test_worker_races.py
	python tests/test_serial_transport.py

e2e: $(BUILD)/sim_bridge python-test
	python tests/test_e2e.py

clean:
	rm -rf build/host

$(BUILD)/test_smp_identity: tests/test_smp_identity_c.c firmware/wch/wch_smp_identity.c
	$(CC) $(CFLAGS) -Ifirmware/wch/vendor/lib -o $@ $<
.PHONY: smp-test
smp-test: $(BUILD)/test_smp_identity
	./$(BUILD)/test_smp_identity
test: smp-test

$(BUILD)/test_encoded_burst: tests/test_product_c.c tests/test_audio_product.inc $(PROT_SRC) $(PROD_SRC) $(HEADERS)
	$(CC) $(CFLAGS) $(INC) -DRBP_BURST_TEST_ONLY -DRBP_AUDIO_POOL_SIZE=448u -DRBP_OUT_RING_SIZE=768u -o $@ $(filter %.c,$^)
.PHONY: burst-test
burst-test: $(BUILD)/test_encoded_burst
	./$(BUILD)/test_encoded_burst
test: burst-test

$(BUILD)/rbp_decoder.dll: client/c/rbp_decoder.c client/c/ima_decoder.c client/c/rbp_decoder.h client/c/ima_decoder.h protocol/include/rbp/audio.h
	$(CC) $(CFLAGS) $(INC) -shared -o $@ $(filter %.c,$^)
decoder: $(BUILD)/rbp_decoder.dll

$(BUILD)/test_sdk_rx_probe: tests/test_sdk_rx_probe.c firmware/wch/sdk_rx_probe.c firmware/wch/sdk_rx_probe.h firmware/debug/trace.h Makefile
	$(CC) $(CFLAGS) -o $@ tests/test_sdk_rx_probe.c
.PHONY: sdk-probe-test
sdk-probe-test: $(BUILD)/test_sdk_rx_probe
	./$(BUILD)/test_sdk_rx_probe
test: sdk-probe-test

$(BUILD)/test_debug_preempt: tests/test_debug_preempt.c firmware/debug/trace.c firmware/debug/trace.h Makefile
	$(CC) $(CFLAGS) -DRBP_DEBUG -DRBP_TRACE_TEST -o $@ tests/test_debug_preempt.c firmware/debug/trace.c
.PHONY: trace-preempt-test
trace-preempt-test: $(BUILD)/test_debug_preempt
	./$(BUILD)/test_debug_preempt
test: trace-preempt-test

$(BUILD)/test_faults: tests/test_faults_c.c firmware/product/faults.c firmware/product/faults.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_faults_c.c firmware/product/faults.c

.PHONY: fault-test
fault-test: $(BUILD)/test_faults
	./$(BUILD)/test_faults
test: fault-test

$(BUILD)/test_gatt $(BUILD)/test_pair_backend: firmware/product/faults.c

$(BUILD)/test_unicom: tests/test_unicom_c.c firmware/adapters/rc003/rc003_atvv.c firmware/adapters/hogp/report_map.c firmware/product/device_model.c $(PROT_SRC) $(HEADERS) firmware/adapters/rc003/rc003_adapter.c firmware/adapters/rc003/unicom_profile.inc firmware/adapters/rc003/unicom_runtime.inc
	$(CC) $(CFLAGS) $(INC) -o $@ $(filter-out firmware/adapters/rc003/rc003_adapter.c,$(filter %.c,$^))
.PHONY: unicom-test
unicom-test: $(BUILD)/test_unicom
	./$(BUILD)/test_unicom
test: unicom-test
$(BUILD)/test_adapter_reference $(BUILD)/test_hogp $(BUILD)/test_product $(BUILD)/sim_bridge $(BUILD)/sim_bridge_mcu $(BUILD)/sim_bridge_debug: firmware/adapters/rc003/unicom_profile.inc firmware/adapters/rc003/unicom_runtime.inc

$(BUILD)/sim_bridge $(BUILD)/sim_bridge_mcu $(BUILD)/sim_bridge_debug: CFLAGS += -DRBP_SIMULATOR
