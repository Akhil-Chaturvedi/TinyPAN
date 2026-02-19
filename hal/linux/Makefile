# TinyPAN Linux Makefile
# Build with: make
# Test with: make test

CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -O2 -g
CFLAGS += -I../../include -I../../src

# BlueZ libraries
LDFLAGS = -lbluetooth

# Source files
CORE_SRC = ../../src/tinypan.c \
           ../../src/tinypan_bnep.c \
           ../../src/tinypan_supervisor.c

HAL_SRC = tinypan_hal_bluez.c

# Test sources
TEST_BNEP_SRC = ../../tests/test_bnep.c
MOCK_HAL_SRC = ../../hal/mock/tinypan_hal_mock.c

# Targets
.PHONY: all clean test demo

all: test_bnep_linux demo_linux

# BNEP tests (uses mock HAL, works without BT hardware)
test_bnep_linux: $(TEST_BNEP_SRC) ../../src/tinypan_bnep.c $(MOCK_HAL_SRC)
	$(CC) $(CFLAGS) -DTINYPAN_ENABLE_DEBUG=1 -o $@ $^
	@echo "Built: test_bnep_linux"

# Full demo with real BlueZ HAL
demo_linux: demo_main.c $(CORE_SRC) $(HAL_SRC)
	$(CC) $(CFLAGS) -DTINYPAN_ENABLE_DEBUG=1 -o $@ $^ $(LDFLAGS)
	@echo "Built: demo_linux"

# Run tests
test: test_bnep_linux
	@echo ""
	@echo "Running BNEP tests..."
	@./test_bnep_linux

clean:
	rm -f test_bnep_linux demo_linux *.o

# Help
help:
	@echo "TinyPAN Linux Build"
	@echo "==================="
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build everything"
	@echo "  test             - Run tests (no hardware needed)"
	@echo "  test_bnep_linux  - Build BNEP unit tests"
	@echo "  demo_linux       - Build real Bluetooth demo"
	@echo "  clean            - Remove built files"
	@echo ""
	@echo "Requirements:"
	@echo "  - BlueZ development libraries (bluez-dev)"
	@echo "  - GCC"
