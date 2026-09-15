CXX ?= g++
CXXSTD = -std=c++17
WARNFLAGS = -Wall -Wextra
INCLUDES = -I. -Ithird_party -Isrc

SRCS = src/skip_row.cpp src/skip_row_fixed.cpp src/count_rows.cpp tests/test_row_count.cpp
LIBS = -lz

BUILD_DIR = build

.PHONY: test test-asan clean

test: $(BUILD_DIR)/test_row_count
	$(BUILD_DIR)/test_row_count

test-asan: $(BUILD_DIR)/test_row_count_asan
	$(BUILD_DIR)/test_row_count_asan

$(BUILD_DIR)/test_row_count: $(SRCS) src/mini_read_buffer.h src/skip_row.h | $(BUILD_DIR)
	$(CXX) $(CXXSTD) $(WARNFLAGS) -O1 -g $(INCLUDES) $(SRCS) $(LIBS) -o $@

$(BUILD_DIR)/test_row_count_asan: $(SRCS) src/mini_read_buffer.h src/skip_row.h | $(BUILD_DIR)
	$(CXX) $(CXXSTD) $(WARNFLAGS) -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(INCLUDES) $(SRCS) $(LIBS) -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
