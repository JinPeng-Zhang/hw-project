/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "ns3/core-module.h"
#include "ns3/topo-traffic-builder.h"
#include "ns3/sim-stats-collector.h"
#include <iostream>
#include <vector>
#include <iomanip>

using namespace ns3;

int main (int argc, char *argv[])
{
  std::string topoFile = "scratch/auto.txt";
  std::string cdfFile = "scratch/FbHdp_distribution.txt";
  uint32_t flows = 100;
  std::string transport = "tcp";
  double loadRate = 0.3;
  double linkRefMbps = 10.0;
  double appsStop = 30.0;

  CommandLine cmd;
  cmd.AddValue ("flows", "流数量", flows);
  cmd.AddValue ("transport", "tcp/udp", transport);
  cmd.AddValue ("load-rate", "负载率", loadRate);
  cmd.AddValue ("link-ref-mbps", "参考带宽", linkRefMbps);
  cmd.AddValue ("appsStop", "停止时间", appsStop);
  cmd.Parse (argc, argv);

  RngSeedManager::SetSeed(time(NULL));
  RngSeedManager::SetRun(1);

  TopoTrafficBuilder builder;
  if (!builder.BuildAndInstall(topoFile, cdfFile, flows, transport, loadRate, linkRefMbps, appsStop)) {
      std::cerr << "环境搭建失败！" << std::endl;
      return 1;
  }
  std::cout << "拓扑构建完成。" << std::endl;

  Ptr<SimStatsCollector> stats = CreateObject<SimStatsCollector>();
  stats->Setup(builder.GetLinks(), builder.GetFlows(), builder.GetAppsStopTime());

  std::cout << "开始仿真..." << std::endl;
  Simulator::Stop(Seconds(appsStop + 1.0));
  Simulator::Run();

  SimResult results = stats->CollectAndPrint();
  
  double myTput = results.global.throughputMbps;
  double myDelay = results.global.avgDelayMs;
  double myLoss = results.global.lossRatePct;

  std::cout << "\n全局指标验证:" << std::endl;
  std::cout << "  吞吐量变量: " << myTput << " Mbps" << std::endl;
  std::cout << "  时延变量: " << myDelay << " ms" << std::endl;
  std::cout << "  丢包率变量: " << myLoss << " %" << std::endl;

  std::vector<int> targets = {10, 11, 12};

  for (int idx : targets) {
      if (idx < (int)results.links.size()) {
          LinkTimeSeries myLinkData = results.links[idx];

          std::vector<double> arrQueueA = myLinkData.queueSnapshotsA;
          std::vector<double> arrQueueB = myLinkData.queueSnapshotsB;
          std::vector<double> arrUtil   = myLinkData.utilSnapshots;

          std::cout << "\n链路 " << idx << " (" << myLinkData.nodeA << "->" << myLinkData.nodeB << ") 数据全量打印:" << std::endl;
          
          auto printFullVec = [](const std::string& name, const std::vector<double>& v) {
              std::cout << "  " << name << ": [";
              for (size_t i = 0; i < v.size(); ++i) {
                  std::cout << std::fixed << std::setprecision(2) << v[i];
                  if (i < v.size() - 1) std::cout << ", ";
              }
              std::cout << "]" << std::endl;
          };

          printFullVec("我的队列A数组变量", arrQueueA);
          printFullVec("我的队列B数组变量", arrQueueB);
          printFullVec("我的利用率数组变量", arrUtil);

      } else {
          std::cout << "\n链路 " << idx << " 不存在！" << std::endl;
      }
  }

  Simulator::Destroy();
  std::cout << "仿真结束。" << std::endl;

  return 0;
}
