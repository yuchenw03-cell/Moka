#!/usr/bin/env python3
# cet4_convert.py —— 把有道风格 CET4_2.json（每行一个嵌套 JSON）转成
# wbx_gen.py 期望的平铺 JSONL：word/phonetic/pos/gloss/example/freq_bucket
#
# 字段映射：
#   word       <- headWord
#   phonetic   <- content.word.content.usphone（加 // 包裹）
#   pos        <- trans[0].pos，映射到 wbx_gen 枚举，未知归为 0
#   gloss      <- 全部 trans 拼成 "pos. 释义；pos. 释义"
#   example    <- 优先 sentence.sentences[0]，退回 realExamSentence[0]
#   freq_bucket<- wordRank 十分位（rank 越小词频越高，桶 0 最高频）
import json
import sys

POS_ALIAS = {
    "n": "n", "v": "v", "vt": "v", "vi": "v",
    "adj": "adj", "a": "adj",
    "adv": "adv", "ad": "adv",
    "prep": "prep", "conj": "conj", "pron": "pron",
    "num": "num", "abbr": "abbr", "phr": "phr",
    "art": "abbr", "int": "abbr",
}

def convert(src, dst):
    with open(src, encoding="utf-8") as f:
        lines = [l for l in f if l.strip()]
    total = len(lines)
    out = []
    skipped = 0
    for line in lines:
        try:
            d = json.loads(line)
            c = d["content"]["word"]["content"]
            word = d["headWord"].strip()
            if not word:
                skipped += 1
                continue
            phone = c.get("usphone") or c.get("ukphone") or ""
            phonetic = "/%s/" % phone.strip("/ ") if phone else ""
            trans = c.get("trans") or []
            pos_raw = (trans[0].get("pos", "") if trans else "").lower()
            pos = POS_ALIAS.get(pos_raw, 0)
            gloss = "；".join(
                "%s. %s" % (t.get("pos", ""), t.get("tranCn", "").strip())
                for t in trans if t.get("tranCn"))
            example = ""
            sents = (c.get("sentence") or {}).get("sentences") or []
            if not sents:
                sents = (c.get("realExamSentence") or {}).get("sentences") or []
            if sents:
                example = sents[0].get("sContent", "").strip()
            rank = int(d.get("wordRank", total))
            bucket = min(9, (rank - 1) * 10 // total)
            out.append({
                "word": word, "phonetic": phonetic, "pos": pos,
                "gloss": gloss, "example": example, "freq_bucket": bucket,
            })
        except Exception as e:
            skipped += 1
            print("跳过行（%s）" % e, file=sys.stderr)
    with open(dst, "w", encoding="utf-8") as f:
        for r in out:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    print("转换完成：%d 词（跳过 %d）→ %s" % (len(out), skipped, dst))

if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2])
