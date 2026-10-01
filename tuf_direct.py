#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tuf_direct.py — 纯 JPEG 直发模式（零控制命令）
================================================
验证: 仅通过图像接口(MI_01)发送 JPEG 图像块, 能否独立驱动水冷屏显示。

与 GUI 版的区别:
  - 不打开控制接口, 全程不发 wake/亮度/开关等任何控制命令
  - 打开图像接口后直接循环发送 JPEG 帧
  - 若屏幕能显示 -> JPEG 流本身即可驱动显示, 控制命令非必需
  - 若屏幕不显示 -> 需要先发送控制命令进入自定义模式

用法:
  python tuf_direct.py <照片或视频路径> [--fps 10] [--mode contain|cover|stretch] [--quality high|medium|low]

示例:
  python tuf_direct.py assets\\photos\\xx.jpg --fps 10 --mode cover
  python tuf_direct.py assets\\videos\\xx.mp4 --fps 15 --quality high
  Ctrl+C 停止
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cv2

import asset_lib
import tuf_hid


def run_photo(cooler, path, fps, mode, quality):
    """照片: 循环发送同一帧 JPEG"""
    img = asset_lib.imread_unicode(path)
    if img is None:
        sys.exit(f'读取照片失败: {path}')
    q = asset_lib.QUALITY_PRESETS.get(quality, 80)
    period = 1.0 / max(0.1, fps)
    count = 0
    t0 = time.time()
    print(f'[纯JPEG] 照片 {os.path.basename(path)}  {fps}fps  mode={mode}  quality={quality}({q})')
    print('[纯JPEG] 未发送任何控制命令, 观察水冷屏是否显示...')
    try:
        while True:
            loop_t0 = time.time()
            fitted = asset_lib.fit_frame(img, mode)
            jpg = asset_lib.bgr_to_jpeg(fitted, q)
            n, ok = cooler.send_jpeg_frame(jpg, block_delay=0.001)
            count += 1
            if count % (max(1, int(fps)) * 5) == 0:
                el = time.time() - t0
                print(f'  已发送 {count} 帧  实际 {count / el:.1f}fps  每帧 {n} 块  设备返回 {"OK" if ok else "写入失败"}')
                t0 = time.time()
                count = 0
            remain = period - (time.time() - loop_t0)
            if remain > 0:
                time.sleep(remain)
    except KeyboardInterrupt:
        print('\n已停止 (Ctrl+C)')


def run_video(cooler, path, fps, mode, quality):
    """视频: 按原视频帧率循环, 落后时丢帧保速"""
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        sys.exit(f'无法打开视频: {path}')
    src_fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    if fps and fps > 0:
        src_fps = float(fps)
    period = 1.0 / src_fps
    q = asset_lib.QUALITY_PRESETS.get(quality, 80)
    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    print(f'[纯JPEG] 视频 {os.path.basename(path)}  {w}x{h}@{src_fps:.1f}fps  mode={mode}  quality={quality}({q})')
    print('[纯JPEG] 未发送任何控制命令, 观察水冷屏是否显示...')
    timeline = time.time()
    count = 0
    t0 = time.time()
    try:
        while True:
            now = time.time()
            while timeline + 2.0 * period < now:
                ok_skip, _ = cap.read()
                if not ok_skip:
                    cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                    timeline = time.time()
                    break
                timeline += period
                now = time.time()
            loop_t0 = time.time()
            ok, frame = cap.read()
            if not ok:
                cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                timeline = time.time()
                continue
            fitted = asset_lib.fit_frame(frame, mode)
            jpg = asset_lib.bgr_to_jpeg(fitted, q)
            n, ok = cooler.send_jpeg_frame(jpg, block_delay=0.001)
            count += 1
            if count % (max(1, int(src_fps)) * 5) == 0:
                el = time.time() - t0
                print(f'  已发送 {count} 帧  实际 {count / el:.1f}fps  每帧 {n} 块  设备返回 {"OK" if ok else "写入失败"}')
                t0 = time.time()
                count = 0
            timeline += period
            remain = timeline - time.time()
            if remain > 0:
                time.sleep(remain)
    except KeyboardInterrupt:
        print('\n已停止 (Ctrl+C)')
    finally:
        cap.release()


def main():
    ap = argparse.ArgumentParser(
        description='TUF 水冷屏 纯JPEG直发模式(零控制命令)',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split('用法:')[1].split('Ctrl+C')[0])
    ap.add_argument('path', help='照片或视频路径')
    ap.add_argument('--fps', type=float, default=0.0,
                    help='目标帧率(视频默认用原帧率, 照片默认10)')
    ap.add_argument('--mode', choices=('contain', 'cover', 'stretch'), default='contain',
                    help='适配模式: contain适应/cover填充/stretch拉伸 (默认contain)')
    ap.add_argument('--quality', choices=('low', 'medium', 'high'), default='high',
                    help='JPEG质量档: low40/medium60/high80 (默认high)')
    args = ap.parse_args()

    path = args.path
    if not os.path.exists(path):
        sys.exit(f'文件不存在: {path}')
    ext = path.lower()
    if not ext.endswith(('.jpg', '.jpeg', '.png', '.bmp', '.mp4', '.avi', '.mov', '.mkv')):
        sys.exit(f'不支持的格式: {ext}')

    # 只打开图像接口, 全程零控制命令
    try:
        cooler = tuf_hid.TufCooler().open_img_only()
    except RuntimeError as e:
        sys.exit(f'设备打开失败: {e}')
    print('图像接口(MI_01)已打开 — 之后不再发送任何控制命令')

    # 提示: 若 GUI 正在显示, 先停掉避免混流
    print('提示: 若 GUI 版正在显示内容, 请先点"停止显示"再运行本脚本')

    try:
        if ext.endswith(('.jpg', '.jpeg', '.png', '.bmp')):
            fps = args.fps if args.fps > 0 else 10.0
            run_photo(cooler, path, fps, args.mode, args.quality)
        else:
            run_video(cooler, path, args.fps, args.mode, args.quality)
    except KeyboardInterrupt:
        pass
    finally:
        cooler.close()
        print('设备已关闭')


if __name__ == '__main__':
    main()
