# docs

这里是两个引擎的**原始设计文档**，写于项目立项阶段。

| 文件 | 对应 |
|---|---|
| `design-orderbook.md` | `orderbook_simulator/` |
| `design-backtest.md` | `backtest_engine/` |

> ⚠️ 这两份是**设计稿**，不是当前实现的准确描述。实现后来走得比设计稿远——比如回测引擎的策略从设计稿里的 2 个长到了 10 个，还多出了组合回测、外部信号回放、风控模块和市场规则查表。
>
> **接口和行为的准确描述请看各子项目的 README**：
> [`backtest_engine/README.md`](../backtest_engine/README.md) · [`orderbook_simulator/README.md`](../orderbook_simulator/README.md)
