CXX ?= g++
CXXSTD = -std=c++17
WARNFLAGS = -Wall -Wextra
INCLUDES = -I. -Ithird_party -Isrc

SRCS = src/skip_row.cpp src/skip_row_fixed.cpp src/count_rows.cpp tests/test_row_count.cpp
LIBS = -lz

BUILD_DIR = build
FIXTURES_DIR = tests/fixtures
# Fixtures at the real DBMS_DEFAULT_BUFFER_SIZE scale (not committed — see
# .gitignore comment — regenerated on demand since they're large and
# trivially reproducible from the script).
SCALE_FIXTURES = $(FIXTURES_DIR)/boundary_1048576.csv.gz $(FIXTURES_DIR)/control_1048576.csv.gz

.PHONY: test test-asan clean fixtures

test: $(BUILD_DIR)/test_row_count
	$(BUILD_DIR)/test_row_count

test-asan: $(BUILD_DIR)/test_row_count_asan
	$(BUILD_DIR)/test_row_count_asan

fixtures: $(SCALE_FIXTURES)

$(SCALE_FIXTURES):
	python3 $(FIXTURES_DIR)/generate_test_files.py 1048576 $(FIXTURES_DIR)

$(BUILD_DIR)/test_row_count: $(SRCS) src/mini_read_buffer.h src/skip_row.h $(SCALE_FIXTURES) | $(BUILD_DIR)
	$(CXX) $(CXXSTD) $(WARNFLAGS) -O1 -g $(INCLUDES) $(SRCS) $(LIBS) -o $@

$(BUILD_DIR)/test_row_count_asan: $(SRCS) src/mini_read_buffer.h src/skip_row.h $(SCALE_FIXTURES) | $(BUILD_DIR)
	$(CXX) $(CXXSTD) $(WARNFLAGS) -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(INCLUDES) $(SRCS) $(LIBS) -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
