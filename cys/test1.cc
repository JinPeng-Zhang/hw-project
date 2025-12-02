/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * 通用拓扑仿真 - 自动读取拓扑,创建协议栈与地址分配
 */

#include <ctime>
#include <sstream>
#include <fstream>
#include <vector>
#include <queue>
#include <set>
#include <map>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <memory>
#include <iomanip>
#include <cmath>
#include <iostream>

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/queue.h"

using namespace ns3;
using namespace std;

NS_LOG_COMPONENT_DEFINE ("GenericTopology");

// =========================================================================
// 辅助函数
// =========================================================================

static Ipv4Address GetFirstNonLoopbackIp (Ptr<Node> node)
{
  Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
  for (uint32_t i = 1; i < ipv4->GetNInterfaces(); ++i)
    {
      if (ipv4->GetNAddresses(i) > 0)
        {
          return ipv4->GetAddress(i, 0).GetLocal();
        }
    }
  return Ipv4Address("0.0.0.0");
}

template <typename T>
static void ShuffleWithNs3Rng (std::vector<T>& v, Ptr<UniformRandomVariable> rng)
{
  if (v.empty()) return;
  for (size_t i = v.size() - 1; i > 0; --i)
    {
      uint32_t j = rng->GetInteger(0, static_cast<uint32_t>(i));
      std::swap(v[i], v[j]);
    }
}

struct DevStats
{
  uint64_t txBytes = 0;
  uint64_t txPackets = 0; 
  uint64_t enqBytes = 0;
  uint64_t enqPackets = 0;
  uint64_t dropBytes = 0;
  uint64_t dropPackets = 0;

  // 【修改】采样快照改为 double 类型，支持小数
  uint32_t currentQueueSize = 0;
  std::vector<double> queueSnapshots;  // 改为 double

  bool firstTxLogged = false;
  std::string tag;

  void Tx (Ptr<const Packet> p)
  {
    txBytes += p->GetSize();
    txPackets += 1;
    if (!firstTxLogged)
    {
      firstTxLogged = true;
      NS_LOG_UNCOND("[DevTx first] t=" << std::fixed << std::setprecision(6)
                    << Simulator::Now().GetSeconds() << "s tag=" << tag
                    << " firstTxBytes=" << p->GetSize());
    }
  }

  void Enq(Ptr<const Packet> p)
  {
    enqBytes += p->GetSize();
    enqPackets += 1;
  }
  void Deq(Ptr<const Packet> p)
  {
    (void)p;
  }
  void Drop(Ptr<const Packet> p)
  {
    dropBytes += p->GetSize();
    dropPackets += 1;
  }

  void RecordSnapshot() {
    queueSnapshots.push_back(currentQueueSize);
  }
};

struct LinkStats
{
  std::shared_ptr<DevStats> sa;
  std::shared_ptr<DevStats> sb;
  uint64_t dataRateBps = 0;

  Ptr<Queue<Packet>> queueA;
  Ptr<Queue<Packet>> queueB;

  uint32_t nodeA;
  uint32_t nodeB;

  uint64_t lastTxBytesTotal = 0;
  double   lastSampleTime   = -1.0;
  std::vector<double> utilSnapshots;
  std::vector<double> sampleTimestamps;
};

struct FlowPlan
{
  uint32_t srcId;
  uint32_t dstId;
  Ipv4Address dstIp;
  uint16_t port;
  uint32_t bytesPlanned;
  std::string proto; 
};

static bool LoadSizeCdf(const std::string& path, std::vector<std::pair<uint32_t,double>>& out, std::string& err)
{
  out.clear();
  std::ifstream fin(path.c_str());
  if (!fin.is_open())
    {
      err = "无法打开 CDF 文件: " + path;
      return false;
    }
  std::string line;
  while (std::getline(fin, line))
    {
      if (line.empty()) continue;
      if (line[0] == '#') continue;
      std::istringstream iss(line);
      double sizeVal=0.0, cdfPct=0.0;
      if (!(iss >> sizeVal >> cdfPct)) continue;
      if (cdfPct < 0.0) cdfPct = 0.0;
      if (cdfPct > 100.0) cdfPct = 100.0;
      double s = std::max(0.0, sizeVal);
      uint32_t sz = (uint32_t) std::floor(s + 0.5);
      out.push_back(std::make_pair(sz, cdfPct));
    }
  fin.close();
  if (out.empty())
    {
      err = "CDF 文件为空或格式不正确";
      return false;
    }
  std::sort(out.begin(), out.end(),
            [](const std::pair<uint32_t,double>& a, const std::pair<uint32_t,double>& b){
              if (a.second == b.second) return a.first < b.first;
              return a.second < b.second;
            });
  std::vector<std::pair<uint32_t,double>> cleaned;
  cleaned.reserve(out.size());
  double lastCdf = -1.0;
  uint32_t lastSize = 0;
  for (auto &p : out)
    {
      double c = p.second;
      uint32_t s = p.first;
      if (c < lastCdf) continue;
      if (cleaned.empty())
        {
          cleaned.push_back(p);
          lastCdf = c;
          lastSize = s;
        }
      else
        {
          if (c == lastCdf)
            {
              if (s < lastSize)
                {
                  cleaned.back().first = s;
                  lastSize = s;
                }
            }
          else
            {
              if (s < lastSize) s = lastSize;
              cleaned.push_back(std::make_pair(s, c));
              lastCdf = c;
              lastSize = s;
            }
        }
    }
  out.swap(cleaned);

  if (out.front().second > 0.0)
    out.insert(out.begin(), std::make_pair(0u, 0.0));
  if (out.back().second < 100.0)
    out.push_back(std::make_pair(out.back().first, 100.0));

  return true;
}

static uint32_t SampleSizeFromCdf(Ptr<UniformRandomVariable> rng,
                                  const std::vector<std::pair<uint32_t,double>>& cdf)
{
  if (cdf.empty()) return 1u;
  double u = rng->GetValue(0.0, 100.0);
  size_t i = 0;
  while (i < cdf.size() && u > cdf[i].second) ++i;
  if (i == 0) return std::max(1u, cdf[0].first);
  if (i >= cdf.size()) return std::max(1u, cdf.back().first);

  size_t j = i - 1;
  uint32_t s0 = cdf[j].first;
  uint32_t s1 = cdf[i].first;
  double p0 = cdf[j].second;
  double p1 = cdf[i].second;

  if (p1 <= p0) return std::max(1u, s1);
  double t = (u - p0) / (p1 - p0);
  double s = (double)s0 + t * (double)(s1 - s0);
  uint32_t bytes = (uint32_t) std::floor(std::max(1.0, s) + 0.5);
  return bytes;
}

// 全局变量用于周期性采样
static std::vector<LinkStats>* g_allLinksPtr = nullptr;
static const uint32_t SUB_SAMPLE_COUNT = 1000;

struct SubSampleAccumulator {
    std::vector<uint64_t> queueA_sum;
    std::vector<uint64_t> queueB_sum;
    uint32_t count = 0;
};

static SubSampleAccumulator g_subSampleAccum;

void RecordAllQueuesAtSubPoint()
{
    if (g_allLinksPtr == nullptr) return;

    if (g_subSampleAccum.queueA_sum.empty())
    {
        g_subSampleAccum.queueA_sum.resize(g_allLinksPtr->size(), 0);
        g_subSampleAccum.queueB_sum.resize(g_allLinksPtr->size(), 0);
    }

    for (size_t i = 0; i < g_allLinksPtr->size(); ++i)
    {
        auto& link = (*g_allLinksPtr)[i];
        uint32_t qA = link.queueA ? link.queueA->GetNPackets() : 0;
        uint32_t qB = link.queueB ? link.queueB->GetNPackets() : 0;
        g_subSampleAccum.queueA_sum[i] += qA;
        g_subSampleAccum.queueB_sum[i] += qB;
    }

    g_subSampleAccum.count++;
}

void RecordAllQueues()
{
    if (g_allLinksPtr == nullptr) return;

    const double now = Simulator::Now().GetSeconds();

    for (size_t i = 0; i < g_allLinksPtr->size(); ++i)
    {
        auto& link = (*g_allLinksPtr)[i];

        link.sampleTimestamps.push_back(now);

        // 【修改】计算平均队列长度时保留小数
        double qA = 0.0, qB = 0.0;
        if (g_subSampleAccum.count > 0)
        {
            qA = (double)g_subSampleAccum.queueA_sum[i] / (double)g_subSampleAccum.count;
            qB = (double)g_subSampleAccum.queueB_sum[i] / (double)g_subSampleAccum.count;
        }
        else
        {
            qA = link.queueA ? (double)link.queueA->GetNPackets() : 0.0;
            qB = link.queueB ? (double)link.queueB->GetNPackets() : 0.0;
        }

        link.sa->queueSnapshots.push_back(qA);
        link.sb->queueSnapshots.push_back(qB);

        uint64_t currTxTotal = link.sa->txBytes + link.sb->txBytes;

        double utilStep = 0.0;
        if (link.lastSampleTime < 0.0)
        {
            utilStep = 0.0;
        }
        else
        {
            double dt = now - link.lastSampleTime;
            
            if (dt > 0.0)
            {
                double capBytesOneDir = (double)link.dataRateBps / 8.0 * dt;
                double capBytesBoth   = 2.0 * capBytesOneDir;
                double deltaBytes     = (double)(currTxTotal - link.lastTxBytesTotal);
                if (capBytesBoth > 0.0)
                {
                    utilStep = 100.0 * deltaBytes / capBytesBoth;
                    if (utilStep < 0.0) utilStep = 0.0;
                    if (utilStep > 100.0) utilStep = 100.0;
                }
            }
        }
        link.utilSnapshots.push_back(utilStep);

        link.lastSampleTime   = now;
        link.lastTxBytesTotal = currTxTotal;
    }

    g_subSampleAccum.queueA_sum.assign(g_allLinksPtr->size(), 0);
    g_subSampleAccum.queueB_sum.assign(g_allLinksPtr->size(), 0);
    g_subSampleAccum.count = 0;
}

std::vector<double> GetStatus(
    uint32_t flows,
    const std::string& transport,
    double loadRate,
    double linkRefMbps,
    double appsStop
);

// =========================================================================
// main 函数
// =========================================================================
int main (int argc, char *argv[])
{
  RngSeedManager::SetSeed(time(NULL));

  uint32_t flows = 100;
  string transport = "tcp";
  double loadRate = 0.3;
  double linkRefMbps = 10.0;
  double appsStop = 30.0;

  CommandLine cmd;
  cmd.AddValue ("flows", "生成的小流数量", flows);
  cmd.AddValue ("transport", "传输层协议: udp 或 tcp", transport);
  cmd.AddValue ("load-rate", "TCP per-flow 速率均值系数", loadRate);
  cmd.AddValue ("link-ref-mbps", "参考链路带宽（Mbps）", linkRefMbps);
  cmd.AddValue ("appsStop", "应用程序停止时间（秒）", appsStop);
  cmd.Parse (argc, argv);

  int run_count = 0;
  while (run_count < 1)
  {
      std::cout << "--- 仿真调用 " << (run_count + 1) << " ---" << std::endl;

      std::vector<double> results = GetStatus(
          flows, transport, loadRate, linkRefMbps, appsStop
      );

      if (results.empty() || results.size() < 3) {
          std::cerr << "仿真 " << (run_count + 1) << " 发生错误" << std::endl;
      } else {
          std::vector<std::pair<uint32_t, uint32_t>> linkNodePairs;
          std::ifstream topoFile("scratch/auto.txt");
          if (topoFile.is_open()) {
              std::string line;
              int state = 0;
              int numNodes = 0, numLinks = 0;

              while (std::getline(topoFile, line)) {
                  if (line.empty() || line[0] == '#') continue;
                  if (line.find("TOPO_TYPE:") == 0) continue;
                  if (line.find("EDGE_NODES:") == 0) continue;

                  std::istringstream iss(line);
                  if (state == 0) {
                      if (iss >> numNodes) state = 1;
                  } else if (state == 1) {
                      if (iss >> numLinks) state = 2;
                  } else if (state == 2) {
                      int n1, n2;
                      if (iss >> n1 >> n2) {
                          linkNodePairs.push_back(std::make_pair((uint32_t)n1, (uint32_t)n2));
                      }
                  }
              }
              topoFile.close();
          }

          uint32_t numLinks = (results.size() - 3) / 2;

          std::cout << "\n========================================" << std::endl;
          std::cout << "仿真结果汇总 (调用 " << (run_count + 1) << ")" << std::endl;
          std::cout << "========================================" << std::endl;
          std::cout << "链路数量: " << numLinks << std::endl;

          std::vector<std::pair<uint32_t, double>> queueData;
          for (uint32_t i = 0; i < numLinks; ++i) {
              queueData.push_back(std::make_pair(i, results[i]));
          }
          std::sort(queueData.begin(), queueData.end(),
                    [](const std::pair<uint32_t, double>& a, const std::pair<uint32_t, double>& b) {
                        return a.second > b.second;
                    });

          std::cout << "\n平均队列长度最大的10条链路:" << std::endl;
          for (uint32_t i = 0; i < std::min(10u, numLinks); ++i) {
              uint32_t linkIdx = queueData[i].first;
              double avgQueue = queueData[i].second;
              if (linkIdx < linkNodePairs.size()) {
                  std::cout << "  节点" << linkNodePairs[linkIdx].first 
                            << "->节点" << linkNodePairs[linkIdx].second 
                            << " 平均队列: " << std::fixed << std::setprecision(2) 
                            << avgQueue << " 包" << std::endl;
              }
          }

          std::vector<std::pair<uint32_t, double>> utilData;
          for (uint32_t i = 0; i < numLinks; ++i) {
              utilData.push_back(std::make_pair(i, results[numLinks + i]));
          }
          std::sort(utilData.begin(), utilData.end(),
                    [](const std::pair<uint32_t, double>& a, const std::pair<uint32_t, double>& b) {
                        return a.second > b.second;
                    });

          std::cout << "\n链路利用率最大的10条链路 (%):" << std::endl;
          for (uint32_t i = 0; i < std::min(10u, numLinks); ++i) {
              uint32_t linkIdx = utilData[i].first;
              double util = utilData[i].second;
              if (linkIdx < linkNodePairs.size()) {
                  std::cout << "  节点" << linkNodePairs[linkIdx].first 
                            << "->节点" << linkNodePairs[linkIdx].second 
                            << " 利用率: " << std::fixed << std::setprecision(2) 
                            << util << "%" << std::endl;
              }
          }

          std::cout << "\n网络总吞吐量 (Mbps): " << std::fixed << std::setprecision(3) 
                    << results[results.size() - 3] << std::endl;
          std::cout << "网络平均时延 (ms): " << std::fixed << std::setprecision(3) 
                    << results[results.size() - 2] << std::endl;
          std::cout << "端到端丢包率 (%): " << std::fixed << std::setprecision(3) 
                    << results[results.size() - 1] << std::endl;
          std::cout << "========================================\n" << std::endl;

      }

      run_count++;
  }

  return 0;
}

// =========================================================================
// GetStatus 函数实现
// =========================================================================
std::vector<double> GetStatus(
    uint32_t flows,
    const std::string& transport,
    double loadRate,
    double linkRefMbps,
    double appsStop
)
{
  Simulator::Destroy(); 
  RngSeedManager::SetRun(1); 

  string input_topo_path ("scratch/auto.txt");
  string sizeCdfPath = "scratch/FbHdp_distribution.txt";

  string pairMode = "global";
  uint32_t basePort = 10000;
  double tcpRateStdFactor = 0.05;
  double tcpRateMinFactor = 0.01;
  double tcpRateMaxFactor = 1.0;
  double sampleInterval = 0.5;

  std::vector<std::pair<uint32_t,double>> sizeCdf;
  std::string err;
  if (!LoadSizeCdf(sizeCdfPath, sizeCdf, err))
    {
      return {-6.0, -6.0, -6.0, -6.0};
    }

  std::ifstream topoFile(input_topo_path.c_str());
  if (!topoFile.is_open())
    {
      return {-1.0, -1.0, -1.0, -1.0};
    }

  int numNodes = 0;
  int numLinks = 0;
  std::string line;
  int state = 0;
  std::vector<uint32_t> edgeNodes;
  std::map<uint32_t, std::vector<uint32_t> > nodeLinks;

  std::string firstLinkLine;
  bool hasFirstLink = false;

  while (std::getline(topoFile, line))
    {
      if (line.empty() || line[0] == '#') continue;
      if (line.find("TOPO_TYPE:") == 0) continue; 
      if (line.find("EDGE_NODES:") == 0)
        {
          std::istringstream iss(line.substr(11));
          uint32_t node;
          while (iss >> node) edgeNodes.push_back(node);
          continue;
        }

      std::istringstream iss(line);

      if (state == 0)
        {
          if (iss >> numNodes)
            {
              state = 1;
              continue;
            }
        }
      else if (state == 1)
        {
          if (iss >> numLinks)
            {
              state = 2;
              if (numLinks == 0) break;
              continue;
            }
        }
      else if (state == 2)
        {
          firstLinkLine = line;
          hasFirstLink = true;
          break;
        }
    }

  if (numNodes <= 0 || numLinks <= 0)
    {
      topoFile.close();
      return {-2.0, -2.0, -2.0, -2.0};
    }

  NodeContainer nodes;
  nodes.Create(numNodes);
  InternetStackHelper stack;
  stack.Install(nodes);

  for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
      Ptr<Ipv4> ipv4 = nodes.Get(i)->GetObject<Ipv4>();
      ipv4->SetAttribute("IpForward", BooleanValue(true));
    }

  PointToPointHelper p2p;
  Ipv4AddressHelper address;
  address.SetBase ("10.0.0.0", "255.255.255.252");
  p2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("20p"));

  std::set<uint32_t> connectedNodes;
  std::vector<LinkStats> allLinks;

  auto installLinkAndTrace = [&](const std::string& linkLine_arg) 
  {
    if (linkLine_arg.empty() || linkLine_arg[0] == '#') return;
    if (linkLine_arg.find("EDGE_NODES:") == 0) return;
    if (linkLine_arg.find("TOPO_TYPE:") == 0) return; 

    std::istringstream iss(linkLine_arg);
    int node1, node2, weight;

    if (!(iss >> node1 >> node2)) return;
    if (!(iss >> weight)) weight = 100;

    if (node1 < 0 || node1 >= numNodes || node2 < 0 || node2 >= numNodes)
      {
        return;
      }

    connectedNodes.insert((uint32_t)node1);
    connectedNodes.insert((uint32_t)node2);

    nodeLinks[(uint32_t)node1].push_back((uint32_t)node2);
    nodeLinks[(uint32_t)node2].push_back((uint32_t)node1);

    std::ostringstream delayStr, bwStr;
    double delay_ms = (weight * 2.0) / 100.0;
    double bandwidth_mbps = (50.0) / weight; 

    delayStr << delay_ms << "ms";
    bwStr << bandwidth_mbps << "Mbps";

    p2p.SetChannelAttribute ("Delay", StringValue (delayStr.str()));
    p2p.SetDeviceAttribute ("DataRate", StringValue (bwStr.str()));

    NodeContainer nc = NodeContainer(nodes.Get((uint32_t)node1), nodes.Get((uint32_t)node2));
    NetDeviceContainer ndc = p2p.Install(nc);
    address.Assign(ndc);
    address.NewNetwork();

    Ptr<PointToPointNetDevice> d0 = DynamicCast<PointToPointNetDevice>(ndc.Get(0));
    Ptr<PointToPointNetDevice> d1 = DynamicCast<PointToPointNetDevice>(ndc.Get(1));
    if (d0 && d1)
      {
        LinkStats ls;
        ls.sa = std::make_shared<DevStats>();
        ls.sb = std::make_shared<DevStats>();

        ls.nodeA = (uint32_t)node1;
        ls.nodeB = (uint32_t)node2;

        ls.sa->tag = std::string("A(") + std::to_string(node1) + "->" + std::to_string(node2) + ")";
        ls.sb->tag = std::string("B(") + std::to_string(node2) + "->" + std::to_string(node1) + ")";

        DataRateValue drv;
        d0->GetAttribute("DataRate", drv);
        ls.dataRateBps = drv.Get().GetBitRate();

        ls.lastTxBytesTotal = 0;
        ls.lastSampleTime   = -1.0;

        d0->TraceConnectWithoutContext("MacTx", MakeCallback(&DevStats::Tx, ls.sa.get()));
        d1->TraceConnectWithoutContext("MacTx", MakeCallback(&DevStats::Tx, ls.sb.get()));

        Ptr<Queue<Packet>> q0 = d0->GetQueue();
        Ptr<Queue<Packet>> q1 = d1->GetQueue();

        ls.queueA = q0;
        ls.queueB = q1;

        if (!q0) {
          NS_LOG_UNCOND("[Queue null] link(" << node1 << "->" << node2 << ") A-side queue is null");
        }
        if (!q1) {
          NS_LOG_UNCOND("[Queue null] link(" << node1 << "->" << node2 << ") B-side queue is null");
        }

        if (q0) {
            q0->TraceConnectWithoutContext("Enqueue", MakeCallback(&DevStats::Enq, ls.sa.get()));
            q0->TraceConnectWithoutContext("Dequeue", MakeCallback(&DevStats::Deq, ls.sa.get()));
            q0->TraceConnectWithoutContext("Drop",    MakeCallback(&DevStats::Drop, ls.sa.get()));
        }
        if (q1) {
            q1->TraceConnectWithoutContext("Enqueue", MakeCallback(&DevStats::Enq, ls.sb.get()));
            q1->TraceConnectWithoutContext("Dequeue", MakeCallback(&DevStats::Deq, ls.sb.get()));
            q1->TraceConnectWithoutContext("Drop",    MakeCallback(&DevStats::Drop, ls.sb.get()));
        }

        allLinks.push_back(ls);
      }
  };

  if (hasFirstLink) {
      installLinkAndTrace(firstLinkLine);
  }
  while (std::getline(topoFile, line)) { 
      installLinkAndTrace(line);
  }
  topoFile.close();

  if (edgeNodes.empty())
    {
      for (std::set<uint32_t>::iterator it = connectedNodes.begin(); it != connectedNodes.end(); ++it)
        edgeNodes.push_back(*it);
    }

  Ipv4GlobalRoutingHelper::PopulateRoutingTables();

  if (edgeNodes.empty() || basePort >= 65535)
    {
      return {-4.0, -4.0, -4.0, -4.0}; 
    }

  uint32_t targetPairs = flows;

  Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable>();

  std::vector<std::pair<uint32_t,uint32_t> > pairs;
  pairs.reserve(targetPairs);

  auto pickFrom = [&](const std::vector<uint32_t>& vec)->uint32_t {
    return vec[rng->GetInteger(0, static_cast<uint32_t>(vec.size()-1))];
  };

  for (uint32_t i = 0; i < targetPairs; ++i) {
      uint32_t src = pickFrom(edgeNodes);
      uint32_t dst;
      do { dst = pickFrom(edgeNodes); } while (dst == src);
      pairs.emplace_back(src, dst);
  }

  std::vector<uint32_t> sampledSizes;
  sampledSizes.reserve(pairs.size());
  if (!sizeCdf.empty())
    {
      for (size_t i = 0; i < pairs.size(); ++i)
        sampledSizes.push_back(SampleSizeFromCdf(rng, sizeCdf));
    }
  else
    {
      std::vector<int> sizeGroup(pairs.size(), 0); 
      for (size_t i = pairs.size()/2; i < pairs.size(); ++i) sizeGroup[i] = 1;
      ShuffleWithNs3Rng(sizeGroup, rng);
      for (size_t i = 0; i < pairs.size(); ++i)
        {
          uint32_t sz = (sizeGroup[i] == 0)
                        ? rng->GetInteger(1, 100)
                        : rng->GetInteger(100, 200);
          sampledSizes.push_back(sz);
        }
    }

  ApplicationContainer allSinks, allClients;
  std::vector< Ptr<PacketSink> > sinkPtrs;
  std::vector<FlowPlan> plans;

  double startMin = 0.1;
  double startMax = 0.6;

  Ptr<NormalRandomVariable> tcpRateRv = CreateObject<NormalRandomVariable>();
  tcpRateRv->SetAttribute("Mean",     DoubleValue(loadRate));
  tcpRateRv->SetAttribute("Variance", DoubleValue(tcpRateStdFactor * tcpRateStdFactor));

for (size_t i = 0; i < pairs.size(); ++i)
{
    uint32_t srcId = pairs[i].first;
    uint32_t dstId = pairs[i].second;

    Ptr<Node> src = nodes.Get(srcId);
    Ptr<Node> dst = nodes.Get(dstId);

    Ipv4Address dstIp = GetFirstNonLoopbackIp(dst);
    if (dstIp == Ipv4Address("0.0.0.0"))
    {
        return {-5.0, -5.0, -5.0, -5.0}; 
    }

    uint32_t bytes = sampledSizes[i];
    if (bytes == 0) bytes = 1;

    uint16_t port = (uint16_t)(basePort + (uint32_t)i);

    bool useTcp = (transport == "tcp"); 
    std::string flowProto = useTcp ? "tcp" : "udp";

    PacketSinkHelper sinkHelper(
        useTcp ? "ns3::TcpSocketFactory" : "ns3::UdpSocketFactory",
        InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApps = sinkHelper.Install(dst);
    sinkApps.Start(Seconds(0.0));
    sinkApps.Stop(Seconds(appsStop));
    allSinks.Add(sinkApps);
    Ptr<PacketSink> sink = DynamicCast<PacketSink>(sinkApps.Get(0));
    sinkPtrs.push_back(sink);

    // 【修改】每条流独立判断：90% 概率在 [0.2s, 0.3s] 突发区间
    double start;
    double burstProb = 0.9;      // 90% 概率突发
    double burstStart = 0.2;     // 突发区间起始
    double burstEnd = 0.3;       // 突发区间结束

    if (rng->GetValue(0.0, 1.0) < burstProb)
    {
        // 90% 概率：在 [0.2s, 0.3s] 区间均匀采样
        start = burstStart + rng->GetValue(0.0, burstEnd - burstStart);
    }
    else
    {
        // 10% 概率：在整个 [0.1s, 0.6s] 区间均匀采样
        start = startMin + rng->GetValue(0.0, startMax - startMin);
    }


    // 【修改】TCP 和 UDP 都使用相同的正态分布速率
    double factor = tcpRateRv->GetValue();
    factor = std::clamp(factor, tcpRateMinFactor, tcpRateMaxFactor);
    double rateMbps = std::max(0.001, factor * linkRefMbps);
    std::ostringstream rateStr; 
    rateStr << rateMbps << "Mbps";

    if (!useTcp)
    {
        // UDP 流量
        uint32_t pktSize = std::min<uint32_t>(bytes, 1400u);
        OnOffHelper onoff("ns3::UdpSocketFactory", InetSocketAddress(dstIp, port));
        onoff.SetAttribute("PacketSize", UintegerValue(pktSize));
        onoff.SetAttribute("MaxBytes",  UintegerValue(bytes));
        onoff.SetAttribute("DataRate",  DataRateValue(DataRate(rateStr.str())));  // ← 使用正态分布速率
        onoff.SetAttribute("OnTime",  StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer clientApps = onoff.Install(src);
        clientApps.Start(Seconds(start));
        clientApps.Stop(Seconds(appsStop));
        allClients.Add(clientApps);
    }
    else
    {
        // TCP 流量
        OnOffHelper onoff("ns3::TcpSocketFactory", InetSocketAddress(dstIp, port));

        double onDur = std::max(0.0, appsStop - start + 1.0);
        onoff.SetAttribute("OnTime",  StringValue("ns3::ConstantRandomVariable[Constant=" +std::to_string(onDur) + "]"));
        onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));

        onoff.SetAttribute("PacketSize", UintegerValue(1448));
        onoff.SetAttribute("MaxBytes",   UintegerValue(bytes));
        onoff.SetAttribute("DataRate",   DataRateValue(DataRate(rateStr.str())));  // 使用正态分布速率

        ApplicationContainer clientApps = onoff.Install(src);
        clientApps.Start(Seconds(start));
        clientApps.Stop(Seconds(appsStop));
        allClients.Add(clientApps);
    }

    plans.push_back(FlowPlan{ srcId, dstId, dstIp, port, bytes, flowProto }); 
}


  FlowMonitorHelper flowmonHelper;
  Ptr<FlowMonitor> monitor = flowmonHelper.InstallAll();

  g_allLinksPtr = &allLinks;

  const double sampleStart = std::max(0.0, startMin);
  const double sampleEnd   = appsStop;

// 【新增】在第一次正式采样前，初始化基准点
for (auto& link : allLinks)
{
    link.lastSampleTime = sampleStart;  // 0.1s
    link.lastTxBytesTotal = 0;          // 初始累计为0
}


  for (double t = sampleStart; t <= sampleEnd + 1e-12; t += sampleInterval)
  {
      double subDt = sampleInterval / (double)SUB_SAMPLE_COUNT;
      for (uint32_t sub = 0; sub < SUB_SAMPLE_COUNT; ++sub)
      {
          double subT = t + (sub + 0.5) * subDt;
          Simulator::Schedule(Seconds(subT), &RecordAllQueuesAtSubPoint);
      }

      Simulator::Schedule(Seconds(t + sampleInterval), &RecordAllQueues);
  }

  Simulator::Stop(Seconds(appsStop + 1.0));
  Simulator::Run();

  monitor->CheckForLostPackets();
  const auto& stats = monitor->GetFlowStats();

  double simTime = std::max(1e-9, Simulator::Now().GetSeconds());

  // 【修改】计算平均队列长度，支持小数
  std::vector<double> avgQueueLengths;
  avgQueueLengths.reserve(allLinks.size());

  for (const auto& L : allLinks)
  {
      const auto& snapshotsA = L.sa->queueSnapshots;
      const auto& snapshotsB = L.sb->queueSnapshots;

      if (snapshotsA.empty() || snapshotsB.empty()) {
          avgQueueLengths.push_back(0.0);
          continue;
      }

      size_t numSamples = std::min(snapshotsA.size(), snapshotsB.size());
      double sumQueue = 0.0;

      for (size_t i = 0; i < numSamples; ++i)
      {
          // 【修改】现在 snapshotsA[i] 和 snapshotsB[i] 都是 double
          double maxQ = std::max(snapshotsA[i], snapshotsB[i]);
          sumQueue += maxQ;
      }

      double avgQ = (numSamples > 0) ? (sumQueue / numSamples) : 0.0;
      avgQueueLengths.push_back(avgQ);
  }

  std::vector<double> linkUtilizations;
  linkUtilizations.reserve(allLinks.size());

  for (const auto& L : allLinks)
  {
      double capBytes = (double)L.dataRateBps / 8.0 * simTime;
      double totalTxBytes = (double)L.sa->txBytes + (double)L.sb->txBytes;
      double totalCapBytes = 2.0 * capBytes;
      double linkUtil = (totalCapBytes > 0.0) ? (100.0 * totalTxBytes / totalCapBytes) : 0.0;

      linkUtilizations.push_back(linkUtil);
  }

  double sumFlowAvgDelay = 0.0;
  uint32_t flowCountForDelay = 0;

  uint64_t fmTxPkts = 0, fmRxPkts = 0, fmLostPkts = 0;

  for (auto const& kv : stats)
  {
      const FlowMonitor::FlowStats& s = kv.second;
      if (s.rxPackets > 0)
      {
          double avgDelayMs = s.delaySum.GetSeconds() / s.rxPackets * 1000.0;
          sumFlowAvgDelay += avgDelayMs;
          flowCountForDelay++;
      }
      fmTxPkts += s.txPackets;
      fmRxPkts += s.rxPackets;
      fmLostPkts += s.lostPackets;
  }

  double networkAvgDelayMs = (flowCountForDelay > 0) ? (sumFlowAvgDelay / flowCountForDelay) : 0.0;
  double e2eLossRatePct = (fmTxPkts > 0) ? (100.0 * fmLostPkts / (double)fmTxPkts) : 0.0;

  uint64_t totalReceivedBytes = 0;
  for (size_t i = 0; i < plans.size(); ++i) 
  {
      uint64_t rx = sinkPtrs[i] ? sinkPtrs[i]->GetTotalRx() : 0;
      totalReceivedBytes += std::min<uint64_t>(rx, plans[i].bytesPlanned);
  }
  // 【修改】使用 appsStop 而非 simTime
  double totalThroughputMbps = (totalReceivedBytes * 8.0) / (appsStop * 1e6);

  NS_LOG_UNCOND("\n==================== 队列长度时间序列统计 ====================");
  NS_LOG_UNCOND("采样间隔: " << sampleInterval << " 秒");
  NS_LOG_UNCOND("采样窗口: [" << std::fixed << std::setprecision(3)
                << sampleStart << "s, " << sampleEnd << "s]");
  NS_LOG_UNCOND("仿真时间: " << simTime << " 秒");
  NS_LOG_UNCOND("链路总数: " << allLinks.size());
  NS_LOG_UNCOND("");

  for (size_t i = 0; i < allLinks.size(); ++i)
  {
      const auto& L = allLinks[i];
      const auto& snapshotsA = L.sa->queueSnapshots;
      const auto& snapshotsB = L.sb->queueSnapshots;

      NS_LOG_UNCOND("链路" << i << " (节点" << L.nodeA << "->节点" << L.nodeB << "):");
      NS_LOG_UNCOND("  平均队列: " << std::fixed << std::setprecision(2) 
                    << avgQueueLengths[i] << " 包");

      // 【修改】只输出前10个时间戳，避免日志过长
      std::ostringstream ossT;
      ossT << "  采样时间戳序列(前10个): [";
      for (size_t j = 0; j < std::min((size_t)10, L.sampleTimestamps.size()); ++j) {
          ossT << std::fixed << std::setprecision(3) << L.sampleTimestamps[j];
          if (j < std::min((size_t)10, L.sampleTimestamps.size()) - 1) ossT << ", ";
      }
      if (L.sampleTimestamps.size() > 10) ossT << ", ...";
      ossT << "]";
      NS_LOG_UNCOND(ossT.str());
      
      if (L.sampleTimestamps.size() > 1) {
          std::ostringstream ossDt;
          ossDt << "  实际采样间隔(前10个): [";
          for (size_t j = 1; j < std::min((size_t)10, L.sampleTimestamps.size()); ++j) {
              double dt = L.sampleTimestamps[j] - L.sampleTimestamps[j-1];
              ossDt << std::fixed << std::setprecision(3) << dt;
              if (j < std::min((size_t)10, L.sampleTimestamps.size()) - 1) ossDt << ", ";
          }
          if (L.sampleTimestamps.size() > 10) ossDt << ", ...";
          ossDt << "]";
          NS_LOG_UNCOND(ossDt.str());
      }

      // 【修改】队列序列保留2位小数
      std::ostringstream ossA, ossB, ossU;
      ossA << "  队列A序列: [";
      for (size_t j = 0; j < snapshotsA.size(); ++j) {
          ossA << std::fixed << std::setprecision(2) << snapshotsA[j];
          if (j < snapshotsA.size() - 1) ossA << ", ";
      }
      ossA << "]";

      ossB << "  队列B序列: [";
      for (size_t j = 0; j < snapshotsB.size(); ++j) {
          ossB << std::fixed << std::setprecision(2) << snapshotsB[j];
          if (j < snapshotsB.size() - 1) ossB << ", ";
      }
      ossB << "]";

      ossU << "  区间利用率序列(%): [";
      for (size_t j = 0; j < L.utilSnapshots.size(); ++j) {
          ossU << std::fixed << std::setprecision(2) << L.utilSnapshots[j];
          if (j < L.utilSnapshots.size() - 1) ossU << ", ";
      }
      ossU << "]";

      NS_LOG_UNCOND(ossA.str());
      NS_LOG_UNCOND(ossB.str());
      NS_LOG_UNCOND(ossU.str());
      NS_LOG_UNCOND("  利用率=" << std::fixed << std::setprecision(2) 
                    << linkUtilizations[i] << "% | 发送字节=" 
                    << (L.sa->txBytes + L.sb->txBytes) 
                    << " | 丢包数=" << (L.sa->dropPackets + L.sb->dropPackets));
      NS_LOG_UNCOND("");
  }

  NS_LOG_UNCOND("---------- 网络整体指标 ----------");
  NS_LOG_UNCOND("FlowMonitor totals: txPkts=" << fmTxPkts
                << " rxPkts=" << fmRxPkts
                << " lostPkts=" << fmLostPkts);
  NS_LOG_UNCOND("总吞吐量: " << std::fixed << std::setprecision(3) 
                << totalThroughputMbps << " Mbps");
  NS_LOG_UNCOND("平均时延: " << std::fixed << std::setprecision(3) 
                << networkAvgDelayMs << " ms");
  NS_LOG_UNCOND("端到端丢包率: " << std::fixed << std::setprecision(3) 
                << e2eLossRatePct << " %");
  NS_LOG_UNCOND("==============================================================\n");

  std::vector<double> results;
  results.reserve(avgQueueLengths.size() + linkUtilizations.size() + 3);

  for (double avgQ : avgQueueLengths)
  {
      results.push_back(avgQ);
  }

  for (double util : linkUtilizations)
  {
      results.push_back(util);
  }

  results.push_back(totalThroughputMbps);
  results.push_back(networkAvgDelayMs);
  results.push_back(e2eLossRatePct);

  g_allLinksPtr = nullptr;

  return results;
}


