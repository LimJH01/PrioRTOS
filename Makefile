# 1. 툴체인 및 도구 설정
CC      := riscv64-unknown-elf-gcc
OBJDUMP := riscv64-unknown-elf-objdump
READELF := riscv64-unknown-elf-readelf

# 2. 컴파일러 플래그
# -MMD -MP : 헤더 파일 변경 감지용 의존성(.d) 파일 생성
# -mpreferred-stack-boundary=4 : 16바이트 스택 정렬 강제
CFLAGS  := -march=rv32i_zicsr -mabi=ilp32 -mcmodel=medany \
           -nostdlib -fno-builtin -ffreestanding \
           -mpreferred-stack-boundary=4 \
           -ffunction-sections -fdata-sections \
           -Wall -Wextra -O0 -g \
           -MMD -MP \
           -Ikernel/include -Ibsp -Iapp

# 3. 링커 플래그 (-lgcc 추가 필수, 미사용 섹션 제거)
LDFLAGS := -T linker.ld -Wl,-Map=$(BUILD_DIR)/kernel.map -Wl,--gc-sections -lgcc

# 4. 빌드 디렉터리 정의
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj

# 5. 소스 파일 및 목적 파일 매핑
SRCS_S := $(wildcard boot/*.S) $(wildcard kernel/*.S)
SRCS_C := $(wildcard kernel/*.c) $(wildcard bsp/*.c) $(wildcard app/*.c)

OBJS := $(patsubst %.S, $(OBJ_DIR)/%.o, $(SRCS_S)) \
        $(patsubst %.c, $(OBJ_DIR)/%.o, $(SRCS_C))

# 헤더 의존성 파일 목록 (.d)
DEPS := $(OBJS:.o=.d)

# 기본 타깃
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

# 8. C 소스 파일 컴파일 룰
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# 9. 어셈블리 소스 파일 컴파일 룰
$(OBJ_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# 10. QEMU 실행
run: $(BUILD_DIR)/kernel.elf
	qemu-system-riscv32 -M virt -bios none -nographic -serial mon:stdio -kernel $<

# 11. 산출물 정리
clean:
	rm -rf $(BUILD_DIR)
	@echo "=== [CLEAN SUCCESS] build directory removed ==="

# 12. 정형 검증 및 분석
cbmc:
	@chmod +x verification/cbmc/run_cbmc.sh
	@./verification/cbmc/run_cbmc.sh

trace:
	@python3 verification/traceability/check_traceability.py

# 생성된 헤더 의존성 파일 포함 (헤더 수정 시 자동 재컴파일 유도)
-include $(DEPS)

.PHONY: all run clean cbmc trace