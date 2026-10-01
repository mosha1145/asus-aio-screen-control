#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
config_store.py — 软件配置存储
==============================
记录: 屏幕开关状态、亮度、显示内容（照片/视频及其路径）、
显示模式、图片帧率、视频质量档位。下次启动自动恢复。
"""
import json
import os
import threading

DEFAULT_CONFIG = {
    "screen_on": True,          # 屏幕开关
    "brightness": 50,           # 亮度 0-100
    "display_type": None,       # "photo" | "video" | None
    "photo_path": None,         # 当前照片素材路径
    "video_path": None,         # 当前视频素材路径
    "fit_mode": "contain",      # contain=适应 / cover=填充 / stretch=拉伸
    "photo_fps": 10.0,          # 照片发送帧率
    "video_quality": "medium",  # low / medium / high
    "rotation": 0,              # 画面旋转 0/90/180/270 (适配水冷安装方向)
    "playback_mode": "single",  # single=单曲循环 / list=列表循环
    "auto_start": False,        # 开机自启
    "power_actions": True,      # 主控: 发送电源操作时执行
    "power_close_app": True,    # 关闭程序时关闭水冷屏
    "power_shutdown": False,    # 关闭电脑时关闭水冷屏
    "power_logout": False,      # 注销时关闭水冷屏
    "power_sleep": False,       # 睡眠时关闭水冷屏
    "overlay_type": "none",     # 自定义覆盖层: none/clock/custom
    "clock_style": "text",      # 时钟预设: text=纯文本时间 / analog=简约钟表
    "clock_text": {             # 时钟/文本样式: x/y(0..1比例), alpha, scale, thick, color(BGR), bg(BGR)
        "x": 0.0, "y": 0.0, "alpha": 0.6, "scale": 0.6, "thick": 1,
        "color": [255, 255, 255], "bg": [0, 0, 0]
    },
    "custom_texts": [],         # 自定义文本: [{text,x,y,color(BGR),alpha,bg(BGR)}]
    "smtc_filter_mode": "all",  # SMTC 来源过滤: all/whitelist/blacklist
    "smtc_filter_apps": [],     # SMTC 过滤应用名列表 (白名单=仅这些; 黑名单=排除这些)
}


class ConfigStore:
    def __init__(self, path):
        self.path = path
        self._lock = threading.Lock()
        self.data = dict(DEFAULT_CONFIG)
        self.load()

    def load(self):
        try:
            if os.path.exists(self.path):
                with open(self.path, 'r', encoding='utf-8') as f:
                    loaded = json.load(f)
                    if isinstance(loaded, dict):
                        # 合并默认值（新版本加的字段用默认）
                        merged = dict(DEFAULT_CONFIG)
                        merged.update(loaded)
                        self.data = merged
        except Exception as e:
            print(f'[配置] 读取失败, 使用默认: {e}')

    def save(self):
        with self._lock:
            try:
                with open(self.path, 'w', encoding='utf-8') as f:
                    json.dump(self.data, f, ensure_ascii=False, indent=2)
            except Exception as e:
                print(f'[配置] 保存失败: {e}')

    def get(self, key, default=None):
        return self.data.get(key, default)

    def set(self, key, value):
        self.data[key] = value
        self.save()
