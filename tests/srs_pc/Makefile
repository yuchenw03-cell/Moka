# tests/srs_pc —— 墨卡 SM-2 调度引擎 PC 单测
# 直接编译 main/src/srs/*.c + 测试文件，无 ESP-IDF 依赖。
CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2
SRS_DIR := ../../main/src/srs
BIN     := srs_pc_tests

SRCS := $(SRS_DIR)/srs_engine.c \
        $(SRS_DIR)/srs_queue.c \
        $(SRS_DIR)/srs_store.c \
        $(SRS_DIR)/wbx.c \
        test_srs_pc.c

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) -I$(SRS_DIR) -o $@ $(SRCS)

test: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN) t9.srs t9.srs.tmp t9tmp_src.srs t9tmp_src.srs.tmp \
	      t10_fixture.wbx t10_bad.wbx t11.stats

.PHONY: test clean
