# abhpc-lbs

中文 | [English](README.en.md)

根据节点历史负载调整 Slurm `Weight` 的 C 程序，支持 EL7 和 EL8。
通过 `scontrol` 一次读取全部节点的负载、CPU 数和权重，再按相同权重批量更新。
负载无效或计算失败的节点保留原权重。

## 编译与运行

构建需要 GCC、Make，测试需要 Python 3.6+。运行只需要 Slurm 的 `scontrol`，
无需 SSH 登录计算节点。

运行用户须有 Slurm 节点管理权限。
自定义 Slurm 安装可在配置中指定 `scontrol_path`；`SLURM_CONF` 和认证环境按集群设置。

```bash
make
make test

# 预览计算结果
./abhpc-lbs --config ./abhpc-lbs.conf --dry-run

# 执行一次权重更新
./abhpc-lbs --config ./abhpc-lbs.conf
```

## 配置

默认读取 `/etc/abhpc-lbs.conf`，每次运行重新加载，修改后下次生效。
默认文件不存在时使用内置值；用 `--config` 指定的文件必须存在。

```ini
scontrol_path = scontrol
formula = 0.75 * 100 * load / cpus + 0.25 * weight
min_weight = 0
max_weight = 100
```

`scontrol_path = scontrol` 默认从 `PATH` 查找命令。非标准安装填写可执行文件的
绝对路径，不加引号，例如：

```ini
scontrol_path = /opt/slurm/bin/scontrol
```

查询和更新均使用该路径，定时器同样生效。路径不存在或无法执行时直接报错。

| 变量 | 含义 |
| --- | --- |
| `load` | Slurm 控制器记录的节点 `CPULoad`，与 `sinfo %O` 同源 |
| `cpus` | Slurm `CPUTot`，可能包含超线程 |
| `weight` | 本轮查询到的节点权重，用于历史平滑 |

公式支持数字、`+ - * /`、括号和正负号，结果按权重上下限截断后取整。
配置支持 `#` 注释，公式不加引号。命令行参数优先于配置文件：

```bash
./abhpc-lbs -c ./abhpc-lbs.conf \
  --formula '0.5 * 100 * load / cpus + 0.5 * weight' \
  --dry-run
```

`load` 是最近一次上报值，查询不会触发即时采样，可能有延迟；更新受 Slurm
上报机制和 `SlurmdTimeout` 等配置影响。核对的 Slurm Linux 实现取 5 分钟
负载均值，见 [Slurm 负载采集源码](https://github.com/SchedMD/slurm/blob/slurm-22-05-11-1/src/slurmd/slurmd/get_mach_stat.c)。
负载均值不等于 CPU 使用率。历史平滑由公式中的 `weight` 完成，程序不读取
作业记账历史。5 分钟是负载的统计时间尺度，执行周期由定时器或 cron 设置；
调整执行周期时，应一起评估平滑系数。

升级时将旧公式中的 `load1`、`load5`、`load15` 改为 `load`，并重新评估系数。
原先的 15 分钟负载与此值口径不同。删除旧配置中的 `fanout`、`connect_timeout`、
`command_timeout`，调用参数中也不再使用 `-j`。RPM 会保留已有配置，需手动迁移；
未迁移的配置会报错退出，不会静默更换公式含义。

## RPM

在对应系统安装构建依赖，再执行 `make rpm`：

```bash
# EL7
sudo yum install gcc make python3 rpm-build
make rpm
```

```bash
# EL8
sudo dnf install gcc make python3 rpm-build
make rpm
```

也可以通过 Docker 分别构建，两个目标均会运行测试：

```bash
make rpm-el7
make rpm-el8
```

| 构建方式 | 二进制包目录 | 源码包目录 |
| --- | --- | --- |
| 本机构建 | `build/rpm/RPMS/` | `build/rpm/SRPMS/` |
| EL7 容器 | `build/rpm-el7/RPMS/` | `build/rpm-el7/SRPMS/` |
| EL8 容器 | `build/rpm-el8/RPMS/` | `build/rpm-el8/SRPMS/` |

EL7 和 EL8 应分别构建、安装对应的包，避免高版本 glibc 依赖。
安装前需准备好 Slurm 客户端：

```bash
# EL7
sudo yum install ./build/rpm-el7/RPMS/*/abhpc-lbs-1.0.0-2.el7.*.rpm
# EL8
sudo dnf install ./build/rpm-el8/RPMS/*/abhpc-lbs-1.0.0-2.el8.*.rpm
```

EL7 容器使用 [CentOS Vault](https://vault.centos.org/7.9.2009/) 和
[EPEL 7 归档](https://archives.fedoraproject.org/pub/archive/epel/7/x86_64/)。
RPM 不限制 Slurm 的安装路径；已有手动安装的 Slurm 可直接通过配置指定。
Python 仅用于测试，不是运行依赖。

## 定时执行

源码安装用 `sudo make install`；RPM 安装也会提供定时器，均保留已有配置。
安装后手动启用：

```bash
sudo systemctl daemon-reload
sudo systemctl enable abhpc-lbs.timer
sudo systemctl start abhpc-lbs.timer
systemctl list-timers abhpc-lbs.timer
journalctl -u abhpc-lbs.service
```

默认以 root 运行，开机一分钟后首次执行，每轮结束一分钟后再执行。
修改周期用 `systemctl edit abhpc-lbs.timer`，例如改为五分钟：

```ini
[Timer]
OnUnitInactiveSec=
OnUnitInactiveSec=5min
```

然后执行 `systemctl daemon-reload` 和 `systemctl restart abhpc-lbs.timer`。
运行用户或环境通过 `systemctl edit abhpc-lbs.service` 设置。也可使用 cron，
只启用一个调度入口，避免重复执行。

## 注意

- 权重只影响后续调度，不迁移运行中的作业。其他条件相同时，Slurm 优先选择
  较低权重的节点，见 [Slurm Weight 定义](https://slurm.schedmd.com/slurm.conf.html#OPT_Weight)。
- 节点处于离线、未响应及电源切换等状态时，跳过并保留旧权重，不影响本轮成功状态。
  在线节点的负载或状态字段无效、公式计算失败时，保留该节点权重并返回非零状态。
  批次写入失败则停止后续更新；已更新的节点不回滚。
- Slurm 重启或重新配置可能恢复节点权重，下一轮按实际查询值计算。
  上线前先用 `--dry-run` 核对结果。

本项目采用 [MIT 许可证](LICENSE)。
