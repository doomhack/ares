BUILD_DIR=build
include $(N64_INST)/include/n64.mk

all: n64calib.z64

$(BUILD_DIR)/n64calib.elf: $(BUILD_DIR)/calib.o $(BUILD_DIR)/cpu_loops.o $(BUILD_DIR)/rsp_calib.o

n64calib.z64: N64_ROM_TITLE="N64 Calibration"

clean:
	rm -rf $(BUILD_DIR) n64calib.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
