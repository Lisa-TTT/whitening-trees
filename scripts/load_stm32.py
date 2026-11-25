import argparse
from functools import reduce
from pathlib import Path
import operator
import struct
import time

import serial

root = Path(__file__).resolve().parents[1]

SRAM = 0x20008000
POSITION = 0x20008F00
STAMP = 0x5A503130


def checksum(data):
    return bytes([reduce(operator.xor, data, 0)])


def exact(port, count):
    data = port.read(count)
    if len(data) != count:
        raise TimeoutError('STM32 串口回报超时')
    return data


def ack(port):
    if exact(port, 1) != b'\x79':
        raise RuntimeError('STM32 未接受引导指令')


def command(port, code):
    port.write(bytes([code, 255 ^ code]))
    ack(port)


def address(port, value):
    packet = struct.pack('>I', value)
    port.write(packet + checksum(packet))
    ack(port)


def enter_rom(port):
    port.rts = True
    port.dtr = False
    time.sleep(.13)
    port.dtr = True
    time.sleep(.15)
    port.reset_input_buffer()
    port.write(b'\x7f')
    ack(port)
    command(port, 2)
    ident = int.from_bytes(exact(port, exact(port, 1)[0] + 1), 'big')
    ack(port)
    if ident != 0x413:
        raise RuntimeError('串口连接的芯片不是预期的 STM32F407')


def read_memory(port, addr, length):
    command(port, 0x11)
    address(port, addr)
    size = length - 1
    port.write(bytes([size, size ^ 255]))
    ack(port)
    return exact(port, length)


def write_block(port, dest, block):
    if not SRAM <= dest <= dest + len(block) <= SRAM + 4096:
        raise ValueError('RAM 地址不在预留范围内')
    command(port, 0x31)
    address(port, dest)
    packet = bytes([len(block) - 1]) + block
    port.write(packet + checksum(packet))
    ack(port)
    if read_memory(port, dest, len(block)) != block:
        raise RuntimeError('临时程序回读不一致')


def load(port_name, image_path, start_position):
    data = image_path.read_bytes()
    stack, reset = struct.unpack_from('<II', data)
    if stack != 0x20020000 or not reset & 1 or not SRAM <= reset < SRAM + len(data) <= POSITION:
        raise ValueError('临时程序地址无效')
    data += b'\0' * (-len(data) % 4)
    with serial.Serial(port_name, 115200, parity=serial.PARITY_EVEN, timeout=2, write_timeout=2) as port:
        enter_rom(port)
        if start_position is None and read_memory(port, SRAM, 256) == data[:256]:
            stamp, retained = struct.unpack('<II', read_memory(port, POSITION, 8))
            if stamp == STAMP and 600 <= retained <= 2400:
                start_position = retained
        position = 1500 if start_position is None else start_position
        for offset in range(0, len(data), 256):
            write_block(port, SRAM + offset, data[offset:offset + 256])
        write_block(port, POSITION, struct.pack('<II', STAMP, position))
        port.rts = False
        command(port, 0x21)
        address(port, SRAM)
    print(f'STM32 RAM 程序已启动。估计起点 {position}，未写 Flash。')


def main():
    parser = argparse.ArgumentParser(description='将 ZP10S 点动程序加载到 STM32F407 SRAM')
    parser.add_argument('--port', required=True, help='STM32 的 USB 串口，例如 COM4')
    parser.add_argument('--image', type=Path, default=root / 'images' / 'stm32-jog-ram.bin')
    parser.add_argument('--start-position', type=int, choices=range(600, 2401), metavar='600–2400',
                        help='已知的当前指令位置。重新上电回中后通常为 1500。')
    args = parser.parse_args()
    load(args.port, args.image, args.start_position)


if __name__ == '__main__':
    main()\n