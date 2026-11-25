import asyncio
from contextlib import asynccontextmanager

from bleak import BleakClient, BleakScanner
from winrt.windows.devices.bluetooth import BluetoothLEDevice
from winrt.windows.devices.enumeration import (
    DeviceInformation,
    DevicePairingKinds,
    DevicePairingProtectionLevel,
    DevicePairingResultStatus,
)

SERVICE = "4cd118d0-4372-40fe-aee0-0c20a21f0071"
COMMAND = "4cd118d1-4372-40fe-aee0-0c20a21f0071"
STATUS = "4cd118d2-4372-40fe-aee0-0c20a21f0071"


@asynccontextmanager
async def connect_servo(pin, progress):
    progress("正在扫描 TreeServoBench…")
    device = await BleakScanner.find_device_by_filter(
        lambda _, ad: SERVICE in ad.service_uuids, timeout=8
    )
    if device is None:
        raise RuntimeError("没找到 TreeServoBench。确认主控已通电，且手机已断开。")

    address = int(device.address.replace(":", ""), 16)
    instance = await BluetoothLEDevice.from_bluetooth_address_async(address)
    if instance is None:
        raise RuntimeError("Windows 无法打开蓝牙设备。")
    try:
        device_id = instance.device_information.id
        info = await DeviceInformation.create_from_id_async(device_id)
        if not info.pairing.is_paired:
            if not pin:
                raise RuntimeError("首次配对需要输入 ESP32 当前的六位 PIN。")
            progress("正在用 PIN 配对…")
            pairing = info.pairing.custom

            def provide_pin(_, args):
                if args.pairing_kind == DevicePairingKinds.PROVIDE_PIN:
                    args.accept_with_pin(pin)

            token = pairing.add_pairing_requested(provide_pin)
            try:
                result = await asyncio.wait_for(
                    pairing.pair_with_protection_level_async(
                        DevicePairingKinds.PROVIDE_PIN,
                        DevicePairingProtectionLevel.ENCRYPTION_AND_AUTHENTICATION,
                    ),
                    timeout=15,
                )
                if result.status not in (DevicePairingResultStatus.PAIRED, DevicePairingResultStatus.ALREADY_PAIRED):
                    raise RuntimeError(f"配对失败（{result.status.name}）。请核对本次上电的 PIN。")
            finally:
                pairing.remove_pairing_requested(token)

        progress("已配对，正在连接控制服务…")
        async with BleakClient(device, timeout=8, winrt={"use_cached_services": False}) as client:
            if not client.services.get_characteristic(COMMAND) or not client.services.get_characteristic(STATUS):
                raise RuntimeError("蓝牙设备缺少舵机控制或状态接口。")
            try:
                await asyncio.wait_for(client.write_gatt_char(COMMAND, b"STOP", response=True), 2.5)
            except Exception as error:
                raise RuntimeError("蓝牙安全握手失败，未启用点动。请重连。") from error
            yield client
    finally:
        instance.close()
