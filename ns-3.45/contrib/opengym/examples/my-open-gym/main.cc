#include <ns3/core-module.h>
#include <ns3/ipv4-global-routing-helper.h>
#include "model/fattree-topology.h"
#include "experiments.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("FatTreeMain");

int main(int argc, char* argv[]) {
  LogComponentEnable("FatTreeMain", LOG_LEVEL_INFO);

  double simTime = 6.0;
  std::string ecmpMode = "perflow"; // perflow | perpacket

  // PFC 参数
  bool     pfcOn       = true;
  uint32_t pfcHighPkts = 8;
  uint32_t pfcLowPkts  = 4;
  uint32_t pfcFanIn    = 16;
  double   pfcRateBps  = 1e9;
  uint32_t pfcPktSize  = 1448;

  CommandLine cmd;
  cmd.AddValue("simTime",     "Simulation time (s)", simTime);
  cmd.AddValue("ecmpMode",    "perflow | perpacket", ecmpMode);

  cmd.AddValue("pfc",         "Enable real PFC (1/0)", pfcOn);
  cmd.AddValue("pfcHighPkts", "PFC high watermark (packets)", pfcHighPkts);
  cmd.AddValue("pfcLowPkts",  "PFC low watermark (packets)",  pfcLowPkts);
  cmd.AddValue("pfcFanIn",    "PFC UDP senders", pfcFanIn);
  cmd.AddValue("pfcRate",     "PFC per-flow UDP rate (bps)", pfcRateBps);
  cmd.AddValue("pfcPktSize",  "PFC UDP pkt size (bytes)", pfcPktSize);

  cmd.Parse(argc, argv);

  // 构建拓扑（基于 QbbPointToPointHelper，链路默认 10Gbps/1ms）
  FatTreeTopology topo(4);

  // 运行场景（仅 PFC+ECMP）
  fattree::RunScenario(topo, simTime, ecmpMode,
                       pfcOn, pfcHighPkts, pfcLowPkts,
                       pfcFanIn, pfcRateBps, pfcPktSize);
  return 0;
}
