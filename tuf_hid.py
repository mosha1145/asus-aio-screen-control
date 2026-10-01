#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tuf_hid.py — TUF 水冷屏 HID 通信封装
=====================================
协议来源: PROTOCOL_REVERSE_REPORT.md（实测验证）

- VID 0x0B05 / PID 0x1C7B，双 HID 接口
  - 控制接口 OutputReportByteLength=441（wire 440B）
  - 图像接口 OutputReportByteLength=1025（wire 1024B）
- WriteFile 必须写 caps 长度（首字节 0x00 报告ID）
- 唤醒/亮度: 12 01 00 80 <V>  (V=0x00~0x64)
- 屏幕开关: 10 01 00 80 <0x00开/0x01关>
- 图片块:   [08][首块=总块数/后续=序号][00][80首块/00后续] + 1020B JPEG
"""
import ctypes
import ctypes.wintypes as wt
import time

VID, PID = 0x0B05, 0x1C7B

GENERIC_READ  = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_RW = 3
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class GUID(ctypes.Structure):
    _fields_ = [('Data1', wt.DWORD), ('Data2', wt.WORD),
                ('Data3', wt.WORD), ('Data4', ctypes.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [('cbSize', wt.DWORD), ('InterfaceClassGuid', GUID),
                ('Flags', wt.DWORD), ('Reserved', ctypes.POINTER(wt.ULONG))]


class SP_DEVICE_INTERFACE_DETAIL_DATA_W(ctypes.Structure):
    _fields_ = [('cbSize', wt.DWORD), ('DevicePath', ctypes.c_wchar * 260)]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [
        ('Usage', wt.USHORT), ('UsagePage', wt.USHORT),
        ('InputReportByteLength', wt.USHORT),
        ('OutputReportByteLength', wt.USHORT),
        ('FeatureReportByteLength', wt.USHORT),
        ('Reserved', wt.USHORT * 17),
        ('NumberLinkCollectionNodes', wt.USHORT),
        ('NumberInputButtonCaps', wt.USHORT),
        ('NumberInputValueCaps', wt.USHORT),
        ('NumberInputDataIndices', wt.USHORT),
        ('NumberOutputButtonCaps', wt.USHORT),
        ('NumberOutputValueCaps', wt.USHORT),
        ('NumberOutputDataIndices', wt.USHORT),
        ('NumberFeatureButtonCaps', wt.USHORT),
        ('NumberFeatureValueCaps', wt.USHORT),
        ('NumberFeatureDataIndices', wt.USHORT),
    ]

DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10

kernel32 = ctypes.windll.kernel32
setupapi = ctypes.windll.setupapi
hidlib   = ctypes.windll.hid

kernel32.CreateFileW.restype = ctypes.c_void_p
kernel32.WriteFile.restype = wt.BOOL
kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p
setupapi.SetupDiDestroyDeviceInfoList.restype = wt.BOOL
setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), ctypes.c_wchar_p, ctypes.c_void_p, wt.DWORD]
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(GUID), wt.DWORD, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [ctypes.c_void_p, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA), ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [ctypes.c_void_p]
kernel32.CreateFileW.argtypes = [ctypes.c_wchar_p, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, ctypes.c_void_p]
kernel32.WriteFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wt.DWORD,
                               ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
kernel32.DeviceIoControl.restype = wt.BOOL
hidlib.HidD_GetPreparsedData.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
hidlib.HidD_GetPreparsedData.restype = wt.BOOL
hidlib.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
hidlib.HidP_GetCaps.restype = wt.LONG
hidlib.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]


def find_hid_interfaces(vid=VID, pid=PID):
    """枚举 HID 设备, 返回匹配 VID/PID 的 (设备路径, 输出报告长度) 列表"""
    guid = GUID()
    hidlib.HidD_GetHidGuid(ctypes.byref(guid))
    devs = setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if devs == INVALID_HANDLE_VALUE:
        return []
    results = []
    i = 0
    while True:
        iface = SP_DEVICE_INTERFACE_DATA()
        iface.cbSize = ctypes.sizeof(iface)
        if not setupapi.SetupDiEnumDeviceInterfaces(devs, None, ctypes.byref(guid), i, ctypes.byref(iface)):
            break
        sz = wt.DWORD(0)
        setupapi.SetupDiGetDeviceInterfaceDetailW(devs, ctypes.byref(iface), None, 0, ctypes.byref(sz), None)
        if not sz.value:
            i += 1
            continue
        detail = SP_DEVICE_INTERFACE_DETAIL_DATA_W()
        detail.cbSize = 8  # x64
        if setupapi.SetupDiGetDeviceInterfaceDetailW(devs, ctypes.byref(iface), ctypes.byref(detail),
                                                     sz.value, None, None):
            path = ctypes.cast(ctypes.addressof(detail) + 4, ctypes.c_wchar_p).value
            if f'vid_{vid:04x}' in path.lower() and f'pid_{pid:04x}' in path.lower():
                results.append(path)
        i += 1
    setupapi.SetupDiDestroyDeviceInfoList(devs)
    return results


def get_output_report_len(path):
    """HidP_GetCaps -> OutputReportByteLength"""
    h = kernel32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_RW,
                             None, OPEN_EXISTING, 0, None)
    if h in (INVALID_HANDLE_VALUE, None):
        return None
    try:
        pd = ctypes.c_void_p()
        if not hidlib.HidD_GetPreparsedData(ctypes.c_void_p(h), ctypes.byref(pd)):
            return None
        try:
            caps = HIDP_CAPS()
            if hidlib.HidP_GetCaps(pd, ctypes.byref(caps)) != 0x00110000:
                return None
            return caps.OutputReportByteLength
        finally:
            hidlib.HidD_FreePreparsedData(pd)
    finally:
        kernel32.CloseHandle(ctypes.c_void_p(h))


class TufCooler:
    """TUF 水冷屏 HID 客户端"""

    def __init__(self):
        self.ctrl_h = None
        self.img_h = None
        self.ctrl_olen = 0
        self.img_olen = 0

    def open(self):
        paths = find_hid_interfaces()
        if not paths:
            raise RuntimeError(f'未找到 VID_{VID:04X}&PID_{PID:04X} HID 设备, 请确认水冷已连接且驱动正常')
        for p in paths:
            olen = get_output_report_len(p)
            if olen is None:
                continue
            h = kernel32.CreateFileW(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_RW,
                                     None, OPEN_EXISTING, 0, None)
            if h in (INVALID_HANDLE_VALUE, None):
                continue
            if olen in (440, 441):
                self.ctrl_h, self.ctrl_olen = h, olen
            elif olen in (1024, 1025):
                self.img_h, self.img_olen = h, olen
            else:
                kernel32.CloseHandle(ctypes.c_void_p(h))
        if not self.ctrl_h and not self.img_h:
            raise RuntimeError('两个 HID 接口都打不开, 请以管理员身份运行')
        return self

    def close(self):
        for h in (self.ctrl_h, self.img_h):
            if h:
                kernel32.CloseHandle(ctypes.c_void_p(h))
        self.ctrl_h = self.img_h = None

    def open_img_only(self):
        """只打开图像接口(MI_01), 全程不发任何控制命令 — 验证纯 JPEG 流能否独立驱动屏幕"""
        paths = find_hid_interfaces()
        if not paths:
            raise RuntimeError(f'未找到 VID_{VID:04X}&PID_{PID:04X} HID 设备')
        for p in paths:
            olen = get_output_report_len(p)
            if olen not in (1024, 1025):
                continue
            h = kernel32.CreateFileW(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_RW,
                                     None, OPEN_EXISTING, 0, None)
            if h not in (INVALID_HANDLE_VALUE, None):
                self.img_h, self.img_olen = h, olen
                break
        if not self.img_h:
            raise RuntimeError('图像接口打不开, 请以管理员身份运行')
        return self

    @property
    def is_open(self):
        return bool(self.ctrl_h)

    def _write(self, h, data: bytes, olen):
        wire_len = olen - 1
        if len(data) < wire_len:
            data = data + b'\x00' * (wire_len - len(data))
        data = b'\x00' + data[:wire_len]
        buf = ctypes.create_string_buffer(data)
        written = wt.DWORD(0)
        ok = kernel32.WriteFile(ctypes.c_void_p(h), ctypes.byref(buf), olen, ctypes.byref(written), None)
        return ok != 0 and written.value == olen

    # ---------- 控制命令 ----------
    def send_ctrl(self, cmd_id: int, value: int):
        """通用控制命令: 前4字节小端命令ID + 第5字节参数"""
        pkt = bytes([cmd_id & 0xFF, (cmd_id >> 8) & 0xFF, (cmd_id >> 16) & 0xFF,
                     (cmd_id >> 24) & 0xFF, value & 0xFF])
        return self._write(self.ctrl_h, pkt, self.ctrl_olen)

    def wake(self, brightness=None):
        """控制屏幕: 发送亮度控制命令（不带入控制则不发 100% 硬编码值）

        实测 0x80000112 同时承担"唤醒/控制"与"亮度"职责。
        brightness=None 时用 0（不改变用户设定亮度，仅触发控制）。
        """
        if not self.ctrl_h:
            raise RuntimeError('控制接口未打开')
        value = brightness if brightness is not None else 0x00
        if not self.send_ctrl(0x80000112, value):
            raise RuntimeError('控制命令发送失败')
        time.sleep(0.1)

    def set_brightness(self, v: int):
        v = max(0, min(100, int(v)))
        if not self.send_ctrl(0x80000112, v):
            raise RuntimeError('亮度命令发送失败')

    def set_screen(self, on: bool):
        """0x01=关闭, 0x00=开启"""
        v = 0x00 if on else 0x01
        if not self.send_ctrl(0x80000110, v):
            raise RuntimeError('屏幕开关命令发送失败')

    # ---------- 图像传输 ----------
    def send_jpeg_frame(self, jpg: bytes, block_delay: float = 0.004):
        """发送一帧 JPEG: 分块 1020B, 首块头 [08][总数][00][80], 后续 [08][序号][00][00]

        返回 (块数, 发送是否全部成功)
        """
        if not self.img_h:
            raise RuntimeError('图像接口未打开')
        n = (len(jpg) + 1019) // 1020
        ok_all = True
        for i in range(n):
            chunk = jpg[i * 1020:(i + 1) * 1020]
            if len(chunk) < 1020:
                chunk = chunk + b'\x00' * (1020 - len(chunk))
            if i == 0:
                head = bytes([0x08, n, 0x00, 0x80])
            else:
                head = bytes([0x08, i, 0x00, 0x00])
            ok = False
            for _ in range(3):
                if self._write(self.img_h, head + chunk, self.img_olen):
                    ok = True
                    break
                time.sleep(0.005)
            if not ok:
                ok_all = False
            if block_delay > 0:
                time.sleep(block_delay)
        return n, ok_all
