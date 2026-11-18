CC      = clang
CFLAGS  = -std=c89 -Wall -Wextra -O2 -Isrc

BUILD   = build
TARGET  = $(BUILD)/ir4c
SRCS    = src/ir4c.c src/frontend.c src/codebuf.c src/path.c src/strpool.c src/vec.c src/be_z80.c src/be_z80_cpm.c src/be_6502.c src/be_6502_sim.c src/be_6502_c64.c src/be_arm64.c src/be_arm64_jit.c src/be_rv32.c src/be_rv32_elf.c src/be_bc.c src/be_bc_run.c
OBJS    = $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all test test-err test-z80 test-6502 test-rv32 rt rt-z80 rt-6502 rt-rv32 clean

all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/ir4.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ir4c.o: src/codebuf.h src/frontend.h src/path.h src/bc.h
$(BUILD)/frontend.o: src/codebuf.h src/frontend.h src/path.h src/strpool.h src/vec.h
$(BUILD)/codebuf.o: src/codebuf.h src/strpool.h src/vec.h
$(BUILD)/path.o: src/path.h src/strpool.h src/vec.h
$(BUILD)/strpool.o: src/strpool.h src/vec.h
$(BUILD)/vec.o: src/vec.h
$(BUILD)/be_bc.o: src/bc.h src/codebuf.h src/vec.h
$(BUILD)/be_bc_run.o: src/bc.h src/codebuf.h src/frontend.h
$(BUILD)/be_z80.o: src/codebuf.h src/be_z80.h src/be_z80_math48.h src/vec.h
$(BUILD)/be_z80_cpm.o: src/codebuf.h src/frontend.h src/be_z80.h
$(BUILD)/be_6502.o: src/codebuf.h src/be_6502.h src/be_6502_wozfp.h src/vec.h
$(BUILD)/be_6502_sim.o: src/codebuf.h src/frontend.h src/be_6502.h
$(BUILD)/be_6502_c64.o: src/codebuf.h src/frontend.h src/be_6502.h
$(BUILD)/be_arm64.o: src/codebuf.h src/vec.h
$(BUILD)/be_arm64_jit.o: src/codebuf.h src/frontend.h
$(BUILD)/be_rv32.o: src/codebuf.h src/be_rv32.h src/be_rv32_fp.h src/vec.h
$(BUILD)/be_rv32_elf.o: src/codebuf.h src/frontend.h src/be_rv32.h

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
	./$(TARGET) -t 6502  -o $(BUILD)/float_6502.bin    tests/float.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/float_arm64.bin --run tests/float.ir4
	./$(TARGET) -t z80   -o $(BUILD)/req_z80.bin       tests/req.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/req_6502.bin      tests/req.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/req_arm64.bin   --run tests/req.ir4
	./$(TARGET) -t z80   -o $(BUILD)/data_z80.bin      tests/data.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/data_6502.bin     tests/data.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/data_arm64.bin  --run tests/data.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/ext_arm64.bin   --run tests/ext.ir4
	./$(TARGET) -t z80   -o $(BUILD)/math_z80.bin     tests/math.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/math_6502.bin    tests/math.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/math_arm64.bin   --run tests/math.ir4
	./$(TARGET) -t z80   -o $(BUILD)/fnptr_z80.bin    tests/fnptr.ir4
	./$(TARGET) -t 6502  -o $(BUILD)/fnptr_6502.bin   tests/fnptr.ir4
	./$(TARGET) -t arm64 -o $(BUILD)/fnptr_arm64.bin  --run tests/fnptr.ir4
	$(MAKE) test-err

test-err: $(TARGET)
	@for f in tests/errors/*.ir4; do \
		if ./$(TARGET) -t arm64 -o $(BUILD)/err.bin $$f >/dev/null 2>&1; then \
			echo "FAIL: expected error for $$f"; exit 1; \
		else \
			echo "OK (rejected): $$f"; \
		fi; \
	done

test-z80: $(TARGET) tests/fib.ir4 tests/core.ir4 tests/muldiv.ir4 tests/mem.ir4 tests/float.ir4 tests/req.ir4 tests/data.ir4 tests/ext.ir4
	./$(TARGET) -t z80 --cpm -o build/fib.com    tests/fib.ir4
	./$(TARGET) -t z80 --cpm -o build/core.com   tests/core.ir4
	./$(TARGET) -t z80 --cpm -o build/muldiv.com tests/muldiv.ir4
	./$(TARGET) -t z80 --cpm -o build/mem.com    tests/mem.ir4
	./$(TARGET) -t z80 --cpm -o build/float.com  tests/float.ir4
	./$(TARGET) -t z80 --cpm -o build/req.com    tests/req.ir4
	./$(TARGET) -t z80 --cpm -o build/data.com   tests/data.ir4
	./$(TARGET) -t z80 --cpm -o build/ext.com    tests/ext.ir4
	./$(TARGET) -t z80 --cpm -o build/fnptr.com  tests/fnptr.ir4
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

test-6502: $(TARGET) tests/fib.ir4 tests/core.ir4 tests/muldiv.ir4 tests/mem.ir4 tests/float.ir4 tests/req.ir4 tests/data.ir4 tests/ext.ir4
	./$(TARGET) -t 6502 --sim65 -o $(BUILD)/fib_sim65.bin    tests/fib.ir4
	./$(TARGET) -t 6502 --sim65 -o $(BUILD)/core_sim65.bin   tests/core.ir4
	./$(TARGET) -t 6502 --sim65 -o $(BUILD)/muldiv_sim65.bin tests/muldiv.ir4
	./$(TARGET) -t 6502 --sim65 -o $(BUILD)/mem_sim65.bin    tests/mem.ir4
	./$(TARGET) -t 6502 --sim65 -o $(BUILD)/float_sim65.bin  tests/float.ir4
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
		out=$$(sim65 $(BUILD)/float_sim65.bin | tr -d '\r\n'); \
		echo "sim65 float_sim65.bin -> $$out"; \
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
		./$(TARGET) -t 6502 --sim65 -o $(BUILD)/math_sim65.bin tests/math.ir4; \
		out=$$(sim65 $(BUILD)/math_sim65.bin | tr -d '\r\n'); \
		echo "sim65 math_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
		./$(TARGET) -t 6502 --sim65 -o $(BUILD)/fnptr_sim65.bin tests/fnptr.ir4; \
		out=$$(sim65 $(BUILD)/fnptr_sim65.bin | tr -d '\r\n'); \
		echo "sim65 fnptr_sim65.bin -> $$out"; \
		test "$$out" = "0"; \
	else \
		echo "sim65 not found; skipping 6502 execution"; \
	fi

test-rv32: $(TARGET) tests/fib.ir4 tests/core.ir4 tests/muldiv.ir4 tests/mem.ir4 tests/float.ir4 tests/req.ir4 tests/data.ir4 tests/ext.ir4
	./$(TARGET) -t rv32 --virt -o $(BUILD)/fib_rv32.elf    tests/fib.ir4
	./$(TARGET) -t rv32 --virt -o $(BUILD)/core_rv32.elf   tests/core.ir4
	./$(TARGET) -t rv32 --virt -o $(BUILD)/muldiv_rv32.elf tests/muldiv.ir4
	./$(TARGET) -t rv32 --virt -o $(BUILD)/mem_rv32.elf    tests/mem.ir4
	./$(TARGET) -t rv32 --virt -o $(BUILD)/float_rv32.elf  tests/float.ir4
	./$(TARGET) -t rv32 --virt -o $(BUILD)/req_rv32.elf    tests/req.ir4
	./$(TARGET) -t rv32 --virt -o $(BUILD)/data_rv32.elf   tests/data.ir4
	./$(TARGET) -t rv32 --virt -o $(BUILD)/ext_rv32.elf    tests/ext.ir4
	./$(TARGET) -t rv32 --virt -o $(BUILD)/math_rv32.elf   tests/math.ir4
	./$(TARGET) -t rv32 --virt -o $(BUILD)/fnptr_rv32.elf  tests/fnptr.ir4
	@if command -v qemu-system-riscv32 >/dev/null 2>&1; then \
		for pair in "fib_rv32.elf:110" "core_rv32.elf:0" "muldiv_rv32.elf:0" "mem_rv32.elf:0" "float_rv32.elf:0" "req_rv32.elf:0" "data_rv32.elf:0" "ext_rv32.elf:OK0" "math_rv32.elf:0" "fnptr_rv32.elf:0"; do \
			f=$${pair%%:*}; exp=$${pair#*:}; \
			out=$$(qemu-system-riscv32 -machine virt -cpu rv32 -bios none -nographic -kernel $(BUILD)/$$f | tr -d '\r\n'); \
			echo "qemu-system-riscv32 $$f -> $$out"; \
			test "$$out" = "$$exp"; \
		done; \
	else \
		echo "qemu-system-riscv32 not found; skipping RV32 execution"; \
	fi

rt: rt-z80 rt-6502 rt-rv32

rt-z80:
	python3 tools/gen_math48.py --input tools/math48.asm --output src/be_z80_math48.h
	python3 tools/asm2ir4.py --cpu z80 --input tools/z80/i32.asm --output lib/z80/i32.ir4
	python3 tools/asm2ir4.py --cpu z80 --input tools/z80/mem.asm --output lib/z80/mem.ir4

rt-6502:
	python3 tools/gen_wozfp.py --input tools/wozfp.asm --output src/be_6502_wozfp.h
	python3 tools/asm2ir4.py --cpu 6502 --input tools/6502/i32.asm --output lib/6502/i32.ir4
	python3 tools/asm2ir4.py --cpu 6502 --input tools/6502/mem.asm --output lib/6502/mem.ir4

rt-rv32:
	python3 tools/gen_rv32_fp.py --input tools/rv32_fp.c --output src/be_rv32_fp.h

clean:
	rm -rf $(BUILD)
