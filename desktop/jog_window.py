import asyncio
from queue import Empty, SimpleQueue
from threading import Event, Thread
from time import monotonic
import tkinter as tk
from tkinter import ttk

from jog_bluetooth import COMMAND, STATUS, connect_servo
HEARTBEAT_SECONDS = 0.08
READ_TIMEOUT_SECONDS = 2.5
WRITE_TIMEOUT_SECONDS = 1.2


async def serve(pin, inputs, updates, ending):
    def progress(text):
        updates.put(('notice', text))

    async with connect_servo(pin, progress) as client:
        updates.put(('connected', None))
        loop = asyncio.get_running_loop()
        motion = sent = None
        next_heartbeat = next_poll = last_ui_event = 0.0
        previous = ''
        status_read = None
        read_failures = 0
        try:
            while client.is_connected and not ending.is_set():
                while True:
                    try:
                        motion, last_ui_event = inputs.get_nowait()
                    except Empty:
                        break
                now = loop.time()
                if motion is not None and monotonic() - last_ui_event >= .20:
                    motion = None
                if motion != sent or (motion is not None and now >= next_heartbeat):
                    if status_read is not None and not status_read.done():
                        status_read.cancel()
                        await asyncio.gather(status_read, return_exceptions=True)
                        status_read = None
                    if motion is not None and monotonic() - last_ui_event >= .20:
                        motion = None
                    try:
                        await asyncio.wait_for(client.write_gatt_char(
                            COMMAND, motion or b'STOP', response=True), WRITE_TIMEOUT_SECONDS)
                    except TimeoutError as exc:
                        raise RuntimeError('蓝牙写入超时，点动已停止。请重新连接。') from exc
                    sent = motion
                    next_heartbeat = now + HEARTBEAT_SECONDS
                    next_poll = loop.time() + .35
                    if motion is None:
                        previous = ''
                if status_read is not None and status_read.done():
                    try:
                        status = bytes(status_read.result()).decode('ascii')
                    except TimeoutError as exc:
                        read_failures += 1
                        if read_failures >= 3:
                            raise RuntimeError('连续三次未读到 STM32 状态，请重新连接。') from exc
                    else:
                        read_failures = 0
                        if status != previous:
                            updates.put(('state', status))
                            previous = status
                    status_read = None
                if sent is None and status_read is None and now >= next_poll:
                    status_read = asyncio.create_task(asyncio.wait_for(client.read_gatt_char(STATUS), READ_TIMEOUT_SECONDS))
                    next_poll = now + .65
                await asyncio.sleep(.02)
        finally:
            if status_read is not None:
                status_read.cancel()
                await asyncio.gather(status_read, return_exceptions=True)
            if client.is_connected:
                try:
                    await asyncio.wait_for(client.write_gatt_char(COMMAND, b'STOP', response=True), WRITE_TIMEOUT_SECONDS)
                except Exception:
                    pass  # The STM32 also stops when the command lease expires.


class JogWindow:
    def __init__(self, root):
        self.root = root
        self.updates = SimpleQueue()
        self.pin = tk.StringVar()
        self.status = tk.StringVar(value='未连接')
        self.active = None
        self.ready = False
        self.worker = None
        self.closing = False
        root.title('ZP10S 按住点动 · 连接修正版')
        root.resizable(False, False)
        frame = ttk.Frame(root, padding=22)
        frame.pack(fill='both', expand=True)
        ttk.Label(frame, text='ZP10S 按住点动', font=('Microsoft YaHei UI', 15, 'bold')).pack(anchor='w', pady=(0, 12))
        login = ttk.Frame(frame)
        login.pack(fill='x')
        ttk.Label(login, text='首次配对 PIN').pack(side='left')
        self.pin_entry = ttk.Entry(login, textvariable=self.pin, width=9, show='•')
        self.pin_entry.pack(side='left', padx=8)
        self.pin_entry.bind('<Return>', lambda _: self.connect())
        self.connect_button = ttk.Button(login, text='连接蓝牙', command=self.connect)
        self.connect_button.pack(side='left', padx=(0, 6))
        self.disconnect_button = ttk.Button(login, text='断开', command=self.disconnect, state='disabled')
        self.disconnect_button.pack(side='left')
        ttk.Label(frame, textvariable=self.status, wraplength=460).pack(anchor='w', pady=(10, 4))
        controls = ttk.Frame(frame)
        controls.pack(fill='x', pady=(12, 8))
        self.direction_buttons = []
        for title, signal in (('按住逆时针', b'CCW'), ('按住顺时针', b'CW')):
            button = ttk.Button(controls, text=title, state='disabled')
            button.pack(side='left', padx=(0, 8), ipadx=8, ipady=5)
            button.bind('<ButtonPress-1>', lambda event, direction=signal: self.press(direction))
            button.bind('<ButtonRelease-1>', lambda event: self.release())
            button.bind('<Leave>', lambda event: self.release())
            self.direction_buttons.append(button)
        self.stop_button = ttk.Button(controls, text='停止', command=self.release, state='disabled')
        self.stop_button.pack(side='left', ipadx=8, ipady=5)
        ttk.Label(frame, text='松开鼠标或移出按钮就停。仅限舵机与夹爪脱开时使用。').pack(anchor='w')
        root.bind_all('<ButtonRelease-1>', lambda event: self.release())
        root.bind('<Escape>', lambda event: self.release())
        root.bind('<Unmap>', lambda event: self.release())
        root.bind('<FocusOut>', lambda event: root.after_idle(self.check_focus))
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(30, self.poll)

    def connect(self):
        if self.worker is not None:
            return
        pin = self.pin.get().strip()
        if pin and (len(pin) != 6 or not pin.isascii() or not pin.isdecimal()):
            self.status.set('请输入六位 PIN。')
            return
        self.events = SimpleQueue()
        self.ending = Event()
        self.connect_button.state(['disabled'])
        self.pin_entry.state(['disabled'])
        self.disconnect_button.state(['!disabled'])
        self.status.set('正在连接…')

        def work():
            try:
                asyncio.run(serve(pin, self.events, self.updates, self.ending))
            except Exception as err:
                self.updates.put(('notice', str(err) or '蓝牙连接超时'))
            finally:
                self.updates.put(('finished', None))

        self.worker = Thread(target=work, daemon=True)
        self.worker.start()

    def press(self, direction):
        if self.ready and not self.ending.is_set() and direction != self.active:
            self.active = direction
            self.events.put((direction, monotonic()))
            self.status.set("正在发出顺时针指令" if direction == b"CW" else "正在发出逆时针指令")
        return 'break'

    def release(self):
        if self.active is not None:
            self.active = None
            self.events.put((None, monotonic()))
            self.status.set("已松开，停止续发")

    def check_focus(self):
        if self.root.focus_displayof() is None:
            self.release()

    def disconnect(self):
        if self.worker is not None:
            self.release()
            self.ending.set()
            self.ready = False
            for button in self.direction_buttons:
                button.state(['disabled'])

    def close(self):
        self.closing = True
        self.disconnect()

    def poll(self):
        if self.active is not None:
            self.events.put((self.active, monotonic()))
        while not self.updates.empty():
            kind, value = self.updates.get_nowait()
            if kind == 'notice':
                self.status.set(value)
            elif kind == 'connected':
                self.stop_button.state(['!disabled'])
            elif kind == 'state':
                self.ready = value in ('READY', '1380', '1620') and not self.ending.is_set()
                if not self.ready:
                    self.release()
                for button in self.direction_buttons:
                    button.state(['!disabled' if self.ready else 'disabled'])
                self.status.set({'READY': '已连接，等待按住按钮', '1380': '顺时针指令保持中',
                                 '1620': '逆时针指令保持中', 'STOP INPUT': 'STM32 停车输入已触发'}.get(value, '等待 STM32'))
            elif kind == 'finished':
                self.release()
                self.worker = None
                self.ready = False
                self.connect_button.state(['!disabled'])
                self.pin_entry.state(['!disabled'])
                for button in [*self.direction_buttons, self.stop_button, self.disconnect_button]:
                    button.state(['disabled'])
                if self.ending.is_set():
                    self.status.set('已断开')
        if self.closing and self.worker is None:
            self.root.destroy()
        else:
            self.root.after(30, self.poll)


if __name__ == '__main__':
    window = tk.Tk()
    JogWindow(window)
    window.mainloop()\n