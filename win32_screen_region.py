"""直接读取 Windows 桌面的一块矩形，返回 OpenCV BGR 图像。"""

import ctypes
from ctypes import wintypes as win

import cv2
import numpy as np


class _BitmapHeader(ctypes.Structure):
    _fields_ = [
        ("size", win.DWORD), ("width", win.LONG), ("height", win.LONG),
        ("planes", win.WORD), ("bits", win.WORD), ("compression", win.DWORD),
        ("image_size", win.DWORD), ("x_pixels_per_meter", win.LONG), ("y_pixels_per_meter", win.LONG),
        ("colors_used", win.DWORD), ("colors_important", win.DWORD),
    ]


def _bind(dll, name, restype, *argtypes):
    func = getattr(dll, name)
    func.argtypes = argtypes
    func.restype = restype
    return func


class ScreenRegion:
    def __init__(self) -> None:
        u = ctypes.WinDLL("user32", use_last_error=True)
        g = ctypes.WinDLL("gdi32", use_last_error=True)
        self._get_dc = _bind(u, "GetDC", win.HDC, win.HWND)
        self._release_dc = _bind(u, "ReleaseDC", ctypes.c_int, win.HWND, win.HDC)
        self._create_dc = _bind(g, "CreateCompatibleDC", win.HDC, win.HDC)
        self._delete_dc = _bind(g, "DeleteDC", win.BOOL, win.HDC)
        self._create_bitmap = _bind(g, "CreateDIBSection", win.HBITMAP, win.HDC,
                                    ctypes.POINTER(_BitmapHeader), win.UINT,
                                    ctypes.POINTER(ctypes.c_void_p), win.HANDLE, win.DWORD)
        self._delete_bitmap = _bind(g, "DeleteObject", win.BOOL, win.HGDIOBJ)
        self._select = _bind(g, "SelectObject", win.HGDIOBJ, win.HDC, win.HGDIOBJ)
        self._copy = _bind(g, "BitBlt", win.BOOL, win.HDC, ctypes.c_int, ctypes.c_int,
                           ctypes.c_int, ctypes.c_int, win.HDC, ctypes.c_int, ctypes.c_int, win.DWORD)
        self._flush = _bind(g, "GdiFlush", win.BOOL)

    def grab(self, left: int, top: int, width: int, height: int) -> np.ndarray:
        screen = self._get_dc(None)
        if not screen:
            raise ctypes.WinError(ctypes.get_last_error())
        memory = bitmap = previous = None
        try:
            memory = self._create_dc(screen)
            if not memory:
                raise ctypes.WinError(ctypes.get_last_error())
            pixels = ctypes.c_void_p()
            header = _BitmapHeader(ctypes.sizeof(_BitmapHeader), width, -height, 1, 32)
            bitmap = self._create_bitmap(memory, ctypes.byref(header), 0, ctypes.byref(pixels), None, 0)
            if not bitmap:
                raise ctypes.WinError(ctypes.get_last_error())
            previous = self._select(memory, bitmap)
            if not previous:
                raise ctypes.WinError(ctypes.get_last_error())
            if not self._copy(memory, 0, 0, width, height, screen, left, top, 0x00CC0020):  # SRCCOPY
                raise ctypes.WinError(ctypes.get_last_error())
            self._flush()
            data = (ctypes.c_uint8 * (width * height * 4)).from_address(pixels.value)
            return cv2.cvtColor(np.ctypeslib.as_array(data).reshape(height, width, 4), cv2.COLOR_BGRA2BGR)
        finally:
            if previous:
                self._select(memory, previous)
            if bitmap:
                self._delete_bitmap(bitmap)
            if memory:
                self._delete_dc(memory)
            self._release_dc(None, screen)
