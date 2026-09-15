# 测试与验收范围（0.6.5）

安装 Demo 依赖后，在项目根目录 MSYS2 shell 运行；Python 指向对应虚拟环境：

```sh
make test reference-test sim
python tests/test_rbp_python.py
python tests/test_worker_races.py
python tests/test_serial_transport.py
python tests/test_srv_budget.py
python tests/test_e2e.py
python tests/test_demo.py
python tests/test_diag.py
python protocol/v3/check_contract.py
python tools/check_design.py
```

这些回归不用实际 COM 口。模拟测试不能代替射频/音质验收。test_reference_audio.py 另需本地完整 Telink 参考源码，不是干净 checkout 的必要依赖。

0.6.5 已刷板并读取真实原始错误，用户确认 GUI 使用正常；0.6.4 清空后手动配对和按键通过。此前版本验证语音、约60秒录音、弱信号重连和上电恢复。未声明 0.6.5 重跑全部历史场景，也未完成 macOS 或其他遥控器真机验收。

改动 BLE/USB 生命周期时回归：扫描选择配对、清绑定、重启恢复、按键、短/长录音、离开覆盖再返回、USB关闭重开、主机拥堵、掉线中断录音和原始错误采集。自动续录不在范围内。
