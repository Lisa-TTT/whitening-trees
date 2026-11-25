import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description='构建 STM32F407 舵机 RAM 固件')
    parser.add_argument('--pio-core', type=Path,
                        default=Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio')))
    args = parser.parse_args()
    packages = args.pio_core / 'packages'
    suffix = '.exe' if os.name == 'nt' else ''
    tools = packages / 'toolchain-gccarmnoneeabi' / 'bin'
    gcc = tools / f'arm-none-eabi-gcc{suffix}'
    objcopy = tools / f'arm-none-eabi-objcopy{suffix}'
    core = packages / 'framework-cmsis' / 'CMSIS' / 'Core' / 'Include'
    device = packages / 'framework-cmsis-stm32f4' / 'Include'
    stm32 = ROOT / 'firmware' / 'stm32'
    common = ROOT / 'firmware' / 'common'
    for required in (gcc, objcopy, core / 'core_cm4.h', device / 'stm32f4xx.h'):
        if not required.exists():
            parser.error(f'缺少 {required}；先运行 pio pkg install -e stm32_tools')
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    elf = build / 'stm32-jog-ram.elf'
    target = build / 'stm32-jog-ram.bin'
    subprocess.run([gcc, '-mthumb', '-mcpu=cortex-m4', '-DSTM32F407xx',
                    '-I', core, '-I', device, '-I', common, '-Os', '-ffreestanding',
                    '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
                    '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra', '-Werror',
                    '-Wl,--fatal-warnings,--gc-sections', '-nostdlib', '-T', stm32 / 'servo_jog_ram.ld',
                    stm32 / 'servo_jog_ram.c', common / 'servo_frames.c', '-o', elf], check=True)
    subprocess.run([objcopy, '-O', 'binary', elf, target], check=True)
    print(target)


if __name__ == '__main__':
    main()\n