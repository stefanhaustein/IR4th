CC      = clang
CFLAGS  = -std=c89 -Wall -Wextra -O2 -Isrc

BUILD   = build
TARGET  = $(BUILD)/ir4c
SRCS    = src/ir4c.c src/frontend.c src/codebuf.c src/be_z80.c src/be_6502.c src/be_arm64.c
OBJS    = $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all test test-z80 test-gforth rt-z80 clean

all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/ir4c.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/be_z80.o: src/be_z80_math48.h

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) -lm

test: $(TARGET) tests/fib.ir4 tests/core.ir4 tests/muldiv.ir4 tests/mem.ir4 tests/float.ir4
	./$(TARGET) -t z80   -o $(BUILD)/fib_z80.bin     tests/fib.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/fib_6502.bin    tests/fib.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/fib_arm64.bin   --run tests/fib.ir4
	./$(TARGET) -t z80   -o $(BUILD)/core_z80.bin    tests/core.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/core_6502.bin   tests/core.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/core_arm64.bin  --run tests/core.ir4
	./$(TARGET) -t z80   -o $(BUILD)/muldiv_z80.bin  tests/muldiv.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/muldiv_arm64.bin --run tests/muldiv.ir4
	./$(TARGET) -t z80   -o $(BUILD)/mem_z80.bin     tests/mem.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/mem_6502.bin    tests/mem.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/mem_arm64.bin   --run tests/mem.ir4
	./$(TARGET) -t z80   -o $(BUILD)/float_z80.bin     tests/float.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/float_arm64.bin --run tests/float.ir4

test-z80: $(TARGET) tests/fib.ir4 tests/core.ir4 tests/muldiv.ir4 tests/mem.ir4 tests/float.ir4
	./$(TARGET) -t z80 --cpm -o $(BUILD)/fib.com    tests/fib.ir4
	./$(TARGET) -t z80 --cpm -o $(BUILD)/core.com   tests/core.ir4
	./$(TARGET) -t z80 --cpm -o $(BUILD)/muldiv.com tests/muldiv.ir4
	./$(TARGET) -t z80 --cpm -o $(BUILD)/mem.com    tests/mem.ir4
	./$(TARGET) -t z80 --cpm -o $(BUILD)/float.com  tests/float.ir4
	@if command -v tnylpo >/dev/null 2>&1; then \
		out=$$(cd $(BUILD) && tnylpo fib.com | tr -d '\r\n'); \
		echo "tnylpo fib.com -> $$out"; \
		test "$$out" = "110"; \
		out=$$(cd $(BUILD) && tnylpo core.com | tr -d '\r\n'); \
		echo "tnylpo core.com -> $$out"; \
		test "$$out" = "0"; \
		out=$$(cd $(BUILD) && tnylpo muldiv.com | tr -d '\r\n'); \
		echo "tnylpo muldiv.com -> $$out"; \
		test "$$out" = "0"; \
		out=$$(cd $(BUILD) && tnylpo mem.com | tr -d '\r\n'); \
		echo "tnylpo mem.com -> $$out"; \
		test "$$out" = "0"; \
		out=$$(cd $(BUILD) && tnylpo float.com | tr -d '\r\n'); \
		echo "tnylpo float.com -> $$out"; \
		test "$$out" = "0"; \
	else \
		echo "tnylpo not found; skipping Z80 execution"; \
	fi

test-gforth: ir4th.fs tests/fib.ir4 tests/core.ir4 tests/mem.ir4
	@out=$$(gforth ir4th.fs tests/fib.ir4 -e "main . cr bye" | tr -d ' \r\n'); \
	echo "gforth fib.ir4 -> $$out"; \
	test "$$out" = "110"
	@out=$$(gforth ir4th.fs tests/core.ir4 -e "main . cr bye" | tr -d ' \r\n'); \
	echo "gforth core.ir4 -> $$out"; \
	test "$$out" = "0"
	@out=$$(gforth ir4th.fs tests/mem.ir4 -e "main . cr bye" | tr -d ' \r\n'); \
	echo "gforth mem.ir4 -> $$out"; \
	test "$$out" = "0"

rt-z80:
	python3 tools/gen_math48.py --input tools/math48.asm --output src/be_z80_math48.h

clean:
	rm -rf $(BUILD)
