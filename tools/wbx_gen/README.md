# wbx_gen — 墨卡 .wbx 词书生成工具

将 CSV / JSONL 词表编译为固件端使用的 `.wbx` 二进制词书，格式逐字节遵循
[`docs/wbx_format.md`](../../docs/wbx_format.md) v1.0：
64B 文件头 + `word_count × 48B` 定长索引 + 文本区（`word\0 phon\0 gloss\0 ex\0`
四段连续，索引 `text_ofs` 为相对文本区起点的偏移），CRC32 最后回填（字段自身置 0 计算）。

纯 Python 3.9+ 标准库实现（argparse / json / csv / struct / zlib / unicodedata / difflib），
无任何第三方依赖。

## 用法

```sh
# 生成
python3 wbx_gen.py --input sample.jsonl --name "考研核心" --output sample.wbx

# 单独自检已有 .wbx
python3 wbx_gen.py --verify sample.wbx

# 生成后立即自检（等价于上面两条）
python3 wbx_gen.py --input sample.jsonl --name "考研核心" --output sample.wbx --verify sample.wbx
```

生成的 `.wbx` 拷到 SD 卡 `/wordbook/` 目录供固件使用。

## 输入格式

每行 / 每条记录一个词条，字段：`word, phonetic, pos, gloss, example, freq_bucket`。

- **JSONL**（`.jsonl`）：每行一个 JSON 对象，键名同上。
- **CSV**（`.csv`）：首行表头含 `word` 时按列名取值；无表头按上述顺序取前 6 列。
- `pos`：枚举名 `n / v / adj / adv / prep / conj / pron / num / abbr / phr`（或数字 0–10，0=未知）。
- `freq_bucket`：0–9（COCA 词频分桶，0 最高频；脚本只校验范围，不负责算词频）。
- `phonetic` / `example` 可为空（对应长度字段写 0）。

## 处理规则

1. **去重**：`word` 大小写不敏感去重（保留首见），按小写词形排序，`word_id` 从 0 连续编号。
2. **干扰项**：在**同 freq_bucket** 的其余词中抽最多 5 个，加权得分 =
   `2.0 × difflib.SequenceMatcher.ratio()`（编辑距离近）
   `+ 1.0`（首字母相同）`+ 1.0`（同词性），按得分降序取前 5；
   桶内不足 5 个补 `0xFFFFFFFF`（固件端跳过空槽）。**绝不跨桶**（验收硬指标，
   `--verify` 会逐条校验并报告跨桶数）。
3. **例句高亮**：例句中目标词形（大小写不敏感，字母边界）用 `0x01(SOH)`/`0x02(STX)`
   包裹并置 `flags.bit0`；找不到目标词形则原样写入、`flags=0`。
4. **CRC32**：`zlib.crc32`，将头部 `crc32` 字段（0x38）置 0 后对全文件计算并回填。

## 自检内容（--verify）

- 文件头 magic / version / meta_ofs / text_ofs / 保留字段；
- CRC32 重算比对；
- `word_id` 从 0 连续、freq_bucket ≤ 9、pos ≤ 10；
- 每条索引的 4 个长度字段与文本区实际分段逐字节吻合（防生成错位）；
- `flags.bit0` 与例句 SOH/STX 标记一致；
- 干扰项全为合法 word_id、非自身、且与主词同桶（跨桶数必须为 0）。

任何一项失败即非零退出并逐条列出问题。

## 文件

| 文件 | 说明 |
|---|---|
| `wbx_gen.py` | 生成器 / 自检器 |
| `sample.jsonl` | 30 行示例输入（考研词汇，含 1 行大小写重复演示去重，桶 2/3/4 各 8–9 词保证干扰项池） |
| `sample.wbx` | 由 sample.jsonl 生成的词书（29 词条，自检通过） |
