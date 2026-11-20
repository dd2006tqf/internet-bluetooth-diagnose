# journald 把含 CR 的消息存成字节数组，grep 出来是 `[74B blob data]`

> 发现于：2026-11-18
> 位置：云端 `journalctl -u weaknet-tunnel-guard`；任何会打印 SSH banner 的日志
> 影响：一次统计里把 **2284 条「健康」读成 0 条**，几乎据此得出「隧道从来没健康过」的反向结论

## 现象

隧道守护每 60s 打一行判定，其中健康时打：

```
健康（banner: SSH-2.0-OpenSSH_8.4p1 Debian-5+deb11u6\r）
```

注意行尾那个 **`\r`**（CR，回车）。它来自 SSH 协议本身：
RFC 4253 规定识别串（identification string）以 CR LF 结尾，
守护把读到的整行原样打进日志，于是 `\r` 被带进了 message。

于是统计：

```bash
$ sudo journalctl -u weaknet-tunnel-guard --since '7 days ago' | grep -c 健康
0
```

**0 条。** 而同一条命令 grep `未监听` 能数出 7702 条，
`半死` 能数出 45 条。看起来就像「守护跑了 7 天，一次都没健康过」——
这个结论会彻底改变修复方向（会去查「为什么从来没连上」而不是「为什么不恢复」）。

换 `-o cat` 也一样，因为问题不在渲染格式，在存储类型。

## 成因

journald 支持两种 payload 类型：

- `MESSAGE=文本`（正常文本）
- `MESSAGE` 为**字节数组**（当内容含 NUL 或其它控制字符时）

`\r` 是控制字符（0x0D）。一旦 message 里出现它，journald 就把整个
MESSAGE 字段存成字节数组，`journalctl` 在默认 / `-o short` / `-o cat` 下
无法把它渲染成文本，只输出占位符：

```
[74B blob data]
```

`grep 健康` 自然一条都匹配不到 —— **字节数组里没有「健康」这两个 UTF-8 字节的可见形态**。

关键点是：**它不报错**。`journalctl` 正常退出、行数正常、占位符看起来
就像一条普通的不可读日志。没有任何信号提示「你正在丢数据」。

## 修法

用 `-o json` 拿结构化输出，把 `MESSAGE` 当字节数组解码：

```bash
sudo journalctl -u weaknet-tunnel-guard --since '7 days ago' -o json \
  | python3 -c '
import sys, json
n = 0
for line in sys.stdin:
    m = json.loads(line).get("MESSAGE")
    if isinstance(m, list):
        m = bytes(m[1:]).decode("utf-8", "replace")   # m[0] 是 payload 长度
    if "健康" in m:
        n += 1
print(n)
'
```

注意 `bytes(m[1:])`：JSON 里的字节数组第一个元素是**长度**，不是数据。
直接 `bytes(m)` 会在首位多一个字节，虽然不影响中文匹配，但边界处会错。

改完统计出 **2284 条健康**（占 7 天约 22.7%）。整个修复方案的判决依据
（「45 次半死只有 8 次自愈，37 次需人工」）正是建立在这份修正后的数据上。

## 边界

- 这个坑只在「日志内容含控制字符」时触发。普通日志不受影响，
  所以它藏得很深：同一个 unit 里 `未监听` / `半死` 都正常可 grep，
  只有 `健康` 那一行是 blob，**看起来像「这一类消息不存在」而不是「这一类消息不可读」**。
- 不限于 `\r`。任何 NUL / 控制字符（例如自己拼的二进制调试输出）都会触发。
- 根治办法是从源头去掉控制字符（守护打印 banner 时 `tr -d '\r'`）。
  `tools/tunnel_guard.sh` 目前未改这一点，因为改日志格式会让**已有的 7 天
  历史数据**与未来数据口径不一致。留给后续统一处理。

## 教训

**统计结果为 0 时，先怀疑统计手段，再怀疑被统计的事实。**
尤其是「某个类别的计数恰好是 0，而相邻类别都有几千条」这种形状——
它更像是那一类被特殊处理了，而不是真的没发生。

具体到日志：`grep` / `journalctl` 的**默认渲染**不是无损的。
在做任何「历史计数」结论前，先用 `-o json` 抽一条样本看看 `MESSAGE`
的类型是字符串还是数组。这一步只花 10 秒，却能避免整个方向走反。

## 关联

- [[journal-log-date-attribution]] — 同族：日志统计口径没验证就下结论
- [[ssh-n-silent-so-logs-are-not-a-criterion]] — 同一个诊断里，「日志静默」的另一种误读
- `openspec/tasks/2026-11-18-tunnel-process-disappear-diagnosis.md` — 本坑的完整上下文
