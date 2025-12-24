/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "ns3/core-module.h"
#include "ns3/topo-traffic-builder.h"
#include "ns3/sim-stats-collector.h"

#include "ns3/qbb-point-to-point-helper.h"
#include "ns3/qbb-net-device.h"

#include "rl-env.h"
#include "ns3/opengym-module.h"
#include <iostream>
#include <vector>
#include <iomanip>

using namespace ns3;

void ConfigurePfc(bool enable, uint32_t high, uint32_t low) {
  Config::Set("/NodeList/*/DeviceList/*/$ns3::QbbNetDevice/PfcEnable", 
              BooleanValue(enable));
  Config::Set("/NodeList/*/DeviceList/*/$ns3::QbbNetDevice/PfcHighPkts", 
              UintegerValue(high));
  Config::Set("/NodeList/*/DeviceList/*/$ns3::QbbNetDevice/PfcLowPkts", 
              UintegerValue(low));
}


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

int main (int argc, char *argv[])
{
// 文件路径
  std::string topoFile = "./auto.txt";
  std::string cdfFile = "./FbHdp_distribution.txt";
  std::string ECMPfile = "./ecmpProbability.txt";
  std::string ECMPCacheFile = "./ecmpCache.txt";
// 仿真参数
  uint32_t flows = 100;
  std::string transport = "tcp";
  double loadRate = 0.3;
  double linkRefMbps = 10.0;
  double appsStop = 30.0;
// PFC与ECMP参数
  bool pfcEnable = true;
  uint32_t pfcHigh = 8;
  uint32_t pfcLow = 4;
  std::string ecmpMode = "perflow";
//opengym环境
  uint32_t openGymPort = 5555;
  double envTimeStep = 1;

// 命令行解析
  CommandLine cmd;
  cmd.AddValue ("openGymPort", "Port number for OpenGym env. Default: 5555", openGymPort);
  cmd.AddValue ("envTimeStep", "Time step interval for time-based TCP env [s]. Default: 0.1s", envTimeStep);
  cmd.AddValue("ecmp", "ECMP mode: perflow|perpacket", ecmpMode);
  cmd.AddValue("pfc", "Enable PFC", pfcEnable);
  cmd.AddValue("pfcHigh", "PFC high threshold (pkts)", pfcHigh);
  cmd.AddValue("pfcLow", "PFC low threshold (pkts)", pfcLow);
  cmd.AddValue ("flows", "流数量", flows);
  cmd.AddValue ("transport", "tcp/udp", transport);
  cmd.AddValue ("load-rate", "负载率", loadRate);
  cmd.AddValue ("link-ref-mbps", "参考带宽", linkRefMbps);
  cmd.AddValue ("appsStop", "停止时间", appsStop);
  cmd.Parse (argc, argv);

  RngSeedManager::SetSeed(time(NULL));
  RngSeedManager::SetRun(1);

//PFC与ECMP配置
  ConfigureEcmp(ecmpMode);
  ConfigurePfc(pfcEnable, pfcHigh, pfcLow); 

// 创建OpenGym环境
//  Ptr<OpenGymInterface> openGymInterface = OpenGymInterface::Get(openGymPort);
  Ptr<OpenGymInterface> openGymInterface = CreateObject<OpenGymInterface> (openGymPort);
  Ptr<MyGymEnv> myGymEnv = CreateObject<MyGymEnv> (Seconds(envTimeStep));
  myGymEnv->SetOpenGymInterface(openGymInterface);

// 拓扑环境搭建
  TopoTrafficBuilder builder;
  if (!builder.BuildAndInstall(topoFile, cdfFile, flows, transport, loadRate, linkRefMbps, appsStop)) {
      std::cerr << "环境搭建失败！" << std::endl;
      return 1;
  }
  std::cout << "拓扑构建完成。" << std::endl;

// 仿真运行
  std::cout << "开始仿真..." << std::endl;
  Simulator::Stop(Seconds(appsStop + 1.0));
  Simulator::Run();

  Simulator::Destroy();
  std::cout << "仿真结束。" << std::endl;

  openGymInterface->NotifySimulationEnd();

  return 0;
}
