#!/usr/bin/env python3
"""语音适配层：把设备的「裸 PCM」协议翻译成各家的云服务协议。

## 为什么需要它

设备端的协议是**厂商中立**的（故意这么设计的，`components/voice_service/README.md`）：

| 方向 | 设备发出 | 设备期待收到 |
|---|---|---|
| 识别 (ASR) | `POST`，body 是裸 PCM（16 kHz / 16 bit / 单声道） | `{"text": "识别结果"}` |
| 合成 (TTS) | `POST {"text": "...", "sample_rate": 16000}` | 裸 PCM 字节（16 kHz / 16 bit / 单声道） |

而**主流云服务没有一个是这个形状**：

* OpenAI / 硅基流动 / 各类 Whisper 服务：识别要 **multipart/form-data**（带 file 字段），
  合成默认回 **MP3**，要 PCM 得显式要 `response_format: "pcm"`，而且那份 PCM 是 **24 kHz**；
* 讯飞 / 阿里云 NLS：音频是 **base64 塞在 JSON 里**，不是裸字节；
* Edge TTS：回 **MP3**（设备没有 MP3 解码器，播不了）。

所以中间必须有这一层。**设备端一个字都不用改** —— 厂商的细节全留在这里，
换一家服务只是改个 `--*-backend`。

## 用法

```bash
# 1) 不接任何云服务，先把设备这一侧的链路跑通（假识别 + 一声测试音）
python tools/voice_proxy.py --mock

# 2) 接云服务（硅基流动的接口和 OpenAI 一样，都是 /v1 开头）
python tools/voice_proxy.py \
    --asr-backend openai --tts-backend openai \
    --base-url https://api.siliconflow.cn/v1 --api-key sk-xxx

# 3) 只接云识别，合成用 Edge TTS（免费、音色自然，但要 edge-tts 和 ffmpeg）
python tools/voice_proxy.py \
    --asr-backend openai --tts-backend edge \
    --base-url https://api.siliconflow.cn/v1 --api-key sk-xxx
```

设备端只要在**网页端 → AI 对话与语音**里把两个端点填成
`http://<这台电脑的地址>:<端口>/asr` 和 `.../tts` 就行。

## 依赖

只用 Python 标准库。OpenAI 兼容的后端需要 `pip install openai`（或直接用
`urllib` 拼 multipart —— 下面就是用 urllib 拼的，所以**不需要装任何东西**）。
Edge TTS 后端需要 `pip install edge-tts` 和 `ffmpeg`（用来把 MP3 变成裸 PCM）。
"""

from __future__ import annotations

import argparse
import io
import json
import math
import os
import struct
import sys
import tempfile
import urllib.error
import urllib.request
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_PORT = 8088
DEVICE_SAMPLE_RATE = 16000  # 设备端录音/播放的采样率（16 kHz / 16 bit / 单声道）


# --------------------------------------------------------------------------
# PCM 小工具
# --------------------------------------------------------------------------

def pcm_to_wav(pcm: bytes, sample_rate: int) -> bytes:
    """裸 PCM → WAV 文件字节（给 multipart 上传用）。"""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm)
    return buf.getvalue()


def wav_to_pcm(wav_bytes: bytes) -> bytes:
    """WAV 文件字节 → 裸 PCM。"""
    with wave.open(io.BytesIO(wav_bytes), "rb") as w:
        if w.getsampwidth() != 2 or w.getnchannels() != 1:
            raise ValueError("只支持 16 bit 单声道")
        return w.readframes(w.getnframes())


def resample_pcm(pcm: bytes, src_rate: int, dst_rate: int) -> bytes:
    """裸 PCM 单声道重采样（线性插值，够用于语音）。"""
    if src_rate == dst_rate or not pcm:
        return pcm
    samples = struct.unpack(f"<%dh" % (len(pcm) // 2), pcm)
    n_src = len(samples)
    n_dst = int(n_src * dst_rate / src_rate)
    if n_dst == 0:
        return b""
    out = []
    for i in range(n_dst):
        pos = i * src_rate / dst_rate
        i0 = int(pos)
        i1 = min(i0 + 1, n_src - 1)
        frac = pos - i0
        out.append(int(samples[i0] * (1 - frac) + samples[i1] * frac))
    return struct.pack("<%dh" % len(out), *out)


def make_tone(seconds: float, sample_rate: int = DEVICE_SAMPLE_RATE) -> bytes:
    """一段测试音（假 TTS 用）：让设备那边「真的播了东西」这件事可听可见。"""
    n = int(seconds * sample_rate)
    out = []
    for i in range(n):
        # 440 Hz + 每 0.5 秒一次短促的静音，方便听出边界
        gate = 1 if (i // (sample_rate // 2)) % 2 == 0 else 0
        v = int(12000 * gate * math.sin(2 * math.pi * 440 * i / sample_rate))
        out.append(v)
    return struct.pack("<%dh" % n, *out)


# --------------------------------------------------------------------------
# 后端
# --------------------------------------------------------------------------

def post_multipart(url: str, api_key: str, fields: dict, file_field: str,
                   filename: str, file_bytes: bytes, timeout: float) -> dict:
    """拼一个 multipart/form-data POST（不依赖第三方库）。"""
    boundary = "----ToDoHelperVoiceProxy"
    body = io.BytesIO()

    def write(text: str) -> None:
        body.write(text.encode("utf-8"))

    for key, value in fields.items():
        write(f"--{boundary}\r\n")
        write(f'Content-Disposition: form-data; name="{key}"\r\n\r\n')
        write(f"{value}\r\n")
    write(f"--{boundary}\r\n")
    write(f'Content-Disposition: form-data; name="{file_field}"; filename="{filename}"\r\n')
    write("Content-Type: audio/wav\r\n\r\n")
    body.write(file_bytes)
    write("\r\n")
    write(f"--{boundary}--\r\n")

    req = urllib.request.Request(url, data=body.getvalue(), method="POST")
    req.add_header("Content-Type", f"multipart/form-data; boundary={boundary}")
    if api_key:
        req.add_header("Authorization", f"Bearer {api_key}")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def post_json(url: str, api_key: str, payload: dict, timeout: float) -> bytes:
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode("utf-8"), method="POST")
    req.add_header("Content-Type", "application/json")
    if api_key:
        req.add_header("Authorization", f"Bearer {api_key}")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


class Backends:
    """各家云服务的调用。全部失败时抛异常，交给上层转成 502。"""

    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.base_url = args.base_url.rstrip("/")
        self.api_key = args.api_key

    # ---- 识别 ----

    def asr(self, pcm: bytes) -> str:
        backend = self.args.asr_backend
        if backend == "mock":
            return self.args.mock_text
        if backend == "openai":
            wav = pcm_to_wav(pcm, DEVICE_SAMPLE_RATE)
            data = post_multipart(
                f"{self.base_url}/audio/transcriptions",
                self.api_key,
                {"model": self.args.asr_model, "language": self.args.asr_language},
                "file", "speech.wav", wav, self.args.timeout)
            return (data.get("text") or "").strip()
        raise ValueError(f"未知的识别后端：{backend}")

    # ---- 合成 ----

    def tts(self, text: str, sample_rate: int) -> bytes:
        backend = self.args.tts_backend
        if backend == "mock":
            return make_tone(self.args.mock_tone_seconds, sample_rate)
        if backend == "openai":
            raw = post_json(
                f"{self.base_url}/audio/speech",
                self.api_key,
                {
                    "model": self.args.tts_model,
                    "input": text,
                    "voice": self.args.tts_voice,
                    "response_format": "pcm",   # 裸 PCM（注意：这份是 24 kHz）
                },
                self.args.timeout)
            return resample_pcm(raw, 24000, sample_rate)
        if backend == "edge":
            return self._tts_edge(text, sample_rate)
        raise ValueError(f"未知的合成后端：{backend}")

    def _tts_edge(self, text: str, sample_rate: int) -> bytes:
        """Edge TTS：免费、音色自然，但回的是 MP3 —— 需要 ffmpeg 转成裸 PCM。"""
        import shutil
        import subprocess

        if not shutil.which("ffmpeg"):
            raise RuntimeError("Edge TTS 后端需要 ffmpeg（把 MP3 转成裸 PCM）")
        with tempfile.TemporaryDirectory() as tmp:
            mp3 = os.path.join(tmp, "a.mp3")
            wav = os.path.join(tmp, "a.wav")
            subprocess.run(
                ["edge-tts", "--voice", self.args.edge_voice, "--text", text,
                 "--write-media", mp3],
                check=True, capture_output=True)
            subprocess.run(
                ["ffmpeg", "-y", "-loglevel", "error", "-i", mp3,
                 "-ac", "1", "-ar", str(sample_rate), "-f", "s16le", wav],
                check=True, capture_output=True)
            with open(wav, "rb") as f:
                return f.read()


# --------------------------------------------------------------------------
# HTTP 服务（就是设备端要的那个协议）
# --------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    backends: Backends = None  # type: ignore[assignment]

    def log_message(self, fmt: str, *args) -> None:  # 少吵一点
        sys.stderr.write("[voice-proxy] " + (fmt % args) + "\n")

    def _send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:  # noqa: N802
        try:
            if self.path.startswith("/asr"):
                self._asr()
            elif self.path.startswith("/tts"):
                self._tts()
            else:
                self._send(404, b'{"error":"no such endpoint"}', "application/json")
        except urllib.error.HTTPError as e:
            detail = e.read().decode("utf-8", "replace")[:400]
            self._send(502, json.dumps({"error": detail}).encode(),
                       "application/json")
        except Exception as e:  # noqa: BLE001
            self._send(502, json.dumps({"error": str(e)}).encode(),
                       "application/json")

    def do_GET(self) -> None:  # noqa: N802
        if self.path.startswith("/health"):
            self._send(200, json.dumps({"ok": True}).encode(), "application/json")
        else:
            self._send(404, b'{"error":"not found"}', "application/json")

    def _body(self) -> bytes:
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def _asr(self) -> None:
        pcm = self._body()
        text = self.backends.asr(pcm)
        self._send(200, json.dumps({"text": text}).encode("utf-8"),
                   "application/json")

    def _tts(self) -> None:
        req = json.loads(self._body().decode("utf-8") or "{}")
        text = req.get("text") or ""
        rate = int(req.get("sample_rate") or DEVICE_SAMPLE_RATE)
        pcm = self.backends.tts(text, rate)
        self._send(200, pcm, "audio/pcm")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--asr-backend", default="mock",
                    choices=["mock", "openai"], help="openai = OpenAI 兼容（硅基流动等）")
    ap.add_argument("--tts-backend", default="mock",
                    choices=["mock", "openai", "edge"])
    ap.add_argument("--base-url", default="https://api.siliconflow.cn/v1",
                    help="OpenAI 兼容服务的 /v1 根地址")
    ap.add_argument("--api-key", default=os.environ.get("VOICE_PROXY_API_KEY", ""))
    ap.add_argument("--asr-model", default="FunAudioLLM/SenseVoiceSmall")
    ap.add_argument("--asr-language", default="zh")
    ap.add_argument("--tts-model", default="FunAudioLLM/CosyVoice2-0.5B")
    ap.add_argument("--tts-voice", default="FunAudioLLM/CosyVoice2-0.5B:anna")
    ap.add_argument("--edge-voice", default="zh-CN-XiaoxiaoNeural")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--mock-text", default="这是代理的假识别结果",
                    help="mock 识别后端返回的文本")
    ap.add_argument("--mock-tone-seconds", type=float, default=1.0,
                    help="mock 合成后端的测试音时长")
    ap.add_argument("--mock", action="store_true",
                    help="两个后端都用 mock（等价于 --asr-backend mock --tts-backend mock）")
    args = ap.parse_args()

    if args.mock:
        args.asr_backend = "mock"
        args.tts_backend = "mock"

    Handler.backends = Backends(args)
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"[voice-proxy] 监听 http://{args.host}:{args.port}  "
          f"asr={args.asr_backend} tts={args.tts_backend}", flush=True)
    print(f"[voice-proxy] 设备端填： http://<本机地址>:{args.port}/asr  与  "
          f"http://<本机地址>:{args.port}/tts", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
