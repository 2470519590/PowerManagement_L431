#!/usr/bin/env python3
"""Bluetooth serial monitor and recorder for the power-management module."""

from __future__ import annotations

import argparse
import queue
import threading
import time
import tkinter as tk
from datetime import datetime
from pathlib import Path
from tkinter import messagebox, ttk

import serial
from serial.tools import list_ports


BAUDRATE = 115200
STATUS_TIMEOUT_SECONDS = 3.0
LOG_DIRECTORY = Path(__file__).resolve().parent / "log"


def parse_status(line: str) -> dict[str, str] | None:
    fields = line.strip().split(",")
    if not fields or fields[0] != "STA":
        return None
    result: dict[str, str] = {}
    for field in fields[1:]:
        key, separator, value = field.partition("=")
        if not separator or not key or not value:
            return None
        result[key] = value
    required = {"PAVG", "IAVG", "BUF", "CUT", "OCP", "CHS"}
    return result if required.issubset(result) else None


class BluetoothPowerMonitor:
    def __init__(self, root: tk.Tk, initial_port: str | None = None) -> None:
        self.root = root
        self.initial_port = initial_port
        self.serial_port: serial.Serial | None = None
        self.reader_thread: threading.Thread | None = None
        self.reader_stop: threading.Event | None = None
        self.messages: queue.Queue[tuple[str, object]] = queue.Queue()
        self.running = False
        self.closed = False
        self.received_status = False
        self.last_status_time = 0.0
        self.previous_cut = False
        self.previous_ocp = False
        self.log_stream = None
        self.log_path: Path | None = None

        root.title("电管蓝牙串口监视器")
        root.resizable(False, False)
        self.values = {
            "current": tk.StringVar(value="-- mA"),
            "power": tk.StringVar(value="-- W"),
            "buffer": tk.StringVar(value="-- J"),
            "limit": tk.StringVar(value="-- W"),
            "switch": tk.StringVar(value="--"),
            "protection": tk.StringVar(value="请选择串口并开始"),
        }
        self.port_value = tk.StringVar(value=initial_port or "")
        self.connection_value = tk.StringVar(value="未开始")
        self._build_window()
        self._refresh_ports()
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(50, self._poll_messages)

    def _build_window(self) -> None:
        frame = tk.Frame(self.root, padx=18, pady=14)
        frame.grid()
        controls = tk.Frame(frame)
        controls.grid(row=0, column=0, columnspan=2, sticky="w", pady=(0, 10))
        tk.Label(controls, text="串口").pack(side="left")
        self.port_combo = ttk.Combobox(controls, textvariable=self.port_value, width=12, state="readonly")
        self.port_combo.pack(side="left", padx=(6, 4))
        self.refresh_button = tk.Button(controls, text="刷新", command=self._refresh_ports)
        self.refresh_button.pack(side="left", padx=2)
        self.start_button = tk.Button(controls, text="开始", width=8, command=self._start)
        self.start_button.pack(side="left", padx=2)
        self.stop_button = tk.Button(controls, text="停止", width=8, command=self._stop)
        self.stop_button.pack(side="left", padx=2)
        self.stop_button.configure(state="disabled")

        rows = (("100 ms 平均电流", "current"), ("100 ms 平均功率", "power"),
                ("缓冲能量剩余", "buffer"), ("当前功率上限", "limit"),
                ("底盘开关", "switch"), ("保护状态", "protection"))
        for row, (label, key) in enumerate(rows, start=1):
            tk.Label(frame, text=label, anchor="w", width=16).grid(row=row, column=0, sticky="w", pady=3)
            tk.Label(frame, textvariable=self.values[key], anchor="w", width=34).grid(row=row, column=1, sticky="w", pady=3)
        tk.Label(frame, textvariable=self.connection_value, fg="gray", anchor="w").grid(
            row=len(rows) + 1, column=0, columnspan=2, sticky="w", pady=(10, 0))

    def _refresh_ports(self) -> None:
        ports = sorted(port.device for port in list_ports.comports())
        self.port_combo["values"] = ports
        if self.port_value.get() not in ports:
            self.port_value.set(ports[0] if ports else "")

    def _make_log(self) -> None:
        LOG_DIRECTORY.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d%H%M%S")
        path = LOG_DIRECTORY / f"realtime_log_{stamp}.txt"
        suffix = 1
        while path.exists():
            path = LOG_DIRECTORY / f"realtime_log_{stamp}_{suffix}.txt"
            suffix += 1
        self.log_path = path
        self.log_stream = path.open("w", encoding="utf-8", newline="")

    def _write_log(self, text: str) -> None:
        if self.log_stream is not None:
            self.log_stream.write(text.rstrip("\r\n") + "\n")
            self.log_stream.flush()

    def _start(self) -> None:
        port = self.port_value.get().strip()
        if not port:
            messagebox.showwarning("未选择串口", "请先选择蓝牙串口。", parent=self.root)
            return
        try:
            serial_port = serial.Serial(port, BAUDRATE, timeout=0.2)
            self._make_log()
            self.serial_port = serial_port
            self.reader_stop = threading.Event()
            self.running = True
            self.received_status = False
            self.last_status_time = time.monotonic()
            self.previous_cut = False
            self.previous_ocp = False
            self._write_log(f"连接中：{port}")
            self._write_log("已连接")
            self.connection_value.set(f"记录中：{self.log_path}")
            self.values["protection"].set("正在订阅 10 Hz 状态帧")
            self.start_button.configure(state="disabled")
            self.stop_button.configure(state="normal")
            self.refresh_button.configure(state="disabled")
            self.port_combo.configure(state="disabled")
            self.reader_thread = threading.Thread(target=self._reader, args=(serial_port, self.reader_stop), daemon=True)
            self.reader_thread.start()
            self.root.after(500, self._subscribe_status)
        except (OSError, serial.SerialException) as error:
            self._close_log()
            self.values["protection"].set(f"串口打开失败：{error}")
            messagebox.showerror("无法打开串口", str(error), parent=self.root)

    def _reader(self, serial_port: serial.Serial, stop_event: threading.Event) -> None:
        try:
            while not stop_event.is_set():
                line = serial_port.readline()
                if line:
                    self.messages.put(("line", line.decode("ascii", errors="replace")))
        except (OSError, serial.SerialException) as error:
            if not stop_event.is_set():
                self.messages.put(("error", str(error)))

    def _poll_messages(self) -> None:
        while True:
            try:
                message_type, content = self.messages.get_nowait()
            except queue.Empty:
                break
            if message_type == "line":
                line = str(content)
                self._write_log(line)
                status = parse_status(line)
                if status is not None and self.running:
                    self._update_status(status)
            elif self.running:
                self._stop(auto=True, reason=f"串口异常：{content}")
                messagebox.showerror("蓝牙串口异常", str(content), parent=self.root)

        if self.running and time.monotonic() - self.last_status_time > STATUS_TIMEOUT_SECONDS:
            self._stop(auto=True, reason=f"超过 {STATUS_TIMEOUT_SECONDS:g} s 未收到有效 STA 状态帧")
            messagebox.showwarning("串口数据超时", f"超过 {STATUS_TIMEOUT_SECONDS:g} 秒没有收到有效的 10 Hz 状态帧，已自动停止记录。", parent=self.root)
        if not self.closed:
            self.root.after(50, self._poll_messages)

    def _subscribe_status(self) -> None:
        if not self.running or self.received_status:
            return
        self._send_command("STA=ON")
        self.root.after(1000, self._subscribe_status)

    def _send_command(self, command: str) -> None:
        if self.serial_port is None:
            return
        try:
            self.serial_port.write((command + "\r\n").encode("ascii"))
            self._write_log(command)
        except (OSError, serial.SerialException) as error:
            self.messages.put(("error", str(error)))

    def _update_status(self, status: dict[str, str]) -> None:
        self.received_status = True
        self.last_status_time = time.monotonic()
        self.values["current"].set(f"{status['IAVG']} mA")
        self.values["power"].set(f"{status['PAVG']} W")
        self.values["buffer"].set(f"{status['BUF']} J")
        self.values["limit"].set(f"{status.get('LIM', '--')} W")
        self.values["switch"].set(status["CHS"])
        protection = []
        if status["CUT"] == "1":
            protection.append("功率超限惩罚：底盘断电 5 s")
        if status["OCP"] == "1":
            protection.append("过流/堵转保护：已锁存断电")
        self.values["protection"].set("；".join(protection) if protection else "正常")
        cut_active, ocp_active = status["CUT"] == "1", status["OCP"] == "1"
        if cut_active and not self.previous_cut:
            self._warn("功率超限惩罚", "缓冲能量已耗尽且功率仍超限，底盘已断电 5 秒。")
        if ocp_active and not self.previous_ocp:
            self._warn("过流/堵转保护", "电流已连续超过阈值 200 ms，底盘已锁存断电。")
        self.previous_cut, self.previous_ocp = cut_active, ocp_active

    def _warn(self, title: str, detail: str) -> None:
        self.root.bell()
        messagebox.showwarning(title, detail, parent=self.root)

    def _close_log(self) -> None:
        if self.log_stream is not None:
            self.log_stream.close()
            self.log_stream = None

    def _stop(self, auto: bool = False, reason: str = "") -> None:
        if not self.running:
            return
        self.running = False
        if reason:
            self._write_log(reason)
        if self.serial_port is not None:
            try:
                self._send_command("STA=OFF")
                self.serial_port.close()
            except (OSError, serial.SerialException):
                pass
        if self.reader_stop is not None:
            self.reader_stop.set()
        self.serial_port = None
        self._close_log()
        self.start_button.configure(state="normal")
        self.stop_button.configure(state="disabled")
        self.refresh_button.configure(state="normal")
        self.port_combo.configure(state="readonly")
        self.connection_value.set(f"已停止，日志：{self.log_path}")
        if auto:
            self.values["protection"].set(reason)

    def close(self) -> None:
        self.closed = True
        self._stop()
        self.root.destroy()


def main() -> None:
    parser = argparse.ArgumentParser(description="Bluetooth serial power monitor")
    parser.add_argument("--port", help="initially selected port, for example COM7")
    parser.add_argument("--list", action="store_true", help="List serial ports and exit")
    args = parser.parse_args()
    if args.list:
        for port in list_ports.comports():
            description = port.description.encode("ascii", errors="replace").decode("ascii")
            print(f"{port.device}: {description}")
        return
    root = tk.Tk()
    BluetoothPowerMonitor(root, args.port)
    root.mainloop()


if __name__ == "__main__":
    main()
