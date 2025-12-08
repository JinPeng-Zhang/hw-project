/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "topo-traffic-builder.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/point-to-point-net-device.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <iomanip>

using namespace std;

namespace ns3 {

// 辅助函数定义在匿名命名空间或static
static Ipv4Address GetFirstNonLoopbackIp(Ptr<Node> node) {
    Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
    for (uint32_t i = 1; i < ipv4->GetNInterfaces(); ++i)
        if (ipv4->GetNAddresses(i) > 0) return ipv4->GetAddress(i, 0).GetLocal();
    return Ipv4Address("0.0.0.0");
}

static bool LoadSizeCdf(const std::string& path, std::vector<std::pair<uint32_t,double>>& out) {
    out.clear(); std::ifstream fin(path.c_str());
    if (!fin.is_open()) return false;
    std::string line;
    while (std::getline(fin, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line); double sizeVal=0.0, cdfPct=0.0;
        if (!(iss >> sizeVal >> cdfPct)) continue;
        out.push_back({(uint32_t)std::floor(std::max(0.0, sizeVal)+0.5), std::clamp(cdfPct, 0.0, 100.0)});
    }
    fin.close(); 
    if(out.empty()) return false;
    std::sort(out.begin(), out.end(), [](auto& a, auto& b){ return a.second < b.second; });
    return true;
}

static uint32_t SampleSizeFromCdf(Ptr<UniformRandomVariable> rng, const std::vector<std::pair<uint32_t,double>>& cdf) {
    if (cdf.empty()) return 1u;
    double u = rng->GetValue(0.0, 100.0);
    size_t i = 0; while (i < cdf.size() && u > cdf[i].second) ++i;
    if (i == 0) return std::max(1u, cdf[0].first);
    if (i >= cdf.size()) return std::max(1u, cdf.back().first);
    double t = (u - cdf[i-1].second) / (cdf[i].second - cdf[i-1].second);
    return (uint32_t)std::floor(std::max(1.0, (double)cdf[i-1].first + t*(double)(cdf[i].first - cdf[i-1].first)) + 0.5);
}

TopoTrafficBuilder::TopoTrafficBuilder() {}

bool TopoTrafficBuilder::BuildAndInstall(std::string topoFilePath, std::string cdfFilePath,
                                         uint32_t flows, std::string transport,
                                         double loadRate, double linkRefMbps, double appsStop) 
{
    m_appsStop = appsStop;
    
    std::ifstream topoFile(topoFilePath.c_str());
    if (!topoFile.is_open()) return false;

    int numNodes=0, numLinks=0, state=0;
    std::string line, firstLinkLine; bool hasFirstLink=false;
    std::set<uint32_t> connectedNodes;
    m_edgeNodes.clear();

    while (std::getline(topoFile, line)) {
        if (line.empty() || line[0]=='#') continue;
        if (line.find("TOPO_TYPE:")==0) continue;
        if (line.find("EDGE_NODES:")==0) {
            std::istringstream iss(line.substr(11)); uint32_t node;
            while (iss >> node) m_edgeNodes.push_back(node);
            continue;
        }
        std::istringstream iss(line);
        if (state==0) { if (iss >> numNodes) state=1; }
        else if (state==1) { if (iss >> numLinks) state=2; }
        else if (state==2) { firstLinkLine=line; hasFirstLink=true; break; }
    }

    if (numNodes <= 0) return false;
    m_nodes.Create(numNodes);
    
    // =================================================================
    // 【拥塞控制修改点】：如果未来要改拥塞控制，就在这里设置 Config::SetDefault
    // 例如: Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpBbr"));
    // =================================================================
    InternetStackHelper stack; 
    stack.Install(m_nodes);
    
    for (uint32_t i=0; i<m_nodes.GetN(); ++i) m_nodes.Get(i)->GetObject<Ipv4>()->SetAttribute("IpForward", BooleanValue(true));

    PointToPointHelper p2p;
    Ipv4AddressHelper address; address.SetBase ("10.0.0.0", "255.255.255.252");
    p2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("20p"));

    auto installLink = [&](const std::string& linkLine) {
        if (linkLine.empty() || linkLine[0]=='#' || linkLine.find(":")!=std::string::npos) return;
        std::istringstream iss(linkLine); int n1, n2, w;
        if (!(iss >> n1 >> n2)) return;
        if (!(iss >> w)) w=100;
        if (n1<0 || n1>=numNodes || n2<0 || n2>=numNodes) return;
        
        connectedNodes.insert(n1); connectedNodes.insert(n2);
        std::ostringstream ds, bs; 
        double delay_ms = (w * 2.0) / 100.0;
        double bandwidth_mbps = 50.0 / w; 
        ds << delay_ms << "ms"; bs << bandwidth_mbps << "Mbps";
        
        p2p.SetChannelAttribute("Delay", StringValue(ds.str()));
        p2p.SetDeviceAttribute("DataRate", StringValue(bs.str()));
        
        NodeContainer nc(m_nodes.Get(n1), m_nodes.Get(n2));
        NetDeviceContainer ndc = p2p.Install(nc);
        address.Assign(ndc); address.NewNetwork();

        Ptr<PointToPointNetDevice> d0 = DynamicCast<PointToPointNetDevice>(ndc.Get(0));
        Ptr<PointToPointNetDevice> d1 = DynamicCast<PointToPointNetDevice>(ndc.Get(1));
        if (d0 && d1) {
            DataRateValue drv; d0->GetAttribute("DataRate", drv);
            m_links.push_back({(uint32_t)n1, (uint32_t)n2, d0, d1, drv.Get().GetBitRate()});
        }
    };

    if (hasFirstLink) installLink(firstLinkLine);
    while (std::getline(topoFile, line)) installLink(line);
    topoFile.close();

    if (m_edgeNodes.empty()) for(auto n : connectedNodes) m_edgeNodes.push_back(n);
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // 流量生成
    std::vector<std::pair<uint32_t,double>> sizeCdf;
    LoadSizeCdf(cdfFilePath, sizeCdf);

    Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable>();
    Ptr<NormalRandomVariable> tcpRateRv = CreateObject<NormalRandomVariable>();
    tcpRateRv->SetAttribute("Mean", DoubleValue(loadRate));
    tcpRateRv->SetAttribute("Variance", DoubleValue(0.0025)); 

    double startMin = 0.1; double startMax = 0.6;
    double tcpRateMinFactor = 0.01; double tcpRateMaxFactor = 1.0;
    uint32_t basePort = 10000;
    
    auto pickNode = [&](const std::vector<uint32_t>& vec) { return vec[rng->GetInteger(0, vec.size()-1)]; };

    for (uint32_t i=0; i<flows; ++i) {
        uint32_t srcId = pickNode(m_edgeNodes);
        uint32_t dstId; do { dstId = pickNode(m_edgeNodes); } while (dstId == srcId);
        
        uint32_t bytes = sizeCdf.empty() ? 1000 : SampleSizeFromCdf(rng, sizeCdf);
        if (bytes == 0) bytes = 1;
        
        Ipv4Address dstIp = GetFirstNonLoopbackIp(m_nodes.Get(dstId));
        uint16_t port = basePort + i;
        bool useTcp = (transport == "tcp");

        PacketSinkHelper sinkHelper(useTcp ? "ns3::TcpSocketFactory" : "ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer sinkApps = sinkHelper.Install(m_nodes.Get(dstId));
        sinkApps.Start(Seconds(0.0)); sinkApps.Stop(Seconds(appsStop));
        Ptr<PacketSink> sink = DynamicCast<PacketSink>(sinkApps.Get(0));
        m_apps.Add(sinkApps);

        double start;
        if (rng->GetValue(0.0, 1.0) < 0.9) start = 0.2 + rng->GetValue(0.0, 0.1);
        else start = startMin + rng->GetValue(0.0, startMax - startMin);

        double factor = tcpRateRv->GetValue();
        factor = std::clamp(factor, tcpRateMinFactor, tcpRateMaxFactor);
        double rateMbps = std::max(0.001, factor * linkRefMbps);
        std::ostringstream rateStr; rateStr << rateMbps << "Mbps";

        OnOffHelper onoff(useTcp ? "ns3::TcpSocketFactory" : "ns3::UdpSocketFactory", InetSocketAddress(dstIp, port));
        onoff.SetAttribute("MaxBytes", UintegerValue(bytes));
        onoff.SetAttribute("DataRate", DataRateValue(DataRate(rateStr.str())));
        onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        
        if (!useTcp) {
            onoff.SetAttribute("PacketSize", UintegerValue(std::min<uint32_t>(bytes, 1400u)));
            onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        } else {
            double onDur = std::max(0.0, appsStop - start + 1.0);
            onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=" +std::to_string(onDur) + "]"));
            onoff.SetAttribute("PacketSize", UintegerValue(1448));
        }

        ApplicationContainer clientApps = onoff.Install(m_nodes.Get(srcId));
        clientApps.Start(Seconds(start)); clientApps.Stop(Seconds(appsStop));
        m_apps.Add(clientApps);
        
        m_flows.push_back({srcId, dstId, bytes, sink});
    }
    return true;
}

} // namespace ns3
