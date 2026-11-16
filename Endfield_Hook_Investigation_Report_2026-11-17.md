# Endfield 真机调试报告 — AppInit 全局 Hook × ACE 反作弊

日期：2026-11-17
分支：Endfield-patch（基线提交 `925c088d2`）
目标游戏：明日方舟：终末地（`E:\games\Arknights Endfield\Endfield.exe`，Unity IL2CPP + Qt 壳，ACE 反作弊）

## 1. 背景与目标

上一里程碑实现了全局 hook 分离安装模式（`startglobalhook --detach` 安装即退出、`stopglobalhook` 恢复），
并通过了良性 D3D11 程序的完整回归（D_CAP=1、注册表恢复原值、零残留）。
本轮目标：在真实游戏 Endfield 上验证分离模式注入，确认 ACE 环境下能否抓帧。

## 2. 实验时间线与结果

| # | 实验 | 结果 | 结论 |
|---|---|---|---|
| E1 | Release 目录安装 detach hook（`--match Endfield.exe -c E:\caps\endfield`），良性 D3D11 程序验证 | 抓帧成功（36KB rdc） | 注入机制正常 |
| E2 | 同一安装，用户正常启动 Endfield，游戏内停留 5 分钟 | 无 overlay、无 `endfield.rdoc.log` 调试日志、无抓帧 | 游戏内注入失败 |
| E3 | 改名对照：`d3d11_test.exe` → `Endfield.exe`（普通用户权限进程） | 注入 + 抓帧成功 | 排除权限/8.3 短路径/匹配逻辑问题，链路健康 |
| E4 | 游戏目录部署（复制 renderdoc.dll/renderdoccmd.exe/renderdocshim64.dll 进游戏目录并从该目录重装 detach hook） | 游戏外探针（`Endfield.exe.probe.exe`）注入+抓帧成功；游戏内仍无 overlay | 链路依然健康；ACE 嫌疑上升 |
| E5 | shim 加 trace instrumentation（进程路径含 endfield 时写 `E:\caps\shim_trace.log`） | 游戏子进程 `UnityCrashHandler64.exe` 留下完整 trace（加载 shim→读配置→no match 跳过）；**主进程 Endfield.exe 零 trace** | ACE 按进程选择性拦截，shim 在主进程内从未执行 |
| E6 | ProcMon 内核事件抓取（动态调试方案） | ACE 弹"检测到不兼容程序" | 本机 ACE 环境下 ProcMon 不可作为辅助工具 |
| E7 | dxgi.dll 代理（转发全部 19 个 dxgi 导出到 System32 + 从游戏目录加载 renderdoc.dll） | 游戏外自测（`dxgitest.exe`）成功抓帧；游戏内无 overlay | 代理机制可行，但未 instrumentation，无法区分"游戏未加载代理"与"renderdoc 加载被拦" |
| E8 | 完全停止（`stopglobalhook` + dxgi.dll 改名解除武装） | 注册表恢复基线，游戏目录恢复原生 | 系统状态干净 |

## 3. 关键结论

1. **AppInit 注入链在游戏外 100% 可用**——注册表安装、shim 加载、配置读取、匹配、renderdoc.dll 加载、
   抓帧全流程经三次对照实验（E1/E3/E4 探针）验证无误。
2. **Endfield.exe 主进程被 ACE 内核级保护**，具体表现：
   - 外部模块枚举被致盲（`Get-Process .Modules` 返回 0、`Path` 为空，仅 StartTime 可读）——内核回调剥离句柄权限的典型特征；
   - instrumented shim 在主进程内**零 trace**（连第一行都没写）——shim 的 DllMain/CreateThread 在该进程内被拦截或线程被杀；
   - 子进程 `UnityCrashHandler64.exe` 不受保护，shim 正常加载并执行。
3. **ACE 组件构成**（游戏目录 AntiCheatExpert/）：ACE-BASE.sys + ACE-CORE.sys 内核驱动、
   ACE-Service64.exe 服务、ACE-Base64.dll 注入游戏进程——具备进程级模块拦截能力，与观察吻合。
4. **dxgi 代理路线未被证伪**：dxgi 不在本机 KnownDLLs 列表（35 项，无 dxgi/d3d），
   游戏目录 dxgi.dll 理论上会被加载器优先解析；游戏内失败原因待 instrumentation 定位。
5. ACE 对本机的互斥名单至少包含 ProcMon（E6 唯一触发弹窗的工具）。

## 4. 当前系统状态（已全部清理）

| 项 | 状态 |
|---|---|
| 注册表 AppInit_DLLs / LoadAppInit_DLLs / RequireSignedAppInit_DLLs | 恢复原值（空 / 0x0 / 0x1） |
| WER LocalDumps 调试键 | 已不存在（查询确认，无需清理） |
| 游戏目录 | 已删除全部 6 个部署文件（dxgi.dll.off、renderdoc.dll、renderdoccmd.exe、renderdocshim64.dll、dxgitest.exe、Endfield.exe.probe.exe） |
| E:\caps | 已清空（含 2.1GB ProcMon 抓取 eftrace.pml） |
| 游戏目录 hook 配置/状态 bin | stopglobalhook 已删除（无 LEFTOVER） |

保留工具集 `E:\Tools\rdoc-test\`：测试脚本（install/stop/check/watch）、源码
（`dxgi.cpp` 代理、`d3d11_test.cpp`）、构建产物（`dxgi.dll`、`d3d11_test.exe` 等），供下一轮复用。

## 5. 下一步建议（proxy v2 — 一次游戏重启定位拦截点）

1. **给 dxgi.cpp 加 instrumentation**（照抄 shim trace 模式）：
   - DllMain 写 trace（证明游戏是否加载了游戏目录的 dxgi.dll）
   - LoadLibraryW(renderdoc.dll) 结果 + GetLastError 写 trace（证明进程内加载是否被 ACE 拦）
2. 若"游戏未加载代理"：检查游戏的 dxgi 解析方式（绝对路径/驱动链），改用其他劫持名（d3d11.dll、winmm.dll、version.dll）
3. 若"renderdoc 加载被拦"：实测 `renderdoccmd inject <pid>`（CreateRemoteThread 向量，游戏运行时注入）
4. 备选信息源：游戏自带日志（sdklogs/HGEventLog.log、CrashSightLog）暂无拦截记录，可作为旁证持续观察

## 6. 附：关键命令记录

```bat
:: 分离安装（从部署目录运行，shim/renderdoc 所在目录决定注册表指向）
renderdoccmd.exe startglobalhook --detach --match Endfield.exe -c E:\caps\endfield

:: 停止并恢复（必须从同一部署目录运行，状态文件在 renderdoc.dll 旁）
renderdoccmd.exe stopglobalhook

:: 对照实验（游戏目录内，普通用户权限即可）
copy d3d11_test.exe "E:\games\Arknights Endfield\Endfield.exe.probe.exe"   :: 命中 match → 应注入成功

:: dxgi 代理构建（vcvars64 环境下）
cl /nologo /O2 /LD /MT dxgi.cpp /Fe:dxgi.dll
```

注意：instrumented shim（写 `E:\caps\shim_trace.log` 的版本）目前只存在于源码
`renderdocshim\renderdocshim.cpp`；`x64\Release\renderdocshim64.dll` 为该版本构建产物，
正式发布前可考虑移除 trace 或改为按需开启。
