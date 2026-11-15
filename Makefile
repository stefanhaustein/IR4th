CC      = clang
CFLAGS  = -std=c89 -Wall -Wextra -O2 -Isrc

BUILD   = build
TARGET  = $(BUILD)/ir4c
SRCS    = src/ir4c.c src/frontend.c src/codebuf.c src/be_z80.c src/be_6502.c src/be_arm64.c
OBJS    = $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all test test-gforth clean

all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/ir4c.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET)

test: $(TARGET) tests/fib.ir4
	./$(TARGET) -t z80   -o $(BUILD)/fib_z80.bin   tests/fib.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/fib_6502.bin  tests/fib.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/fib_arm64.bin --run tests/fib.ir4

test-gforth: tests/fib.ir4
	gforth tests/fib.ir4 -e "main . cr bye"

clean:
	rm -rf $(BUILD)
