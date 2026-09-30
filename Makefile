# 1. 툴체인 및 플래그 설정
CC      := riscv64-unknown-elf-gcc
OBJDUMP := riscv64-unknown-elf-objdump
READELF := riscv64-unknown-elf-readelf

CFLAGS  := -march=rv32i_zicsr -mabi=ilp32 -mcmodel=medany -nostdlib -fno-builtin \
           -Wall -Wextra -O0 -g \
           -Ikernel/include -Ibsp -Iapp

LDFLAGS := -T linker.ld -Wl,-Map=build/kernel.map

# 2. 빌드 디렉터리 정의
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj

# 3. 소스 파일 수집 (boot, kernel, bsp, app)
SRCS_S := $(wildcard boot/*.S) $(wildcard kernel/*.S)
SRCS_C := $(wildcard kernel/*.c) $(wildcard bsp/*.c) $(wildcard app/*.c)

# 4. 목적 파일(.o) 경로 변환: build/obj/하위폴더/파일명.o 형태로 매핑
OBJS := $(patsubst %.S, $(OBJ_DIR)/%.o, $(SRCS_S)) \
        $(patsubst %.c, $(OBJ_DIR)/%.o, $(SRCS_C))

# 기본 타깃: ELF 생성, 디스어셈블리 추출
all: $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/kernel.asm

# 5. 최종 ELF 바이너리 링킹
$(BUILD_DIR)/kernel.elf: $(OBJS) linker.ld
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@
	@echo "=== [LINK SUCCESS] $@ ==="

# 6. 디스어셈블리 덤프 자동 생성
$(BUILD_DIR)/kernel.asm: $(BUILD_DIR)/kernel.elf
	$(OBJDUMP) -d -S $< > $@
	@echo "=== [DUMP SUCCESS] $@ ==="

# 7. C 소스 파일 컴파일 룰 (폴더 자동 생성)
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# 8. 어셈블리 소스 파일 컴파일 룰 (폴더 자동 생성)
$(OBJ_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# 9. QEMU 실행
run: $(BUILD_DIR)/kernel.elf
	qemu-system-riscv32 -M virt -bios none -nographic -serial mon:stdio -kernel $<

# 10. 산출물 완전 청소
clean:
	rm -rf $(BUILD_DIR)
	@echo "=== [CLEAN SUCCESS] build directory removed ==="

# 11. 정형 검증 (CBMC) 타깃
cbmc:
	@chmod +x verification/cbmc/run_cbmc.sh
	@./verification/cbmc/run_cbmc.sh

# 12. 양방향 추적성 및 Dead Code 검사 타깃
trace:
	@python3 verification/traceability/check_traceability.py

.PHONY: all run clean cbmc trace