#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_t2.py -- T2 端到端验收:运行时下发 payload -> 落 flash -> 复位 -> 内容还在。

链路:
  1. 宿主往 RAM disk 的 PAYLOAD.BIN 写 [64 字节头][payload](补零到信箱容量)
  2. 设备轮询到变化 -> CRC 校验 -> 写 script0/script1(A/B) -> 覆盖 RAM disk 槽位
  3. 宿主复位设备 -> 开机 overlay 从 flash 把 payload 还原回槽位
  4. 宿主读回槽位文件,和源文件逐字节比对

判据:
  PASS  设备日志出现 "payload stored"
  PASS  设备日志出现 "flash payload active"(复位后)
  PASS  槽位文件大小 == payload 长度
  PASS  槽位文件内容 sha256 == 源文件 sha256

纯 ASCII(Windows 工具链约定)。
"""

import ctypes
import hashlib
import os
import re
import struct
import subprocess
import sys
import time
import zlib

try:
    import serial
except ImportError:
    print("NO_PYSERIAL")
    sys.exit(3)

LABEL = "IOAGENT"
PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAYLOAD_SRC = os.path.join(REPO, "watcher", "watch.ps1")
SHELL_SRC = os.path.join(REPO, ".stage", "t2_agent.sh")
INBOX_NAME = "PAYLOAD.BIN"
INBOX_CAPACITY = 131072
SLOT_NAME = "WATCH.PS1"
SLOT_INDEX = 0
SHELL_SLOT_NAME = "AGENT.SH"
SHELL_SLOT_INDEX = 1
HDR_SIZE = 64
MAGIC = b"IOPB"

# 固件日志里的快照标识。写盘:"flash write done in N ms (part=scriptX seq=N records=N total=N)"
# 开机:"flash payload active: part=scriptX seq=N records=N" -- 两种格式同一套字段。
FLASH_WRITE_RE = r"part=(script\d) seq=(\d+) records=(\d+)"

READER = os.path.join(REPO, "tools", "read_serial.py")

# 每次运行都带唯一标记:设备对"内容与 flash 快照相同"的信箱会走幂等分支跳过
# 写入,不带标记的话第二次跑这个测试就测不到真正的 flash 写入了。
RUN_ID = "{:08x}".format(int(time.time()) & 0xFFFFFFFF)


def make_shell_body(run_id):
    return (
        "#!/bin/sh\n"
        "# T2 payload smoke test -- deployed into AGENT.SH by tools/verify_t2.py\n"
        "# t2-run {}\n"
        'echo "io_agent payload ok"\n'
        "exit 0\n"
    ).format(run_id).encode("ascii")


# ----------------------------------------------------------------- volume

def find_volume(label):
    mask = ctypes.windll.kernel32.GetLogicalDrives()
    for i in range(26):
        if not (mask >> i) & 1:
            continue
        root = "{}:\\".format(chr(ord("A") + i))
        name = ctypes.create_unicode_buffer(261)
        fsname = ctypes.create_unicode_buffer(261)
        serial_no = ctypes.c_ulong()
        max_comp = ctypes.c_ulong()
        flags = ctypes.c_ulong()
        ok = ctypes.windll.kernel32.GetVolumeInformationW(
            ctypes.c_wchar_p(root), name, 261, ctypes.byref(serial_no),
            ctypes.byref(max_comp), ctypes.byref(flags), fsname, 261)
        if ok and name.value == label:
            return root
    return None


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(65536)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


# ----------------------------------------------------------------- serial

class LogReader:
    """后台读串口日志。注意:打开串口前必须先拉低 DTR/RTS,否则开发板会被复位。"""

    def __init__(self, port):
        self.buf = ""
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.2
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.ser.dtr = False
        self.ser.rts = False

    def drain(self):
        try:
            while True:
                chunk = self.ser.read(4096)
                if not chunk:
                    break
                self.buf += chunk.decode("utf-8", "replace")
        except Exception:
            pass
        return self.buf

    def mark(self):
        """当前缓冲长度,用作后续 wait_for 的搜索起点。

        必须先 drain 再取长度,否则已经在串口里但还没读进缓冲的日志会被跳过。
        """
        self.drain()
        return len(self.buf)

    def wait_for(self, needle, timeout, since=0):
        """在 buf[since:] 里搜 needle(诊断用)。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.drain()
            if needle in self.buf[since:]:
                return True
            time.sleep(0.2)
        print("  [diag] wait_for {!r} timeout; since={} buflen={}".format(
            needle, since, len(self.buf)))
        print("  [diag] buf[since:] tail: {}".format(
            self.buf[since:][-400:].replace("\r", " ").replace("\n", " | ")))
        return False

    def count(self, needle):
        return self.buf.count(needle)

    def wait_count(self, needle, timeout, baseline):
        """等到 needle 出现次数超过 baseline 才算命中。

        不能用"记录偏移量再往后搜"的写法:宿主的 fsync 可能阻塞好几秒,
        设备在这段时间里已经写完 flash 并打了日志,偏移量会把这条日志挡在
        判据之外(实测踩过:mark() 取到 buf=400,而 payload stored 就在
        buf[0:400] 里)。计数法对"日志在基线前后到达"都成立。
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.drain()
            if self.buf.count(needle) > baseline:
                return True
            time.sleep(0.2)
        print("  [diag] wait_count {!r} timeout; baseline={} now={}".format(
            needle, baseline, self.buf.count(needle)))
        print("  [diag] buf tail: {}".format(
            self.buf[-600:].replace("\r", " ").replace("\n", " | ")))
        return False

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


# ----------------------------------------------------------------- main

def write_inbox(root, slot_index, payload):
    """把 [头][payload] 原位写进信箱。返回写进去的字节数。"""
    inbox = os.path.join(root, INBOX_NAME)
    hdr = MAGIC + struct.pack("<IIIIII", 1, slot_index, len(payload),
                              zlib.crc32(payload) & 0xFFFFFFFF, 1, 0)
    hdr += b"\x00" * (HDR_SIZE - len(hdr))
    blob = hdr + payload
    blob += b"\x00" * (INBOX_CAPACITY - len(blob))
    # 必须**原位覆写**:用 "wb" 打开会截断文件,Windows 随即释放原簇链并
    # 另分配新簇 -- 设备是按"构建期算好的簇位置"读信箱的,文件一旦搬家
    # 就读不到。"r+b" 不截断,Windows 直接写进原来的簇,链不变。
    cur = os.path.getsize(inbox)
    if cur != INBOX_CAPACITY:
        raise RuntimeError("inbox size {} != expected {} (factory image broken)".format(
            cur, INBOX_CAPACITY))
    with open(inbox, "r+b") as f:
        f.seek(0)
        f.write(blob)
        f.flush()
        os.fsync(f.fileno())
    return len(blob)


def snapshot_slots(root, names):
    """读槽位文件的 (size, sha256)。用来证明"运行期下发没碰副本"。"""
    out = {}
    for name in names:
        path = os.path.join(root, name)
        try:
            if not os.path.exists(path):
                out[name] = ("missing", "")
                continue
            data = open(path, "rb").read()
            out[name] = (len(data), hashlib.sha256(data).hexdigest())
        except OSError as e:
            out[name] = ("error", str(e))
    return out


def wait_slot_file(root, name, expect_size, timeout=25.0):
    """等槽位文件在复位后重新枚举出来且大小正确。

    复位会让 USB 重新枚举,Windows 重新挂载卷。设备 overlay 是开机时改的
    目录项 size 字段,所以必须等新的枚举结果,不能直接读旧句柄。
    """
    path = os.path.join(root, name)
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        try:
            if os.path.exists(path):
                size = os.path.getsize(path)
                if size == expect_size:
                    return True, size
                last = "size={} want={}".format(size, expect_size)
            else:
                last = "not present yet"
        except OSError as e:
            last = "os error: {}".format(e)
        time.sleep(0.5)
    return False, last


def main():
    root = find_volume(LABEL)
    if not root:
        print("FAIL: volume {} not found".format(LABEL))
        return 2
    print("volume {} at {}".format(LABEL, root))

    with open(PAYLOAD_SRC, "rb") as f:
        payload = f.read()
    # 追加唯一标记:内容与 flash 快照相同的话设备会走幂等分支跳过写入,
    # 那样就测不到真正的 flash 写入了。
    payload += b"\n# t2-run " + RUN_ID.encode("ascii") + b"\n"
    if len(payload) > INBOX_CAPACITY - HDR_SIZE:
        print("FAIL: payload {} bytes exceeds inbox capacity".format(len(payload)))
        return 2
    src_sha = hashlib.sha256(payload).hexdigest()
    print("run id: {}".format(RUN_ID))
    print("payload source: {} + run marker ({} bytes) sha256={}".format(
        PAYLOAD_SRC, len(payload), src_sha))

    shell_body = make_shell_body(RUN_ID)
    os.makedirs(os.path.dirname(SHELL_SRC), exist_ok=True)
    with open(SHELL_SRC, "wb") as f:
        f.write(shell_body)
    shell_sha = hashlib.sha256(shell_body).hexdigest()

    inbox = os.path.join(root, INBOX_NAME)
    slot_path = os.path.join(root, SLOT_NAME)
    shell_path = os.path.join(root, SHELL_SLOT_NAME)
    if not os.path.exists(inbox):
        print("FAIL: {} not found on the volume".format(INBOX_NAME))
        return 2

    log = LogReader(PORT)
    results = []
    try:
        # --- 0. 下发前的槽位状态(设备开机时 apply 过 flash 里的旧内容) ----
        before = snapshot_slots(root, (SLOT_NAME, SHELL_SLOT_NAME))
        print("slots before deploy: {}".format(before))

        # --- 1. 第一次下发:真实 watcher -> WATCH.PS1 ----------------------
        # 计数基线必须在写信箱**之前**取:fsync 可能阻塞好几秒,设备在这期间
        # 就写完 flash 并打了日志,基线取晚了会把这条算成"旧日志"。
        base = log.count("payload stored")
        n = write_inbox(root, SLOT_INDEX, payload)
        print("inbox <- slot {} ({} bytes)".format(SLOT_INDEX, n))

        mark = log.mark()
        ok = log.wait_count("payload stored", 90, base)
        results.append(("device logged 'payload stored' (slot 0)", ok))
        if not ok:
            print("FAIL: device did not report 'payload stored'")
            print(log.drain()[-2000:])
            return 1
        w1 = re.findall(FLASH_WRITE_RE, log.buf[mark:])
        if not w1:
            # 日志可能落在 mark 之前(设备在 fsync 阻塞期间就写完了),退回全量找
            w1 = re.findall(FLASH_WRITE_RE, log.buf)
        if not w1:
            print("FAIL: no flash write record in log")
            print(log.buf[-2000:])
            return 1
        part1, seq1, rec1 = w1[-1][0], int(w1[-1][1]), int(w1[-1][2])
        print("write #1: part={} seq={} records={}".format(part1, seq1, rec1))
        # 快照里的槽位数**取决于之前下发了多少个槽**,不能写死:
        # 从出厂态第一次跑是 1,重复跑就是 2。这里只断言"非空"。
        results.append(("write #1 snapshot non-empty", rec1 >= 1))

        # --- 2. 第二次下发:另一个槽 -> AGENT.SH (走另一个 A/B 分区) -------
        base2 = log.count("payload stored")
        n = write_inbox(root, SHELL_SLOT_INDEX, shell_body)
        print("inbox <- slot {} ({} bytes)".format(SHELL_SLOT_INDEX, n))

        mark2 = log.mark()
        ok2 = log.wait_count("payload stored", 90, base2)
        results.append(("device logged 'payload stored' (slot 1)", ok2))
        if not ok2:
            print("FAIL: device did not report 'payload stored' for slot 1")
            print(log.drain()[-3000:])
            return 1
        w2 = re.findall(FLASH_WRITE_RE, log.buf[mark2:])
        if not w2:
            w2 = re.findall(FLASH_WRITE_RE, log.buf)
        if not w2:
            print("FAIL: no flash write record in log for slot 1")
            print(log.buf[-2000:])
            return 1
        part2, seq2, rec2 = w2[-1][0], int(w2[-1][1]), int(w2[-1][2])
        print("write #2: part={} seq={} records={}".format(part2, seq2, rec2))
        results.append(("slot 1 named in device log",
                        "payload stored: slot {}".format(SHELL_SLOT_NAME) in log.buf))
        # A/B:第二次必须写进**另一个**分区,且 seq 递增
        results.append(("A/B alternated (write #2 used the other partition)",
                        part1 != part2))
        results.append(("seq increased ({} -> {})".format(seq1, seq2), seq2 > seq1))
        # 关键不变量:**写一个槽位不会把别的槽位挤掉**(槽位数不减),
        # 且两个下发的槽位最终都在同一份快照里。
        results.append(("write #2 snapshot dropped no slot", rec2 >= rec1))
        results.append(("write #2 snapshot holds both deployed slots", rec2 >= 2))

        # --- 2.5 运行期下发不得触碰 RAM disk 副本 -------------------------
        # 契约:flash 是唯一真源,RAM disk 槽位是"每次开机铺开的副本"。
        # 运行期下发只写 flash,所以盘上槽位的 size+sha256 必须与下发前**完全一致**。
        after = snapshot_slots(root, (SLOT_NAME, SHELL_SLOT_NAME))
        print("slots after deploy : {}".format(after))
        results.append(("runtime deploy did NOT touch the RAM disk copy",
                        before == after))

        # --- 3. 复位,验证 flash 持久化 ------------------------------------
        log.close()
        env = dict(os.environ)
        env["PYTHONIOENCODING"] = "utf-8"
        out = subprocess.run([sys.executable, READER, PORT, "18", "--reset"],
                             capture_output=True, text=True, encoding="utf-8",
                             errors="replace", env=env)
        boot = out.stdout or ""
        ok_boot = "flash payload active" in boot
        results.append(("boot log shows 'flash payload active'", ok_boot))
        ok_overlay = boot.count("overlay applied") >= 2
        results.append(("boot log shows 2 overlays applied", ok_overlay))
        boot_m = re.findall(FLASH_WRITE_RE, boot)
        if boot_m:
            print("boot active: part={} seq={} records={}".format(*boot_m[-1]))
        results.append(("boot activated the newest snapshot",
                        bool(boot_m) and boot_m[-1][0] == part2
                        and int(boot_m[-1][1]) == seq2))
        if not ok_boot:
            print("FAIL: flash payload not active after reset")
            print(boot[-3000:])
            return 1

        # --- 4. 槽位内容比对 ----------------------------------------------
        ok_size, info = wait_slot_file(root, SLOT_NAME, len(payload))
        results.append(("slot file size == payload length", ok_size))
        if not ok_size:
            print("FAIL: slot 0 size mismatch: {}".format(info))
            return 1
        got = open(slot_path, "rb").read()
        got_sha = hashlib.sha256(got).hexdigest()
        results.append(("slot file sha256 == payload sha256", got_sha == src_sha))
        print("slot file: {} bytes sha256={}".format(len(got), got_sha))

        ok_size2, info2 = wait_slot_file(root, SHELL_SLOT_NAME, len(shell_body))
        results.append(("slot 1 file size == payload length", ok_size2))
        if not ok_size2:
            print("FAIL: slot 1 size mismatch: {}".format(info2))
            got2_sha = ""
        else:
            got2 = open(shell_path, "rb").read()
            got2_sha = hashlib.sha256(got2).hexdigest()
            results.append(("slot 1 sha256 == payload sha256", got2_sha == shell_sha))
            print("slot 1 file: {} bytes sha256={}".format(len(got2), got2_sha))

        # --- 5. 第三个槽仍是出厂态 ----------------------------------------
        other_ok = True
        for name in ("AGENT.EXE",):
            p = os.path.join(root, name)
            if os.path.exists(p) and os.path.getsize(p) != 0:
                other_ok = False
        results.append(("untouched slot still empty (factory)", other_ok))
    finally:
        log.close()

    print("")
    failed = 0
    for name, ok in results:
        print("{} {}".format("PASS" if ok else "FAIL", name))
        if not ok:
            failed += 1
    print("RESULT: {}".format("OK" if failed == 0 else "FAILED ({} item(s))".format(failed)))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
