# .wbx 词书二进制格式定义 v1.0

> 全部小端序（ESP32 Xtensa 为 LE，与 x86 PC 一致，无需字节序转换）。
> 设计目标：固定长度索引 + 可变长度文本区，固件端用偏移直接寻址，
> 不需要全量载入内存；干扰项索引预生成，固件端零计算。

## 1. 文件布局

```
+------------------------+ 0x0000
│ 文件头 (64 B)          │
+------------------------+ 0x0040
│ 词条索引区             │ word_count × 48 B
│ (固定长度，可直接 mmap │
│  /fread 定点读)        │
+------------------------+ meta_ofs + word_count*48
│ 文本区                 │ word / phon / gloss / example
│ (UTF-8, \0 结尾,       │ 四段连续存放，索引区 text_ofs
│  按 word_id 升序)      │ 指向本区内相对偏移
+------------------------+
```

## 2. 文件头（64 字节）

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0x00 | 4 | magic | "WBX1" |
| 0x04 | 4 | version | u32，当前 = 1 |
| 0x08 | 4 | word_count | 词条总数 |
| 0x0C | 4 | meta_ofs | 索引区偏移（固定 0x40） |
| 0x10 | 4 | text_ofs | 文本区偏移 |
| 0x14 | 4 | name_len | 词书名长度（含\0，≤32） |
| 0x18 | 32 | name | 词书名（UTF-8，如 "考研核心 5500"） |
| 0x38 | 4 | crc32 | 全文件（本字段置 0 后计算 CRC32） |
| 0x3C | 4 | reserved | 置 0 |

## 3. 词条索引项（48 字节定长）

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0x00 | 4 | word_id | 词条唯一 ID（本词书内 0..N-1，跨书不保证唯一） |
| 0x04 | 1 | freq_bucket | 词频桶 0-9（COCA 分桶，0 最高频） |
| 0x05 | 1 | pos | 词性枚举：0=未知 1=n 2=v 3=adj 4=adv 5=prep 6=conj 7=pron 8=num 9=abbr 10=phr |
| 0x06 | 2 | word_len | 单词字符串长度（含\0，≤48） |
| 0x08 | 2 | phon_len | 音标长度（含\0，可为 0=无音标） |
| 0x0A | 2 | gloss_len | 释义长度（含\0） |
| 0x0C | 2 | ex_len | 例句长度（含\0，可为 0） |
| 0x0E | 2 | flags | bit0=例句中含反色高亮标记（见下） |
| 0x10 | 4 | text_ofs | 相对文本区起点的偏移，指向 word 段起点；四段连续：word\0 phon\0 gloss\0 ex\0 |
| 0x14 | 20 | distract[5] | u32 ×5：干扰项 word_id，**必须与主词同 freq_bucket**（生成端保证）；不足 5 个填 0xFFFFFFFF |
| 0x28 | 8 | reserved | 置 0 |

**例句高亮约定**：例句中目标词用 ASCII 0x01（SOH）/0x02（STX）包裹，
渲染时 SOH..STX 区间反色。为什么不用特殊 Unicode？——位图字体的字符集
由生成端控制，控制字符不占字形位宽，且 C 端按字节扫描最快。

## 4. C 结构体定义（main/src/srs 不依赖；供 hal/app 使用）

```c
/* wbx_format.h —— .wbx 格式与磁盘字节序完全一致，仅允许 u32/u16/u8 成员，
   用 #pragma pack(1) 保证无填充；读取时 fread 进栈上 48B 缓冲即可。 */
#pragma pack(push, 1)
typedef struct {
    char     magic[4];       /* "WBX1" */
    uint32_t version;        /* = 1 */
    uint32_t word_count;
    uint32_t meta_ofs;       /* 固定 0x40 */
    uint32_t text_ofs;
    uint32_t name_len;
    char     name[32];
    uint32_t crc32;          /* 本字段置 0 后计算 */
    uint32_t reserved;
} wbx_header_t;              /* 64 B */

typedef struct {
    uint32_t word_id;
    uint8_t  freq_bucket;    /* 0-9 */
    uint8_t  pos;            /* 词性枚举 */
    uint16_t word_len;
    uint16_t phon_len;
    uint16_t gloss_len;
    uint16_t ex_len;
    uint16_t flags;          /* bit0 = 例句含高亮标记 */
    uint32_t text_ofs;       /* 相对文本区起点 */
    uint32_t distract[5];    /* 同桶干扰项；0xFFFFFFFF = 空槽 */
    uint8_t  reserved[8];
} wbx_entry_t;               /* 48 B */
#pragma pack(pop)
```

## 5. Python 生成脚本需求（tools/wbx_gen/wbx_gen.py）

**输入**：CSV 或 JSONL，每行一条：`word, phonetic, pos, gloss, example, freq_bucket`
（freq_bucket 由 COCA 词频分桶；生成脚本不负责算词频，只校验范围）。

**处理要求**：
1. 按 word 去重并按 word_id 排序；word_id 从 0 连续编号；
2. 干扰项池：对每个词，在**同 freq_bucket** 的其余词中按"编辑距离近 / 首字母相同 / 同词性"
   加权抽 5 个，写入 distract[]；桶内不足 5 个则补 0xFFFFFFFF（固件端跳过空槽，
   模式 B 选项不足时降级为 2~3 选项而非跨桶——NFR/验收要求跨桶次数为 0）；
3. 例句中目标词（大小写不敏感匹配词形）插入 SOH/STX 包裹，置 flags.bit0；
4. 输出 .wbx：文件头 + 索引区 + 文本区，最后回填 CRC32（zlib.crc32）；
5. 自检：读回验证每条索引的长度字段与实际文本区吻合（防止生成错位）。

**命令行**：`python3 wbx_gen.py --input cet6.jsonl --name "六级核心" --output cet6.wbx`

**放置**：生成的 .wbx 拷到 SD 卡 `/wordbook/` 目录。
