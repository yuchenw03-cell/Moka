#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""wbx_gen.py — 墨卡 .wbx 词书生成工具

实现 docs/wbx_format.md v1.0 定义的二进制格式：
64B 文件头 + word_count*48B 定长索引 + 文本区（word\\0 phon\\0 gloss\\0 ex\\0 四段连续），
最后回填 CRC32（crc32 字段自身置 0 后计算）。

仅使用 Python 3.9+ 标准库，无第三方依赖。

用法：
    python3 wbx_gen.py --input sample.jsonl --name "考研核心" --output sample.wbx
    python3 wbx_gen.py --verify sample.wbx
    python3 wbx_gen.py --input sample.jsonl --name "考研核心" --output sample.wbx --verify sample.wbx
"""

import argparse
import csv
import difflib
import json
import os
import struct
import sys
import unicodedata
import zlib

MAGIC = b"WBX1"
VERSION = 1
HEADER_SIZE = 64
ENTRY_SIZE = 48
NAME_MAX = 32        # 含 \0
WORD_MAX = 48        # 含 \0
FILL = 0xFFFFFFFF    # 干扰项空槽
MAX_DISTRACT = 5

FLAG_EXAMPLE_HIGHLIGHT = 0x0001

SOH = "\x01"
STX = "\x02"

HEADER_STRUCT = struct.Struct("<4s5I32s2I")
ENTRY_STRUCT = struct.Struct("<IBB4HHI5I8s")

POS_MAP = {
    "未知": 0, "unknown": 0, "": 0,
    "n": 1, "v": 2, "adj": 3, "adv": 4, "prep": 5,
    "conj": 6, "pron": 7, "num": 8, "abbr": 9, "phr": 10,
}
POS_NAMES = {0: "未知", 1: "n", 2: "v", 3: "adj", 4: "adv", 5: "prep",
             6: "conj", 7: "pron", 8: "num", 9: "abbr", 10: "phr"}


class GenError(Exception):
    pass


def norm_text(s):
    """NFC 归一化并去首尾空白；None 视为空串。"""
    if s is None:
        return ""
    return unicodedata.normalize("NFC", str(s)).strip()


def parse_pos(value, line_desc):
    """词性：枚举名（n/v/adj/...）或 0-10 数字。"""
    s = norm_text(value).lower()
    if s in POS_MAP:
        return POS_MAP[s]
    if s.isdigit() and 0 <= int(s) <= 10:
        return int(s)
    raise GenError("%s: 非法词性 %r（枚举名 n/v/adj/adv/prep/conj/pron/num/abbr/phr 或 0-10）"
                   % (line_desc, value))


def parse_bucket(value, line_desc):
    s = norm_text(value)
    if s.isdigit() and 0 <= int(s) <= 9:
        return int(s)
    raise GenError("%s: 非法 freq_bucket %r（应为 0-9）" % (line_desc, value))


def make_entry(word, phonetic, pos, gloss, example, freq_bucket):
    return {
        "word": norm_text(word),
        "phonetic": norm_text(phonetic),
        "pos": pos,
        "gloss": norm_text(gloss),
        "example": norm_text(example),
        "freq_bucket": freq_bucket,
    }


def load_jsonl(path):
    entries = []
    with open(path, "r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as e:
                raise GenError("JSONL 第 %d 行解析失败：%s" % (lineno, e))
            desc = "JSONL 第 %d 行" % lineno
            entries.append(make_entry(
                obj.get("word", ""),
                obj.get("phonetic", ""),
                parse_pos(obj.get("pos", 0), desc),
                obj.get("gloss", ""),
                obj.get("example", ""),
                parse_bucket(obj.get("freq_bucket", ""), desc),
            ))
    return entries


def load_csv(path):
    entries = []
    with open(path, "r", encoding="utf-8-sig", newline="") as f:
        reader = csv.reader(f)
        rows = list(reader)
    if not rows:
        return entries
    header = [h.strip().lower() for h in rows[0]]
    named = "word" in header
    for i, row in enumerate(rows[1:] if named else rows, 2 if named else 1):
        if not row or all(not c.strip() for c in row):
            continue
        desc = "CSV 第 %d 行" % i
        if named:
            rec = dict(zip(header, row))
            get = lambda k: rec.get(k, "")
        else:
            row = row + [""] * (6 - len(row))
            get = lambda k, _r=row: _r[["word", "phonetic", "pos", "gloss",
                                        "example", "freq_bucket"].index(k)]
        entries.append(make_entry(
            get("word"), get("phonetic"), parse_pos(get("pos"), desc),
            get("gloss"), get("example"), parse_bucket(get("freq_bucket"), desc),
        ))
    return entries


def load_input(path):
    ext = os.path.splitext(path)[1].lower()
    if ext == ".csv":
        return load_csv(path)
    if ext in (".jsonl", ".json"):
        return load_jsonl(path)
    raise GenError("不支持的输入格式 %r（仅支持 .csv / .jsonl）" % ext)


def dedup_and_sort(entries):
    """word 大小写不敏感去重（保留首见），按小写词形排序，word_id 从 0 连续编号。"""
    seen = {}
    for e in entries:
        key = e["word"].casefold()
        if not key:
            raise GenError("存在空 word 词条")
        if key not in seen:
            seen[key] = e
    words = sorted(seen.values(), key=lambda e: e["word"].casefold())
    for wid, e in enumerate(words):
        e["word_id"] = wid
    return words


def highlight_example(word, example):
    """例句中目标词形（大小写不敏感，字母序列边界）用 SOH/STX 包裹。
    返回 (处理后的例句, flags)。找不到目标词则原样返回，flags=0。"""
    if not example:
        return "", 0
    target = word.casefold()
    n = len(target)
    if not n:
        return example, 0
    low = example.casefold()
    start = 0
    while True:
        i = low.find(target, start)
        if i < 0:
            return example, 0
        j = i + n
        before_ok = i == 0 or not example[i - 1].isalpha()
        after_ok = j >= len(example) or not example[j].isalpha()
        if before_ok and after_ok:
            return example[:i] + SOH + example[i:j] + STX + example[j:], \
                FLAG_EXAMPLE_HIGHLIGHT
        start = i + 1


def build_distractors(words):
    """同 freq_bucket 内抽最多 5 个干扰项，按加权得分降序：
    编辑距离近（difflib ratio * 2.0）+ 首字母相同（+1.0）+ 同词性（+1.0）。
    绝不跨桶；不足 5 个补 0xFFFFFFFF。"""
    by_bucket = {}
    for e in words:
        by_bucket.setdefault(e["freq_bucket"], []).append(e)

    for e in words:
        pool = [c for c in by_bucket[e["freq_bucket"]] if c["word_id"] != e["word_id"]]
        w_lower = e["word"].casefold()
        scored = []
        for c in pool:
            c_lower = c["word"].casefold()
            score = 2.0 * difflib.SequenceMatcher(None, w_lower, c_lower).ratio()
            if c_lower[:1] == w_lower[:1]:
                score += 1.0
            if c["pos"] == e["pos"]:
                score += 1.0
            scored.append((score, c["word_id"]))
        scored.sort(key=lambda t: (-t[0], t[1]))
        picked = [wid for _, wid in scored[:MAX_DISTRACT]]
        picked += [FILL] * (MAX_DISTRACT - len(picked))
        e["distract"] = picked


def build_wbx(name, words):
    """生成 .wbx 字节串。返回 (data, text_len)。"""
    name_bytes = unicodedata.normalize("NFC", name).encode("utf-8") + b"\x00"
    if len(name_bytes) > NAME_MAX:
        raise GenError("词书名过长：%d 字节（含\\0，上限 %d）" % (len(name_bytes), NAME_MAX))

    # 文本区 + 索引字段
    text = bytearray()
    for e in words:
        word_b = e["word"].encode("utf-8") + b"\x00"
        phon_b = (e["phonetic"].encode("utf-8") + b"\x00") if e["phonetic"] else b""
        gloss_b = e["gloss"].encode("utf-8") + b"\x00"
        ex, flags = highlight_example(e["word"], e["example"])
        ex_b = (ex.encode("utf-8") + b"\x00") if ex else b""

        if len(word_b) > WORD_MAX:
            raise GenError("word %r 过长：%d 字节（含\\0，上限 %d）"
                           % (e["word"], len(word_b), WORD_MAX))
        for field, b in (("phonetic", phon_b), ("gloss", gloss_b), ("example", ex_b)):
            if len(b) > 0xFFFF:
                raise GenError("word %r 的 %s 过长（u16 长度字段溢出）" % (e["word"], field))

        e["_lens"] = (len(word_b), len(phon_b), len(gloss_b), len(ex_b))
        e["_flags"] = flags
        e["_ofs"] = len(text)
        text += word_b + phon_b + gloss_b + ex_b

    word_count = len(words)
    text_ofs = HEADER_SIZE + word_count * ENTRY_SIZE

    header = HEADER_STRUCT.pack(
        MAGIC, VERSION, word_count, HEADER_SIZE, text_ofs,
        len(name_bytes), name_bytes.ljust(NAME_MAX, b"\x00"),
        0,  # crc32 先置 0
        0,  # reserved
    )

    index = bytearray()
    for e in words:
        wl, pl, gl, xl = e["_lens"]
        index += ENTRY_STRUCT.pack(
            e["word_id"], e["freq_bucket"], e["pos"],
            wl, pl, gl, xl, e["_flags"], e["_ofs"],
            *e["distract"], b"\x00" * 8,
        )

    data = bytearray(header + bytes(index) + bytes(text))
    crc = zlib.crc32(bytes(data)) & 0xFFFFFFFF
    struct.pack_into("<I", data, 0x38, crc)
    return bytes(data), len(text)


def verify_wbx(path):
    """读回 .wbx 全量自检。返回 (word_count, name, crc, errors, cross_bucket_count)。"""
    errors = []
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < HEADER_SIZE:
        return 0, "", 0, ["文件不足 64B 文件头"], 0

    (magic, version, word_count, meta_ofs, text_ofs,
     name_len, name_raw, crc_stored, reserved) = HEADER_STRUCT.unpack_from(data, 0)

    if magic != MAGIC:
        errors.append("magic 错误：%r" % magic)
    if version != VERSION:
        errors.append("version 错误：%d" % version)
    if meta_ofs != HEADER_SIZE:
        errors.append("meta_ofs 错误：0x%02X（应为 0x40）" % meta_ofs)
    if reserved != 0:
        errors.append("头部 reserved 非 0")
    if not (1 <= name_len <= NAME_MAX):
        errors.append("name_len 越界：%d" % name_len)
    name = name_raw[:max(name_len - 1, 0)].decode("utf-8", "replace")

    # CRC32（字段置 0 计算）
    z = bytearray(data)
    struct.pack_into("<I", z, 0x38, 0)
    crc_calc = zlib.crc32(bytes(z)) & 0xFFFFFFFF
    if crc_calc != crc_stored:
        errors.append("CRC32 不匹配：存储 0x%08X，计算 0x%08X" % (crc_stored, crc_calc))

    if text_ofs != HEADER_SIZE + word_count * ENTRY_SIZE:
        errors.append("text_ofs 错误：%d（应为 %d）"
                      % (text_ofs, HEADER_SIZE + word_count * ENTRY_SIZE))
    if len(data) < text_ofs:
        errors.append("文件长度 %d 小于 text_ofs %d" % (len(data), text_ofs))
        return word_count, name, crc_stored, errors, 0

    entries = []
    for i in range(word_count):
        off = meta_ofs + i * ENTRY_SIZE
        (wid, bucket, pos, wl, pl, gl, xl, flags,
         tofs, d0, d1, d2, d3, d4, res) = ENTRY_STRUCT.unpack_from(data, off)
        entries.append({"word_id": wid, "bucket": bucket, "pos": pos,
                        "lens": (wl, pl, gl, xl), "flags": flags, "ofs": tofs,
                        "distract": [d0, d1, d2, d3, d4], "res": res})

    # word_id 连续
    for i, e in enumerate(entries):
        if e["word_id"] != i:
            errors.append("索引 %d 的 word_id=%d，不连续" % (i, e["word_id"]))
        if e["bucket"] > 9:
            errors.append("word_id=%d freq_bucket=%d 越界" % (e["word_id"], e["bucket"]))
        if e["pos"] > 10:
            errors.append("word_id=%d pos=%d 越界" % (e["word_id"], e["pos"]))
        if e["res"] != b"\x00" * 8:
            errors.append("word_id=%d 索引 reserved 非 0" % e["word_id"])

    # 长度字段与文本区逐条吻合
    for e in entries:
        wl, pl, gl, xl = e["lens"]
        base = text_ofs + e["ofs"]
        total = wl + pl + gl + xl
        if base + total > len(data):
            errors.append("word_id=%d 文本越界" % e["word_id"])
            continue
        blob = data[base:base + total]
        parts = []
        p = 0
        for ln in (wl, pl, gl, xl):
            parts.append(blob[p:p + ln])
            p += ln
        ok = True
        for seg, ln, label in zip(parts, (wl, pl, gl, xl),
                                  ("word", "phon", "gloss", "ex")):
            if ln == 0:
                if len(seg) != 0:
                    ok = False
            elif len(seg) != ln or not seg.endswith(b"\x00") or b"\x00" in seg[:-1]:
                ok = False
                errors.append("word_id=%d %s 段长度字段与实际文本不吻合" % (e["word_id"], label))
        if ok and wl > 0:
            e["word_text"] = parts[0][:-1].decode("utf-8", "replace")
        # flags.bit0 与 SOH/STX 一致性
        ex_seg = parts[3]
        has_mark = b"\x01" in ex_seg and b"\x02" in ex_seg
        if bool(e["flags"] & FLAG_EXAMPLE_HIGHLIGHT) != has_mark:
            errors.append("word_id=%d flags.bit0 与例句 SOH/STX 标记不一致" % e["word_id"])

    # 干扰项同桶校验（跨桶次数必须为 0）
    cross_bucket = 0
    buckets = {e["word_id"]: e["bucket"] for e in entries}
    for e in entries:
        for d in e["distract"]:
            if d == FILL:
                continue
            if d not in buckets:
                errors.append("word_id=%d 干扰项 %d 不是合法 word_id" % (e["word_id"], d))
                continue
            if d == e["word_id"]:
                errors.append("word_id=%d 干扰项包含自身" % d)
            if buckets[d] != e["bucket"]:
                cross_bucket += 1
                errors.append("word_id=%d 干扰项 %d 跨桶（%d vs %d）"
                              % (e["word_id"], d, e["bucket"], buckets[d]))
    return word_count, name, crc_stored, errors, cross_bucket


def cmd_generate(input_path, name, out_path):
    entries = load_input(input_path)
    words = dedup_and_sort(entries)
    build_distractors(words)
    data, text_len = build_wbx(name, words)

    tmp = out_path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, out_path)

    crc = struct.unpack_from("<I", data, 0x38)[0]
    buckets = {}
    for e in words:
        buckets[e["freq_bucket"]] = buckets.get(e["freq_bucket"], 0) + 1
    print("生成完成：%s" % out_path)
    print("  词条数      : %d（输入 %d 条，去重后 %d 条）"
          % (len(words), len(entries), len(words)))
    print("  文件大小    : %d 字节（头 64 + 索引 %d + 文本 %d）"
          % (len(data), len(words) * ENTRY_SIZE, text_len))
    print("  CRC32       : 0x%08X" % crc)
    print("  词频桶分布  : %s" % ", ".join("桶%d:%d" % (b, buckets[b])
                                        for b in sorted(buckets)))
    return True


def cmd_verify(path):
    count, name, crc, errors, cross = verify_wbx(path)
    print("自检：%s" % path)
    print("  词书名      : %s" % name)
    print("  词条数      : %d" % count)
    print("  CRC32       : 0x%08X" % crc)
    print("  跨桶干扰项  : %d" % cross)
    if errors:
        print("  校验结果    : 失败，%d 个问题" % len(errors))
        for e in errors[:50]:
            print("    - %s" % e)
        return False
    print("  校验结果    : 通过（长度字段与文本区吻合 / CRC 正确 / 干扰项同桶）")
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="墨卡 .wbx 词书生成工具（docs/wbx_format.md v1.0）")
    ap.add_argument("--input", help="输入文件（.csv 或 .jsonl）")
    ap.add_argument("--name", default="未命名词书", help="词书名（UTF-8，含\\0 ≤32B）")
    ap.add_argument("--output", help="输出 .wbx 路径")
    ap.add_argument("--verify", metavar="WBX",
                    help="自检已有 .wbx；与 --output 同用时生成后立即自检")
    args = ap.parse_args(argv)

    ok = True
    try:
        if args.output:
            if not args.input:
                ap.error("--output 需要同时指定 --input")
            ok = cmd_generate(args.input, args.name, args.output)
        if args.verify:
            ok = cmd_verify(args.verify) and ok
        if not args.output and not args.verify:
            ap.error("需要 --output（生成）或 --verify（自检）至少一项")
    except GenError as e:
        print("错误：%s" % e, file=sys.stderr)
        return 1
    except FileNotFoundError as e:
        print("错误：文件不存在：%s" % e.filename, file=sys.stderr)
        return 1
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
