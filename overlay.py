# -*- coding: utf-8 -*-
"""
overlay.py — 画面叠加层 (硬件信息 / 时钟 / 自定义文本)
在 320x320 BGR 帧上绘制, 发送前与预览共用同一绘制结果
"""
import math
import time

import cv2

FONT = cv2.FONT_HERSHEY_SIMPLEX


def _put_text(out, text, x, y, scale, color, thick, bg=None, bg_alpha=0.0):
    """画文本(带黑描边), 可选半透明底色。x,y 为文本基线坐标"""
    if bg is not None and bg_alpha > 0:
        (tw, th), _ = cv2.getTextSize(text, FONT, scale, thick)
        x0, y0 = x - 2, y - th - 2
        ov = out.copy()
        cv2.rectangle(ov, (x0, y0), (x0 + tw + 4, y0 + th + 4), bg, -1)
        out[:] = cv2.addWeighted(ov, bg_alpha, out, 1.0 - bg_alpha, 0)
    cv2.putText(out, text, (x, y), FONT, scale, (0, 0, 0), thick + 2, cv2.LINE_AA)
    cv2.putText(out, text, (x, y), FONT, scale, color, thick, cv2.LINE_AA)


def _draw_clock_analog(out, t=None):
    """简约钟表: 表盘 + 12 刻度 + 时/分/秒针 (居中) — 图形样式, 不参与文本编辑"""
    h, w = out.shape[:2]
    if t is None:
        t = time.localtime()
    cx, cy = w // 2, h // 2
    r = min(w, h) // 2 - 12
    cv2.circle(out, (cx, cy), r, (255, 255, 255), 2, cv2.LINE_AA)
    for i in range(12):
        ang = math.radians(i * 30)
        x1 = int(cx + (r - 9) * math.sin(ang)); y1 = int(cy - (r - 9) * math.cos(ang))
        x2 = int(cx + r * math.sin(ang)); y2 = int(cy - r * math.cos(ang))
        cv2.line(out, (x1, y1), (x2, y2), (255, 255, 255), 2, cv2.LINE_AA)
    hr, mn, sc = t.tm_hour % 12, t.tm_min, t.tm_sec
    a = math.radians(hr * 30 + mn * 0.5 - 90)
    cv2.line(out, (cx, cy), (int(cx + r * 0.5 * math.cos(a)), int(cy + r * 0.5 * math.sin(a))),
             (255, 255, 255), 4, cv2.LINE_AA)
    a = math.radians(mn * 6 + sc * 0.1 - 90)
    cv2.line(out, (cx, cy), (int(cx + r * 0.75 * math.cos(a)), int(cy + r * 0.75 * math.sin(a))),
             (255, 255, 255), 3, cv2.LINE_AA)
    a = math.radians(sc * 6 - 90)
    cv2.line(out, (cx, cy), (int(cx + r * 0.85 * math.cos(a)), int(cy + r * 0.85 * math.sin(a))),
             (0, 140, 255), 2, cv2.LINE_AA)
    cv2.circle(out, (cx, cy), 4, (255, 255, 255), -1)


def _draw_text_items(out, items):
    """统一文本条目绘制: 位置/颜色/底色/透明度/字号/粗细可调
    text == '__time__' 时显示实时时间 %H:%M:%S (时钟纯文本样式复用此逻辑)"""
    h, w = out.shape[:2]
    for it in items:
        if it.get('text') == '__time__':
            text = time.strftime('%H:%M:%S')
        else:
            text = str(it.get('text', ''))
        if not text:
            continue
        xf = min(max(float(it.get('x', 0.5)), 0.0), 1.0)
        yf = min(max(float(it.get('y', 0.5)), 0.0), 1.0)
        color = tuple(int(c) for c in it.get('color', (255, 255, 255)))
        bg = tuple(int(c) for c in it.get('bg', (0, 0, 0)))
        alpha = min(max(float(it.get('alpha', 0.6)), 0.0), 1.0)
        scale = max(0.2, min(2.0, float(it.get('scale', 0.6))))
        thick = max(1, min(5, int(it.get('thick', 1))))
        x = int(round((w - 1) * xf))
        y = int(round((h - 1) * yf))
        _put_text(out, text, x, y, scale, color, thick, bg=bg, bg_alpha=alpha)


def draw_overlay(bgr, hub, placements, cfg=None):
    """绘制覆盖层: cfg = {overlay_type, clock_style, custom_texts}
    注: hub/placements 为旧 HWInfo 硬件信息叠加的参数, 该功能已移除, 保留仅为兼容调用签名"""
    cfg = cfg or {}
    out = bgr.copy()
    # 1) 时钟
    if cfg.get('overlay_type') == 'clock':
        if cfg.get('clock_style') == 'analog':
            _draw_clock_analog(out)
        else:
            # 纯文本时间: 复用文本条目绘制, 样式/位置可编辑 (cfg['clock_text'])
            _draw_text_items(out, [cfg.get('clock_text') or {}])
    # 2) 自定义文本
    if cfg.get('overlay_type') == 'custom':
        _draw_text_items(out, cfg.get('custom_texts', []))
    return out
