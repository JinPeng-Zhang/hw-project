# NS-3.45 项目仓库
hw project 11/29

替换文件 ns3.45/src/internet/model/ipv4-global-routing.cc和ns3.45/src/point-to-point/model/point-to-point-net-device.cc

一、PFC 功能集成指南

1.1 替换链路创建方式

原有代码（标准 P2P）：

```cpp
PointToPointHelper p2p;
p2p.SetDeviceAttribute("DataRate", StringValue("10Gbps"));
p2p.SetChannelAttribute("Delay", StringValue("1ms"));
p2p.Install(node1, node2);
```

修改为（支持 PFC）：

```cpp
#include "pfc/qbb-point-to-point-helper.h"

QbbPointToPointHelper qbb;
qbb.SetDeviceAttribute("DataRate", StringValue("10Gbps"));
qbb.SetChannelAttribute("Delay", StringValue("1ms"));

// PFC 配置
qbb.SetPfcAttribute("PfcEnable", BooleanValue(true));
qbb.SetPfcAttribute("PfcHighPkts", UintegerValue(8));   // 高水位阈值
qbb.SetPfcAttribute("PfcLowPkts", UintegerValue(4));    // 低水位阈值
qbb.SetPfcAttribute("DefaultQuanta", UintegerValue(65535)); // 暂停时长

qbb.Install(node1, node2);
```

1.2 全局 PFC 参数配置（可选）

在创建拓扑**之前**通过 `Config::SetDefault` 统一配置：

```cpp
// 在 main 函数开头
Config::SetDefault("ns3::QbbNetDevice::PfcEnable", BooleanValue(true));
Config::SetDefault("ns3::QbbNetDevice::PfcHighPkts", UintegerValue(12));
Config::SetDefault("ns3::QbbNetDevice::PfcLowPkts", UintegerValue(6));
```

1.3 运行时动态调整（推荐）

参考 `experiments.cc` 中的 `ApplyPfcConfig` 函数：

```cpp
void ApplyPfcConfig(bool enable, uint32_t highPkts, uint32_t lowPkts) {
  Config::Set("/NodeList/*/DeviceList/*/$ns3::QbbNetDevice/PfcEnable", 
              BooleanValue(enable));
  Config::Set("/NodeList/*/DeviceList/*/$ns3::QbbNetDevice/PfcHighPkts", 
              UintegerValue(highPkts));
  Config::Set("/NodeList/*/DeviceList/*/$ns3::QbbNetDevice/PfcLowPkts", 
              UintegerValue(lowPkts));
}

// 在拓扑创建后、仿真运行前调用
ApplyPfcConfig(true, 8, 4);
```

1.4 PFC 关键参数说明

| 参数              | 含义                   | 推荐值             |
| --------------- | -------------------- | --------------- |
| `PfcEnable`     | 是否启用 PFC             | true            |
| `PfcHighPkts`   | 高水位阈值（包数）            | 8-12（根据 BDP 调整） |
| `PfcLowPkts`    | 低水位阈值（包数）            | 高水位的 50%        |
| `DefaultQuanta` | XOFF 暂停时长（bit-times） | 65535（最大值）      |


二、ECMP 功能集成指南

2.1 ECMP 模式配置

参考 `experiments.cc` 中的实现：

```cpp
void ApplyEcmpConfig(const std::string& mode) {
  bool perflow   = (mode == "perflow");   // 每条流固定路径
  bool perpacket = (mode == "perpacket"); // 每个包随机路径

  // 方法1：全局默认配置（在创建拓扑前）
  Config::SetDefault("ns3::Ipv4GlobalRouting::PerflowEcmpRouting", 
                     BooleanValue(perflow));
  Config::SetDefault("ns3::Ipv4GlobalRouting::RandomEcmpRouting", 
                     BooleanValue(perpacket));

  // 方法2：运行时配置（在创建拓扑后）
  std::string base = "/NodeList/*/$ns3::Ipv4L3Protocol/"
                     "$ns3::Ipv4ListRouting/$ns3::Ipv4GlobalRouting/";
  Config::Set(base + "PerflowEcmpRouting", BooleanValue(perflow));
  Config::Set(base + "RandomEcmpRouting", BooleanValue(perpacket));
}
```

2.2 强制运行时生效（重要）

由于 ECMP 属性可能在节点创建时已固化，需要强制刷新：

```cpp
void ForceEcmpConfigRuntime(const std::string& mode) {
  bool perflow   = (mode == "perflow");
  bool perpacket = !perflow;

  for (uint32_t i = 0; i < NodeList::GetNNodes(); ++i) {
    Ptr<Node> node = NodeList::GetNode(i);
    Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
    if (!ipv4) continue;

    Ptr<Ipv4ListRouting> list = 
        DynamicCast<Ipv4ListRouting>(ipv4->GetRoutingProtocol());
    if (!list) continue;

    for (uint32_t k = 0; k < list->GetNRoutingProtocols(); ++k) {
      int16_t prio;
      Ptr<Ipv4GlobalRouting> gr = 
          DynamicCast<Ipv4GlobalRouting>(list->GetRoutingProtocol(k, prio));
      if (!gr) continue;

      gr->SetAttribute("PerflowEcmpRouting", BooleanValue(perflow));
      gr->SetAttribute("RandomEcmpRouting", BooleanValue(perpacket));
    }
  }
}
```

2.3 完整使用流程

```cpp
// 1. 在 main 函数中配置
std::string ecmpMode = "perflow"; // 或 "perpacket"

// 2. 构建拓扑（使用你们自己的拓扑代码）
YourTopology topo;

// 3. 安装协议栈和分配 IP（你们自己的实现）
// ...

// 4. 应用 ECMP 配置
ApplyEcmpConfig(ecmpMode);
ForceEcmpConfigRuntime(ecmpMode);

// 5. 填充路由表
Ipv4GlobalRoutingHelper::PopulateRoutingTables();

// 6. 安装流量
// ...

// 7. 运行仿真
Simulator::Run();
Simulator::Destroy();
```

---

三、完整集成示例模板

```cpp
#include <ns3/core-module.h>
#include <ns3/internet-module.h>
#include <ns3/ipv4-global-routing-helper.h>

// 引入你提供的头文件
#include "pfc/qbb-point-to-point-helper.h"
#include "pfc/qbb-net-device.h"

using namespace ns3;

// ========== PFC 配置函数 ==========
void ConfigurePfc(bool enable, uint32_t high, uint32_t low) {
  Config::Set("/NodeList/*/DeviceList/*/$ns3::QbbNetDevice/PfcEnable", 
              BooleanValue(enable));
  Config::Set("/NodeList/*/DeviceList/*/$ns3::QbbNetDevice/PfcHighPkts", 
              UintegerValue(high));
  Config::Set("/NodeList/*/DeviceList/*/$ns3::QbbNetDevice/PfcLowPkts", 
              UintegerValue(low));
}

// ========== ECMP 配置函数 ==========
void ConfigureEcmp(const std::string& mode) {
  bool perflow = (mode == "perflow");
  std::string base = "/NodeList/*/$ns3::Ipv4L3Protocol/"
                     "$ns3::Ipv4ListRouting/$ns3::Ipv4GlobalRouting/";
  Config::Set(base + "PerflowEcmpRouting", BooleanValue(perflow));
  Config::Set(base + "RandomEcmpRouting", BooleanValue(!perflow));

  // 强制运行时生效
  for (uint32_t i = 0; i < NodeList::GetNNodes(); ++i) {
    Ptr<Node> node = NodeList::GetNode(i);
    Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
    if (!ipv4) continue;
    
    Ptr<Ipv4ListRouting> list = 
        DynamicCast<Ipv4ListRouting>(ipv4->GetRoutingProtocol());
    if (!list) continue;

    for (uint32_t k = 0; k < list->GetNRoutingProtocols(); ++k) {
      int16_t prio;
      Ptr<Ipv4GlobalRouting> gr = 
          DynamicCast<Ipv4GlobalRouting>(list->GetRoutingProtocol(k, prio));
      if (gr) {
        gr->SetAttribute("PerflowEcmpRouting", BooleanValue(perflow));
        gr->SetAttribute("RandomEcmpRouting", BooleanValue(!perflow));
      }
    }
  }
}

// ========== 主函数 ==========
int main(int argc, char* argv[]) {
  // 参数
  std::string ecmpMode = "perflow";
  bool pfcEnable = true;
  uint32_t pfcHigh = 8;
  uint32_t pfcLow = 4;

  CommandLine cmd;
  cmd.AddValue("ecmp", "ECMP mode: perflow|perpacket", ecmpMode);
  cmd.AddValue("pfc", "Enable PFC", pfcEnable);
  cmd.AddValue("pfcHigh", "PFC high threshold (pkts)", pfcHigh);
  cmd.AddValue("pfcLow", "PFC low threshold (pkts)", pfcLow);
  cmd.Parse(argc, argv);

  // ========== 1. 构建拓扑（使用 QbbPointToPointHelper）==========
  NodeContainer nodes;
  nodes.Create(4); // 示例：4 个节点

  QbbPointToPointHelper qbb;
  qbb.SetDeviceAttribute("DataRate", StringValue("10Gbps"));
  qbb.SetChannelAttribute("Delay", StringValue("1ms"));
  
  // 连接节点
  NetDeviceContainer devices01 = qbb.Install(nodes.Get(0), nodes.Get(1));
  NetDeviceContainer devices02 = qbb.Install(nodes.Get(0), nodes.Get(2));
  NetDeviceContainer devices13 = qbb.Install(nodes.Get(1), nodes.Get(3));
  NetDeviceContainer devices23 = qbb.Install(nodes.Get(2), nodes.Get(3));

  // ========== 2. 安装协议栈 ==========
  InternetStackHelper stack;
  stack.Install(nodes);

  // ========== 3. 分配 IP ==========
  Ipv4AddressHelper ipv4;
  ipv4.SetBase("10.1.1.0", "255.255.255.0");
  ipv4.Assign(devices01);
  ipv4.SetBase("10.1.2.0", "255.255.255.0");
  ipv4.Assign(devices02);
  ipv4.SetBase("10.1.3.0", "255.255.255.0");
  ipv4.Assign(devices13);
  ipv4.SetBase("10.1.4.0", "255.255.255.0");
  ipv4.Assign(devices23);

  // ========== 4. 配置 ECMP ==========
  ConfigureEcmp(ecmpMode);

  // ========== 5. 填充路由表 ==========
  Ipv4GlobalRoutingHelper::PopulateRoutingTables();

  // ========== 6. 配置 PFC ==========
  ConfigurePfc(pfcEnable, pfcHigh, pfcLow);

  // ========== 7. 安装你们的流量应用 ==========
  // ... 你们的流量代码 ...

  // ========== 8. 运行仿真 ==========
  Simulator::Stop(Seconds(10.0));
  Simulator::Run();
  Simulator::Destroy();

  return 0;
}
```

---

四、命令行参数说明

编译后可通过命令行灵活配置：

```bash
启用 PFC + perflow ECMP
./your-program --pfc=1 --pfcHigh=12 --pfcLow=6 --ecmp=perflow

禁用 PFC + perpacket ECMP
./your-program --pfc=0 --ecmp=perpacket


```
---

五、关于流量生成和拓扑：

由src下的generic-topology库完成功能

拓扑文件作出修改：

之前的是所有链路带宽一样

现在把核心层-汇聚层，汇聚层-汇聚层带宽设为了汇聚层-边缘层的4倍




---

六、关于获取状态

sim-stats-collector库最后会返回一个results：

获取三个全局指标：
```bash
double myTput = results.global.throughputMbps;
double myDelay = results.global.avgDelayMs;
double myLoss = results.global.lossRatePct;
```

获取每条链路的实时队列以及链路利用率

```bash
LinkTimeSeries myLinkData = results.links[idx];
std::vector<double> arrQueueA = myLinkData.queueSnapshotsA;
std::vector<double> arrQueueB = myLinkData.queueSnapshotsB;
std::vector<double> arrUtil   = myLinkData.utilSnapshots;
```

延迟，带宽的设置：
见topo-traffic-builder.cc：

```bash
double delay_ms = (w * 2.0) / 100.0;
double bandwidth_mbps = 50.0 / w; 
```

这个w是auto.txt文件里面的第三列，现在50.0 / w的话等于汇聚层-边缘层带宽0.5Mbps，核心层-汇聚层和汇聚层-汇聚层带宽2Mbps，要设多大带宽改这个50就行


---
七、如何配置本项目

ns-3.45
1. Install all dependencies required by ns-3.
```

# minimal requirements for C++:
apt-get install gcc g++ python3 python3-pip cmake

```

Check [ns-3 requirements](https://www.nsnam.org/docs/tutorial/html/getting-started.html#prerequisites/)

2. Install ZMQ, Protocol Buffers and pkg-config libs:
```
sudo apt-get update
apt-get install libzmq5 libzmq3-dev
apt-get install libprotobuf-dev
apt-get install protobuf-compiler
apt-get install pkg-config
```

3. Download and install ns3

```
wget https://www.nsnam.org/releases/ns-allinone-3.45.tar.bz2
tar xf ns-allinone-3.45.tar.bz2
cd ns-allinone-3.45
```

4. Clone ns3-gym repository into `contrib` directory and change the branch:
```
cd ./ns-3.45/contrib
git clone https://github.com/tkn-tub/ns3-gym.git ./opengym
cd opengym/
git checkout app-ns-3.36+
```
Check [working with cmake](https://www.nsnam.org/docs/manual/html/working-with-cmake.html)

It is important to use the `opengym` as the name of the ns3-gym app directory. 

ns3gym
 
Install ns3gym located in model/ns3gym (Python3 required)
```
cd ./contrib/opengym/
pip3 install --user ./model/ns3gym
```
or
```
python3 -m venv ns3gym-venv
source ./ns3gym-venv/bin/activate
pip3 install ./model/ns3gym
```


项目编译和运行

此处需要改动我们的项目名称my-open-gym为opengym

Configure and build ns-3 project:
```
cd ../../
./ns3 configure --enable-examples
./ns3 build
```
Note: Opengym Protocol Buffer messages (C++ and Python) are build during configure.

Run a project:

```
conda env create -f env.yml
conda activate LoadBalancingRL_env
```
```
cd ./contrib/opengym/examples/opengym/ 
python train.py
```