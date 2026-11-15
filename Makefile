CC      = clang
CFLAGS  = -std=c89 -Wall -Wextra -O2 -Isrc

BUILD   = build
TARGET  = $(BUILD)/ir4c
SRCS    = src/ir4c.c src/frontend.c src/codebuf.c src/be_z80.c src/be_z80_cpm.c src/be_6502.c src/be_6502_sim.c src/be_arm64.c src/be_arm64_jit.c
OBJS    = $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all test test-z80 test-6502 test-gforth rt-z80 clean

all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/ir4c.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/be_z80.o: src/be_z80.h src/be_z80_math48.h
$(BUILD)/be_z80_cpm.o: src/be_z80.h

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
	./$(TARGET) -t 6502  -o $(BUILD)/muldiv_6502.bin tests/muldiv.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/muldiv_arm64.bin --run tests/muldiv.ir4
	./$(TARGET) -t z80   -o $(BUILD)/mem_z80.bin     tests/mem.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/mem_6502.bin    tests/mem.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/mem_arm64.bin   --run tests/mem.ir4
	./$(TARGET) -t z80   -o $(BUILD)/float_z80.bin     tests/float.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/float_arm64.bin --run tests/float.ir4
	./$(TARGET) -t z80   -o $(BUILD)/req_z80.bin       tests/req.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/req_6502.bin      tests/req.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/req_arm64.bin   --run tests/req.ir4
	./$(TARGET) -t z80   -o $(BUILD)/data_z80.bin      tests/data.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/data_6502.bin     tests/data.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/data_arm64.bin  --run tests/data.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/ext_arm64.bin   --run tests/ext.ir4

test-z80: $(TARGET) tests/fib.ir4 tests/core.ir4 tests/muldiv.ir4 tests/mem.ir4 tests/float.ir4 tests/req.ir4 tests/data.ir4 tests/ext.ir4
	./$(TARGET) -t z80 --cpm -o build/fib.com    tests/fib.ir4
	./$(TARGET) -t z80 --cpm -o build/core.com   tests/core.ir4
	./$(TARGET) -t z80 --cpm -o build/muldiv.com tests/muldiv.ir4
	./$(TARGET) -t z80 --cpm -o build/mem.com    tests/mem.ir4
	./$(TARGET) -t z80 --cpm -o build/float.com  tests/float.ir4
	./$(TARGET) -t z80 --cpm -o build/req.com    tests/req.ir4
	./$(TARGET) -t z80 --cpm -o build/data.com   tests/data.ir4
	./$(TARGET) -t z80 --cpm -o build/ext.com    tests/ext.ir4
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
		out=$$(cd $(BUILD) && tnylpo req.com | tr -d '\r\n'); \
		echo "tnylpo req.com -> $$out"; \
		test "$$out" = "0"; \
		out=$$(cd $(BUILD) && tnylpo data.com | tr -d '\r\n'); \
		echo "tnylpo data.com -> $$out"; \
		test "$$out" = "0"; \
		out=$$(cd $(BUILD) && tnylpo ext.com | tr -d '\r\n'); \
		echo "tnylpo ext.com -> $$out"; \
		test "$$out" = "OK0"; \
	else \
		echo "tnylpo not found; skipping Z80 execution"; \
	fi

test-6502: $(TARGET) tests/fib.ir4 tests/core.ir4 tests/muldiv.ir4 tests/mem.ir4 tests/req.ir4 tests/data.ir4 tests/ext.ir4
	./$(TARGET) -t 6502 --sim65 -o $(BUILD)/fib_sim65.bin    tests/fib.ir4
	./$(TARGET) -t 6502 --sim65 -o $(BUILD)/core_sim65.bin   tests/core.ir4
	./$(TARGET) -t 6502 --sim65 -o $(BUILD)/muldiv_sim65.bin tests/muldiv.ir4
	./$(TARGET) -t 6502 --sim65 -o $(BUILD)/mem_sim65.bin    tests/mem.ir4
	./$(TARGET) -t 6502 --sim65 -o $(BUILD)/req_sim65.bin    tests/req.ir4
	./$(TARGET) -t 6502 --sim65 -o $(BUILD)/data_sim65.bin   tests/data.ir4
	./$(TARGET) -t 6502 --sim65 -o $(BUILD)/ext_sim65.bin    tests/ext.ir4
	@if command -v sim65 >/dev/null 2>&1; then \
		out=$$(sim65 $(BUILD)/fib_sim65.bin | tr -d '\r\n'); \
		echo "sim65 fib_sim65.bin -> $$out"; \
		test "$$out" = "110"; \
		out=$$(sim65 $(BUILD)/core_sim65.bin | tr -d '\r\n'); \
		echo "sim65 core_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
		out=$$(sim65 $(BUILD)/muldiv_sim65.bin | tr -d '\r\n'); \
		echo "sim65 muldiv_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
		out=$$(sim65 $(BUILD)/mem_sim65.bin | tr -d '\r\n'); \
		echo "sim65 mem_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
		out=$$(sim65 $(BUILD)/req_sim65.bin | tr -d '\r\n'); \
		echo "sim65 req_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
		out=$$(sim65 $(BUILD)/data_sim65.bin | tr -d '\r\n'); \
		echo "sim65 data_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
		out=$$(sim65 $(BUILD)/ext_sim65.bin | tr -d '\r\n'); \
		echo "sim65 ext_sim65.bin -> $$out"; \
		test "$$out" = "OK0"; \
	else \
		echo "sim65 not found; skipping 6502 execution"; \
	fi

test-gforth: ir4th.fs tests/fib.ir4 tests/core.ir4 tests/mem.ir4 tests/req.ir4 tests/data.ir4 tests/ext.ir4
	@out=$$(gforth ir4th.fs tests/fib.ir4 -e "main . cr bye" | tr -d ' \r\n'); \
	echo "gforth fib.ir4 -> $$out"; \
	test "$$out" = "110"
	@out=$$(gforth ir4th.fs tests/core.ir4 -e "main . cr bye" | tr -d ' \r\n'); \
	echo "gforth core.ir4 -> $$out"; \
	test "$$out" = "0"
	@out=$$(gforth ir4th.fs tests/mem.ir4 -e "main . cr bye" | tr -d ' \r\n'); \
	echo "gforth mem.ir4 -> $$out"; \
	test "$$out" = "0"
	@out=$$(gforth ir4th.fs tests/req.ir4 -e "main . cr bye" | tr -d ' \r\n'); \
	echo "gforth req.ir4 -> $$out"; \
	test "$$out" = "0"
	@out=$$(gforth ir4th.fs tests/data.ir4 -e "main . cr bye" | tr -d ' \r\n'); \
	echo "gforth data.ir4 -> $$out"; \
	test "$$out" = "0"
	@out=$$(gforth ir4th.fs tests/ext.ir4 -e "main . cr bye" | tr -d ' \r\n'); \
	echo "gforth ext.ir4 -> $$out"; \
	test "$$out" = "OK0"

rt-z80:
	python3 tools/gen_math48.py --input tools/math48.asm --output src/be_z80_math48.h

clean:
	rm -rf $(BUILD)
