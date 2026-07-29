CC       = gcc
CFLAGS   = -O2 -std=c99 -Wall -Wextra -pedantic
LDFLAGS  =
SRC_DIR  = src
BUILD_DIR = build
OUTPUT   = $(BUILD_DIR)/memory
LIBS     =

# Source files (excluding semantic.c unless explicit)
SRCS     = $(wildcard $(SRC_DIR)/*.c)
# Filter out semantic.c for standard build
SRCS_STD = $(filter-out $(SRC_DIR)/semantic.c, $(SRCS))
# Full sources including semantic
SRCS_SEM = $(SRCS)

OBJS_STD = $(SRCS_STD:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
OBJS_SEM = $(SRCS_SEM:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

.PHONY: all semantic debug test tools clean install

all: $(OUTPUT)

$(OUTPUT): $(OBJS_STD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Semantic build
semantic: CFLAGS += -DHIPOOL_ENABLE_SEMANTIC
semantic: LIBS += -lm
semantic: $(OUTPUT)-semantic

$(OUTPUT)-semantic: $(OBJS_SEM)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

$(BUILD_DIR)/semantic.o: $(SRC_DIR)/semantic.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Debug build
debug: CFLAGS = -O0 -g -std=c99 -Wall -Wextra -pedantic
debug: $(OUTPUT)-debug

$(OUTPUT)-debug: $(OBJS_STD)
	$(CC) $(CFLAGS) -o $@ $^

# Thread-safe build
threadsafe: CFLAGS += -DHIPOOL_USE_MUTEX
threadsafe: LIBS += -lpthread
threadsafe: $(OUTPUT)-ts

$(OUTPUT)-ts: $(OBJS_STD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lpthread

# Tools
tools:
	$(CC) $(CFLAGS) tools/dict_distill.c -o $(BUILD_DIR)/dict_distill

# Tests
# [P0-4] 单元测试: 直接链接 src/*.c (除 main.c/semantic.c), 测试内部数据结构。
UNIT_SRCS = $(filter-out $(SRC_DIR)/main.c $(SRC_DIR)/semantic.c, $(SRCS))
UNIT_OBJS = $(UNIT_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

$(BUILD_DIR)/unit_test: tests/unit_test.c $(UNIT_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/unit_test.c $(UNIT_OBJS) $(LDFLAGS) $(LIBS)

test-unit: $(BUILD_DIR)/unit_test
	./$(BUILD_DIR)/unit_test

test: all test-unit
	cd tests && bash test.sh

# Clean
clean:
	rm -rf $(BUILD_DIR)

# Install
install: all
	cp $(OUTPUT) /usr/local/bin/memory

# Help
help:
	@echo "Targets:"
	@echo "  all          - Build standard memory binary (default)"
	@echo "  semantic     - Build with HIPOOL_ENABLE_SEMANTIC (semantic search)"
	@echo "  debug        - Build with debug symbols (-O0 -g)"
	@echo "  threadsafe   - Build with -DHIPOOL_USE_MUTEX -lpthread"
	@echo "  tools        - Build auxiliary tools (dict_distill)"
	@echo "  test         - Run unit tests then shell tests"
	@echo "  test-unit    - Run C unit tests only"
	@echo "  clean        - Remove build artifacts"
	@echo "  install      - Copy binary to /usr/local/bin/"
