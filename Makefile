# 1. 툴체인 및 도구 설정
CC      := riscv64-unknown-elf-gcc
OBJDUMP := riscv64-unknown-elf-objdump
READELF := riscv64-unknown-elf-readelf

# 2. 빌드 디렉터리 정의 (플래그 참조보다 먼저 선언 필수)
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj

# 3. 컴파일러 플래그
CFLAGS  := -march=rv32i_zicsr -mabi=ilp32 -mcmodel=medany \
           -nostdlib -fno-builtin -ffreestanding \
           -mpreferred-stack-boundary=4 \
           -ffunction-sections -fdata-sections \
           -Wall -Wextra -O0 -g \
           -MMD -MP \
           -Ikernel/include -Ibsp -Iapp

# 4. 링커 플래그 ($(BUILD_DIR) 참조 정상화 및 -lgcc 명시)
LDFLAGS := -T linker.ld -Wl,-Map=$(BUILD_DIR)/kernel.map -Wl,--gc-sections -lgcc

# 5. 소스 파일 및 목적 파일 매핑
SRCS_S := $(wildcard boot/*.S) $(wildcard kernel/*.S)
SRCS_C := $(wildcard kernel/*.c) $(wildcard bsp/*.c) $(wildcard app/*.c)

OBJS := $(patsubst %.S, $(OBJ_DIR)/%.o, $(SRCS_S)) \
        $(patsubst %.c, $(OBJ_DIR)/%.o, $(SRCS_C))

# 헤더 의존성 파일 목록 (.d)
DEPS := $(OBJS:.o=.d)

# 기본 타깃: ELF 생성, 디스어셈블리 추출
all: $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/kernel.asm

# 6. 최종 ELF 바이너리 링킹
$(BUILD_DIR)/kernel.elf: $(OBJS) linker.ld
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@
	@echo "=== [LINK SUCCESS] $@ ==="

# 7. 디스어셈블리 덤프 자동 생성
$(BUILD_DIR)/kernel.asm: $(BUILD_DIR)/kernel.elf
	$(OBJDUMP) -d -S $< > $@
	@echo "=== [DUMP SUCCESS] $@ ==="

# 8. C 소스 파일 컴파일 룰 (폴더 자동 생성)
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# 9. 어셈블리 소스 파일 컴파일 룰 (폴더 자동 생성)
$(OBJ_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# 10. QEMU 실행
run: $(BUILD_DIR)/kernel.elf
	qemu-system-riscv32 -M virt -bios none -nographic -serial mon:stdio -kernel $<

# 11. 산출물 완전 청소
clean:
	rm -rf $(BUILD_DIR)
	@echo "=== [CLEAN SUCCESS] build directory removed ==="

# 12. 정형 검증 (CBMC) 타깃
cbmc:
	@chmod +x verification/cbmc/run_cbmc.sh
	@./verification/cbmc/run_cbmc.sh

# 13. 양방향 추적성 및 Dead Code 검사 타깃
trace:
	@python3 verification/traceability/check_traceability.py

# 생성된 헤더 의존성 파일 포함 (헤더 수정 시 자동 재컴파일)
-include $(DEPS)

.PHONY: all run clean cbmc trace