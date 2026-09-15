# CH582F bridge firmware (cross build, MounRiver GCC 8.2.0, rv32imac/ilp32).
# Usage: make -f firmware/Makefile   (host tests stay on ./Makefile)
#
# Board conditions (docs/firmware-bringup.md):
#   - CLK_OSC32K=0: central role requires the external 32 kHz crystal
#   - BLE SNV block stays at the default 0x7E00; our peer store sits below
#     it at 0x7C00 (firmware/wch/board.h)

# invoked from the project root:  make -f firmware/Makefile
TOOLBIN ?= $(shell pwd)/tools/toolchain/riscv-none-embed-gcc8/bin
export PATH := $(TOOLBIN):$(PATH)

CC      := $(TOOLBIN)/riscv-none-embed-gcc.exe
OBJDUMP := $(TOOLBIN)/riscv-none-embed-objdump.exe
OBJCOPY := $(TOOLBIN)/riscv-none-embed-objcopy.exe
SIZE    := $(TOOLBIN)/riscv-none-embed-size.exe

DEBUG ?= 0
SDK_RX_PROBE ?= 0
HOST_VOICE_START ?= 0
.DEFAULT_GOAL := all
VERSION ?= 0.6.5
BUILD   := build/fw
ifeq ($(DEBUG),1)
BUILD := build/fw-debug
endif

SDK     := firmware/wch/vendor
INC := -I$(SDK)/hal \
       -I$(SDK)/lib \
       -I$(SDK)/periph/inc \
       -Ifirmware/product \
       -Ifirmware/adapters \
       -Ifirmware/adapters/rc003 \
       -Ifirmware/adapters/hogp \
       \
       -Iprotocol/include

# SDK configuration (see $(SDK)/hal/config.h)
DEFS := -DCLK_OSC32K=0 -DRBP_FIRMWARE_SINGLETON \
        -DCENTRAL_MAX_CONNECTION=1 \
        -DPERIPHERAL_MAX_CONNECTION=0 \
        -DBLE_MEMHEAP_SIZE=6144 \
        -DBLE_BUFF_MAX_LEN=251 \
        -DBLE_BUFF_NUM=5 \
        -DBLE_TX_NUM_EVENT=1 \
        -DBLE_SNV=1 \
        -DHAL_SLEEP=FALSE \
        -DHAL_LED=FALSE \
        -DHAL_KEY=FALSE \
        -DFW_VERSION=\"$(VERSION)\" \
        -DRBP_OUT_RING_SIZE=768u \
        -DRBP_AUDIO_POOL_SIZE=960u \
        -DRBP_CTL_SLOTS=6u -DRBP_CTL_BYTES=1536u \
        -DRBP_MEDIA_DEPTH=8u

ifeq ($(DEBUG),1)
DEFS := $(filter-out -DRBP_AUDIO_POOL_SIZE=960u -DFW_VERSION=\"$(VERSION)\",$(DEFS)) -DRBP_AUDIO_POOL_SIZE=448u -DFW_VERSION=\"$(VERSION)-debug\" -DRBP_DEBUG
endif

ifeq ($(SDK_RX_PROBE),1)
ifneq ($(DEBUG),1)
$(error SDK_RX_PROBE requires DEBUG=1)
endif
DEFS += -DRBP_SDK_RX_PROBE
endif

# Unverified on RC003: remote acknowledges OPEN but supplies no audio.
# Never enable this experiment implicitly with DEBUG.
ifeq ($(HOST_VOICE_START),1)
DEFS += -DRBP_EXPERIMENTAL_HOST_VOICE_START
endif

CFLAGS := -std=gnu99 -march=rv32imac -mabi=ilp32 -mcmodel=medany \
          -msmall-data-limit=8 -Os -g3 -Wall -Wextra \
          -MMD -MP -fstack-usage -fno-common -ffunction-sections -fdata-sections \
          $(DEFS) $(INC)

LDFLAGS := -march=rv32imac -mabi=ilp32 -mcmodel=medany -nostartfiles --specs=nano.specs \
           -Xlinker --gc-sections -Wl,-Map,$(BUILD)/ch582f.map \
           -Tfirmware/wch/bridge.ld

LIBS := $(SDK)/lib/LIBCH58xBLE.a $(SDK)/periph/libISP583.a -lm
LDFLAGS += -Wl,--wrap=smpInitiatorProcessIncoming
ifeq ($(SDK_RX_PROBE),1)
LDFLAGS += -Wl,--wrap=ble_ll_chkcrc
LDFLAGS += -Wl,--wrap=AES_DevPktDec
endif

# Private SDK layout used by wch_smp_identity.c is tied to this exact archive.
.PHONY: sdk-identity-check
sdk-identity-check:
	@echo '13bd910bc647ce7ba2bc9a36de2fa5bed987ef83b9dacf5045f653bb062c67ce  $(SDK)/lib/LIBCH58xBLE.a' | sha256sum -c -

# project sources
PROT := protocol/src/rbp_frame.c protocol/src/rbp_tlv.c firmware/product/faults.c
PROD := firmware/product/rbp_server.c \
        firmware/product/device_model.c \
        firmware/adapters/hogp/report_map.c \
        firmware/adapters/rc003/rc003_atvv.c \
        firmware/adapters/rc003/rc003_adapter.c
WCH := firmware/wch/main.c \
       firmware/wch/wch_smp_identity.c \
       firmware/wch/board.c \
       firmware/wch/usb_cdc.c \
       firmware/wch/wch_gatt.c \
       firmware/wch/wch_central.c        firmware/wch/syscalls.c
ifeq ($(SDK_RX_PROBE),1)
WCH += firmware/wch/sdk_rx_probe.c
endif
VEND := $(SDK)/hal/MCU.c \
        $(SDK)/hal/RTC.c \
        $(SDK)/hal/SLEEP.c \
        $(SDK)/periph/CH58x_clk.c \
        $(SDK)/periph/CH58x_gpio.c \
        $(SDK)/periph/CH58x_flash.c \
        $(SDK)/periph/CH58x_sys.c \
        $(SDK)/periph/CH58x_pwr.c \
        $(SDK)/periph/CH58x_uart1.c \
        $(SDK)/periph/CH58x_usbdev.c \
        $(SDK)/periph/CH58x_adc.c

OBJS := $(addprefix $(BUILD)/, $(PROT:.c=.o) $(PROD:.c=.o) $(WCH:.c=.o) $(VEND:.c=.o))
ifeq ($(DEBUG),1)
OBJS += $(BUILD)/firmware/debug/trace.o
endif

.PHONY: all size clean FORCE
all: $(BUILD)/ch582f.elf size

# Make does not otherwise notice changed command-line feature flags. Keep a
# content-stable stamp so reusing BUILD cannot mix release and debug objects.
$(BUILD)/build-config.txt: FORCE
	@mkdir -p $(BUILD)
	@printf '%s\n' '$(CC)' '$(CFLAGS)' '$(LDFLAGS)' '$(OBJS)' > $@.tmp
	@cmp -s $@.tmp $@ || cp $@.tmp $@

$(OBJS): firmware/Makefile $(BUILD)/build-config.txt

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/ch582f.elf: $(OBJS) $(BUILD)/build-config.txt $(SDK)/Startup/startup_CH583.S firmware/wch/bridge.ld $(SDK)/lib/LIBCH58xBLE.a $(SDK)/periph/libISP583.a | sdk-identity-check
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SDK)/Startup/startup_CH583.S $(OBJS) $(LDFLAGS) $(LIBS)

size: $(BUILD)/ch582f.elf
	$(SIZE) $<
	$(OBJDUMP) -x $< > $(BUILD)/ch582f.dump

$(BUILD)/ch582f.bin: $(BUILD)/ch582f.elf
	$(OBJCOPY) -O binary $< $@

all: $(BUILD)/ch582f.bin

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name "*.d" 2>/dev/null)
