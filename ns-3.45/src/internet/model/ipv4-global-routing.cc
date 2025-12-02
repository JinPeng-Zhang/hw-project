//
// Copyright (c) 2008 University of Washington
//
// SPDX-License-Identifier: GPL-2.0-only
//

#include "ipv4-global-routing.h"

#include "global-route-manager.h"
#include "ipv4-route.h"
#include "ipv4-routing-table-entry.h"

#include "ns3/boolean.h"
#include "ns3/log.h"
#include "ns3/names.h"
#include "ns3/net-device.h"
#include "ns3/node.h"
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/tcp-header.h"
#include "ns3/udp-header.h"
#include "ns3/ipv4.h"

#include <iomanip>
#include <vector>

#include <map>
#include <fstream>
#include <sstream>
#include <limits>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <cstdint>
// 修复：find_if / isspace 需要的头文件
#include <algorithm>
#include <cctype>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("Ipv4GlobalRouting");

NS_OBJECT_ENSURE_REGISTERED(Ipv4GlobalRouting);

// 匿名命名空间: ECMP 权重（按节点，来自单一文件）、节点重盐与哈希工具
namespace
{
// (nodeId, N备选数) -> 该节点此N下的权重前缀（缓存）
static std::map<std::pair<uint32_t, size_t>, std::vector<uint32_t>> g_ecmpWeightCacheByNodeN;

// 节点原始权重表：nodeId -> 一串权重（读取整行，运行时按前缀取 N 个）
static std::map<uint32_t, std::vector<uint32_t>> g_nodeRawWeights;
static bool g_weightsLoaded = false;

// 64-bit FNV-1a 常量
static constexpr uint64_t FNV64_OFFSET = 1469598103934665603ull;
static constexpr uint64_t FNV64_PRIME  = 1099511628211ull;

static inline void Fnv64MixByte(uint64_t& h, uint8_t b)
{
    h ^= b;
    h *= FNV64_PRIME;
}
static inline void Fnv64Mix16(uint64_t& h, uint16_t v)
{
    Fnv64MixByte(h, static_cast<uint8_t>(v & 0xffu));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 8) & 0xffu));
}
static inline void Fnv64Mix32(uint64_t& h, uint32_t v)
{
    Fnv64MixByte(h, static_cast<uint8_t>(v & 0xffu));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 8) & 0xffu));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 16) & 0xffu));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 24) & 0xffu));
}
static inline void Fnv64Mix64(uint64_t& h, uint64_t v)
{
    Fnv64MixByte(h, static_cast<uint8_t>(v & 0xffull));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 8) & 0xffull));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 16) & 0xffull));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 24) & 0xffull));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 32) & 0xffull));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 40) & 0xffull));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 48) & 0xffull));
    Fnv64MixByte(h, static_cast<uint8_t>((v >> 56) & 0xffull));
}

// 64-bit 雪崩压缩为 32-bit
static inline uint32_t Avalanche64To32(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return static_cast<uint32_t>(x ^ (x >> 32));
}

// 节点重盐：NodeId + 所有 IPv4 接口信息（64-bit FNV-1a），跨节点差异大
static uint64_t
GetNodeHeavySalt(Ptr<Ipv4> ipv4)
{
    if (!ipv4 || !ipv4->GetObject<Node>())
    {
        return 0x9e3779b97f4a7c15ull;
    }
    uint32_t nodeId = ipv4->GetObject<Node>()->GetId();

    uint64_t h = FNV64_OFFSET;
    Fnv64Mix32(h, nodeId);

    uint32_t nIf = ipv4->GetNInterfaces();
    Fnv64Mix32(h, nIf);
    for (uint32_t i = 0; i < nIf; ++i)
    {
        uint32_t nAddr = ipv4->GetNAddresses(i);
        Fnv64Mix32(h, nAddr);
        for (uint32_t j = 0; j < nAddr; ++j)
        {
            Ipv4InterfaceAddress ifa = ipv4->GetAddress(i, j);
            Fnv64Mix32(h, ifa.GetLocal().Get());
            Fnv64Mix32(h, ifa.GetMask().Get());
            Fnv64Mix32(h, ifa.GetBroadcast().Get());
        }
    }
    Fnv64Mix32(h, nodeId ^ 0x85ebca6bu);
    if (h == 0) h = 0x9e3779b97f4a7c15ull;
    return h;
}

// 去掉行内注释并修剪空白
static std::string
StripCommentAndTrim(const std::string& line)
{
    std::string s = line;
    auto pos = s.find('#');
    if (pos != std::string::npos) s = s.substr(0, pos);
    // trim 左右空白（安全类型）
    auto notspace = [](char ch){ return !std::isspace(static_cast<unsigned char>(ch)); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    return s;
}

// 仅从当前工作目录读取 ecmpProbability.txt，加载所有节点权重
// 文件格式（每行一个节点）：
//   nodeId: w1 w2 w3 ...
// 或 nodeId w1 w2 w3 ...
// 若多次定义同一 nodeId，后者覆盖前者
static bool
LoadAllNodeWeightsFile()
{
    if (g_weightsLoaded) return !g_nodeRawWeights.empty();

    std::ifstream fin("ecmpProbability.txt");
    if (!fin.good())
    {
        NS_LOG_WARN("ECMP 权重文件无法打开: ecmpProbability.txt");
        g_weightsLoaded = true;
        return false;
    }

    std::string line;
    uint32_t lineNo = 0;
    uint32_t okCnt = 0;
    while (std::getline(fin, line))
    {
        ++lineNo;
        std::string s = StripCommentAndTrim(line);
        if (s.empty()) continue;

        std::istringstream iss(s);
        std::string key;
        iss >> key;
        if (!iss) continue;

        // 解析 nodeId
        if (!key.empty() && key.back() == ':')
        {
            key.pop_back();
        }
        errno = 0;
        char* endp = nullptr;
        unsigned long nid64 = std::strtoul(key.c_str(), &endp, 10);
        if (errno != 0 || endp == key.c_str())
        {
            NS_LOG_WARN("ecmpProbability.txt 第 " << lineNo << " 行节点号解析失败: " << key);
            continue;
        }
        uint32_t nodeId = static_cast<uint32_t>(nid64);

        // 解析权重序列
        std::vector<uint32_t> w;
        std::string tok;
        while (iss >> tok)
        {
            errno = 0;
            char* ep = nullptr;
            unsigned long long val = std::strtoull(tok.c_str(), &ep, 10);
            if (errno != 0 || ep == tok.c_str())
            {
                NS_LOG_WARN("ecmpProbability.txt 第 " << lineNo << " 行存在无法解析的权重: " << tok);
                continue;
            }
            if (val > std::numeric_limits<uint32_t>::max())
            {
                NS_LOG_WARN("ecmpProbability.txt 第 " << lineNo << " 行权重过大, 截断: " << val);
                val = std::numeric_limits<uint32_t>::max();
            }
            w.push_back(static_cast<uint32_t>(val));
        }

        if (w.empty())
        {
            NS_LOG_WARN("ecmpProbability.txt 第 " << lineNo << " 行无有效权重");
            continue;
        }

        g_nodeRawWeights[nodeId] = std::move(w);
        ++okCnt;
    }

    g_weightsLoaded = true;
    if (okCnt == 0)
    {
        NS_LOG_WARN("ecmpProbability.txt 中未加载到任何节点权重");
        return false;
    }
    NS_LOG_INFO("已加载节点权重条目数: " << okCnt << " 来自 ecmpProbability.txt");
    return true;
}

// 获取某节点在候选数 N 下的权重前缀；失败返回 false（回退等权）
static bool
GetNodeWeights(uint32_t nodeId, size_t n, std::vector<uint32_t>& out)
{
    auto key = std::make_pair(nodeId, n);
    auto it = g_ecmpWeightCacheByNodeN.find(key);
    if (it != g_ecmpWeightCacheByNodeN.end())
    {
        out = it->second;
        return true;
    }

    if (!g_weightsLoaded)
    {
        LoadAllNodeWeightsFile();
    }

    auto itRaw = g_nodeRawWeights.find(nodeId);
    if (itRaw == g_nodeRawWeights.end())
    {
        // 未配置该节点
        return false;
    }
    const auto& raw = itRaw->second;
    if (raw.size() < n)
    {
        NS_LOG_WARN("节点 " << nodeId << " 权重数量不足: 需要 " << n << " 实际 " << raw.size());
        return false;
    }

    std::vector<uint32_t> w(raw.begin(), raw.begin() + n);
    uint64_t sum = 0;
    for (uint32_t v : w) sum += v;
    if (sum == 0)
    {
        NS_LOG_WARN("节点 " << nodeId << " 权重前缀总和为 0 (N=" << n << ")");
        return false;
    }

    g_ecmpWeightCacheByNodeN.emplace(key, w);
    out = std::move(w);
    return true;
}

} // anonymous namespace

TypeId
Ipv4GlobalRouting::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::Ipv4GlobalRouting")
            .SetParent<Object>()
            .SetGroupName("Internet")
            .AddAttribute("RandomEcmpRouting",
                          "Set to true if packets are randomly routed among ECMP; set to false for "
                          "using only one route consistently",
                          BooleanValue(false),
                          MakeBooleanAccessor(&Ipv4GlobalRouting::m_randomEcmpRouting),
                          MakeBooleanChecker())
            .AddAttribute("RespondToInterfaceEvents",
                          "Set to true if you want to dynamically recompute the global routes upon "
                          "Interface notification events (up/down, or add/remove address)",
                          BooleanValue(false),
                          MakeBooleanAccessor(&Ipv4GlobalRouting::m_respondToInterfaceEvents),
                          MakeBooleanChecker())
            .AddAttribute("PerflowEcmpRouting",
                          "Set to true to select ECMP path via a stable hash of the flow "
                          "(prefer TCP/UDP 5-tuple; fallback to 3-tuple src/dst/proto). "
                          "When true, this overrides RandomEcmpRouting.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&Ipv4GlobalRouting::m_perflowEcmpRouting),
                          MakeBooleanChecker());
    return tid;
}

Ipv4GlobalRouting::Ipv4GlobalRouting()
    : m_randomEcmpRouting(false),
      m_respondToInterfaceEvents(false),
      m_perflowEcmpRouting(false)
{
    NS_LOG_FUNCTION(this);
    m_rand = CreateObject<UniformRandomVariable>();
}

Ipv4GlobalRouting::~Ipv4GlobalRouting()
{
    NS_LOG_FUNCTION(this);
}

void
Ipv4GlobalRouting::AddHostRouteTo(Ipv4Address dest, Ipv4Address nextHop, uint32_t interface)
{
    NS_LOG_FUNCTION(this << dest << nextHop << interface);
    auto route = new Ipv4RoutingTableEntry();
    *route = Ipv4RoutingTableEntry::CreateHostRouteTo(dest, nextHop, interface);
    m_hostRoutes.push_back(route);
}

void
Ipv4GlobalRouting::AddHostRouteTo(Ipv4Address dest, uint32_t interface)
{
    NS_LOG_FUNCTION(this << dest << interface);
    auto route = new Ipv4RoutingTableEntry();
    *route = Ipv4RoutingTableEntry::CreateHostRouteTo(dest, interface);
    m_hostRoutes.push_back(route);
}

void
Ipv4GlobalRouting::AddNetworkRouteTo(Ipv4Address network,
                                     Ipv4Mask networkMask,
                                     Ipv4Address nextHop,
                                     uint32_t interface)
{
    NS_LOG_FUNCTION(this << network << networkMask << nextHop << interface);
    auto route = new Ipv4RoutingTableEntry();
    *route = Ipv4RoutingTableEntry::CreateNetworkRouteTo(network, networkMask, nextHop, interface);
    m_networkRoutes.push_back(route);
}

void
Ipv4GlobalRouting::AddNetworkRouteTo(Ipv4Address network, Ipv4Mask networkMask, uint32_t interface)
{
    NS_LOG_FUNCTION(this << network << networkMask << interface);
    auto route = new Ipv4RoutingTableEntry();
    *route = Ipv4RoutingTableEntry::CreateNetworkRouteTo(network, networkMask, interface);
    m_networkRoutes.push_back(route);
}

void
Ipv4GlobalRouting::AddASExternalRouteTo(Ipv4Address network,
                                        Ipv4Mask networkMask,
                                        Ipv4Address nextHop,
                                        uint32_t interface)
{
    NS_LOG_FUNCTION(this << network << networkMask << nextHop << interface);
    auto route = new Ipv4RoutingTableEntry();
    *route = Ipv4RoutingTableEntry::CreateNetworkRouteTo(network, networkMask, nextHop, interface);
    m_ASexternalRoutes.push_back(route);
}

Ptr<Ipv4Route>
Ipv4GlobalRouting::LookupGlobal(Ipv4Address dest,
                                Ptr<NetDevice> oif,
                                const Ipv4Header* hdr,
                                Ptr<const Packet> payload)
{
    Ptr<Ipv4Route> rtentry = nullptr;

    uint32_t nodeId = m_ipv4 ? m_ipv4->GetObject<Node>()->GetId() : 0;

    typedef std::vector<Ipv4RoutingTableEntry*> RouteVec_t;
    RouteVec_t allRoutes;

    // Host routes
    for (auto i = m_hostRoutes.begin(); i != m_hostRoutes.end(); i++)
    {
        NS_ASSERT((*i)->IsHost());
        if ((*i)->GetDest() == dest)
        {
            if (oif && oif != m_ipv4->GetNetDevice((*i)->GetInterface()))
            {
                continue;
            }
            allRoutes.push_back(*i);
        }
    }

    // Network routes
    if (allRoutes.empty())
    {
        for (auto j = m_networkRoutes.begin(); j != m_networkRoutes.end(); j++)
        {
            Ipv4Mask mask = (*j)->GetDestNetworkMask();
            Ipv4Address entry = (*j)->GetDestNetwork();
            if (mask.IsMatch(dest, entry))
            {
                if (oif && oif != m_ipv4->GetNetDevice((*j)->GetInterface()))
                {
                    continue;
                }
                allRoutes.push_back(*j);
            }
        }
    }

    // External routes
    if (allRoutes.empty())
    {
        for (auto k = m_ASexternalRoutes.begin(); k != m_ASexternalRoutes.end(); k++)
        {
            Ipv4Mask mask = (*k)->GetDestNetworkMask();
            Ipv4Address entry = (*k)->GetDestNetwork();
            if (mask.IsMatch(dest, entry))
            {
                if (oif && oif != m_ipv4->GetNetDevice((*k)->GetInterface()))
                {
                    continue;
                }
                allRoutes.push_back(*k);
                break;
            }
        }
    }

    if (allRoutes.empty())
    {
        return nullptr;
    }

    uint32_t selectIndex = 0;

    if (allRoutes.size() > 1)
    {
        if (m_perflowEcmpRouting)
        {
            // 6 元组：5 元组 + 节点重盐
            bool ok = false;
            uint64_t h64 = FNV64_OFFSET;

            if (hdr && payload)
            {
                const uint8_t proto = hdr->GetProtocol();
                uint16_t sport = 0, dport = 0;

                if (proto == 6) // TCP
                {
                    TcpHeader th;
                    Ptr<Packet> pkt = payload->Copy();
                    if (pkt->PeekHeader(th))
                    {
                        sport = th.GetSourcePort();
                        dport = th.GetDestinationPort();
                        ok = true;
                    }
                }
                else if (proto == 17) // UDP
                {
                    UdpHeader uh;
                    Ptr<Packet> pkt = payload->Copy();
                    if (pkt->PeekHeader(uh))
                    {
                        sport = uh.GetSourcePort();
                        dport = uh.GetDestinationPort();
                        ok = true;
                    }
                }

                if (ok)
                {
                    // 5 元组按字节混入（64-bit FNV-1a）
                    Fnv64Mix32(h64, hdr->GetSource().Get());
                    Fnv64Mix32(h64, hdr->GetDestination().Get());
                    Fnv64Mix16(h64, sport);
                    Fnv64Mix16(h64, dport);
                    Fnv64MixByte(h64, proto);

                    // 节点重盐（第六元组）
                    uint64_t nodeSalt = GetNodeHeavySalt(m_ipv4);
                    Fnv64Mix64(h64, 0x9e3779b97f4a7c15ull);
                    Fnv64Mix64(h64, nodeSalt);

                    uint32_t h32 = Avalanche64To32(h64);

                    // ---- 加权选择（按节点，从 ./ecmpProbability.txt）----
                    std::vector<uint32_t> weights;
                    if (GetNodeWeights(nodeId, allRoutes.size(), weights))
                    {
                        uint64_t total = 0;
                        for (uint32_t v : weights) total += v;

                        // 无偏缩放
                        uint64_t r = (static_cast<uint64_t>(h32) * total) >> 32;

                        uint64_t acc = 0;
                        size_t idx = 0;
                        for (; idx < weights.size(); ++idx)
                        {
                            acc += weights[idx];
                            if (r < acc) break;
                        }
                        if (idx >= weights.size()) idx = weights.size() - 1;
                        selectIndex = static_cast<uint32_t>(idx);

                        std::ostringstream wss;
                        wss << "[";
                        for (size_t ii = 0; ii < weights.size(); ++ii)
                        {
                            if (ii) wss << ",";
                            wss << weights[ii];
                        }
                        wss << "]";

                        NS_LOG_INFO("ECMP模式: 基于流(加权, 按节点) node=" << nodeId);
                        NS_LOG_INFO("五元组: " << hdr->GetSource() << ":" << sport
                                    << " -> " << hdr->GetDestination() << ":" << dport
                                    << " proto=" << static_cast<unsigned>(proto)
                                    << " hash32=" << h32
                                    << " totalW=" << total
                                    << " r=" << r
                                    << " weights=" << wss.str()
                                    << " 选中index=" << selectIndex);
                    }
                    else
                    {
                        // 权重不可用 -> 等权回退（高质量哈希后的等权映射）
                        selectIndex = (static_cast<uint64_t>(h32) * allRoutes.size()) >> 32;
                        NS_LOG_WARN("ECMP 节点权重不可用（或不足），回退等权 node=" << nodeId
                                    << " hash32=" << h32
                                    << " index=" << selectIndex
                                    << " paths=" << allRoutes.size());
                    }
                    // ---- 加权选择结束 ----
                }
                else
                {
                    NS_LOG_INFO("ECMP模式: 默认 node=" << nodeId);
                    selectIndex = 0;
                }
            }
            else
            {
                NS_LOG_INFO("ECMP模式: 默认 node=" << nodeId);
                selectIndex = 0;
            }
        }
        else if (m_randomEcmpRouting)
        {
            NS_LOG_INFO("ECMP模式: 基于包 node=" << nodeId);
            selectIndex = m_rand->GetInteger(0, allRoutes.size() - 1);
        }
        else
        {
            NS_LOG_INFO("ECMP模式: 默认 node=" << nodeId);
            selectIndex = 0;
        }
    }
    else
    {
        NS_LOG_INFO("ECMP模式: 默认 node=" << nodeId);
        selectIndex = 0;
    }

    Ipv4RoutingTableEntry* route = allRoutes.at(selectIndex);

    rtentry = Create<Ipv4Route>();
    rtentry->SetDestination(route->GetDest());
    rtentry->SetSource(m_ipv4->GetAddress(route->GetInterface(), 0).GetLocal());
    rtentry->SetGateway(route->GetGateway());
    rtentry->SetOutputDevice(m_ipv4->GetNetDevice(route->GetInterface()));
    return rtentry;
}


uint32_t
Ipv4GlobalRouting::GetNRoutes() const
{
    uint32_t n = 0;
    n += m_hostRoutes.size();
    n += m_networkRoutes.size();
    n += m_ASexternalRoutes.size();
    return n;
}

Ipv4RoutingTableEntry*
Ipv4GlobalRouting::GetRoute(uint32_t index) const
{
    if (index < m_hostRoutes.size())
    {
        uint32_t tmp = 0;
        for (auto i = m_hostRoutes.begin(); i != m_hostRoutes.end(); i++)
        {
            if (tmp == index)
            {
                return *i;
            }
            tmp++;
        }
    }
    index -= m_hostRoutes.size();
    uint32_t tmp = 0;
    if (index < m_networkRoutes.size())
    {
        for (auto j = m_networkRoutes.begin(); j != m_networkRoutes.end(); j++)
        {
            if (tmp == index)
            {
                return *j;
            }
            tmp++;
        }
    }
    index -= m_networkRoutes.size();
    tmp = 0;
    for (auto k = m_ASexternalRoutes.begin(); k != m_ASexternalRoutes.end(); k++)
    {
        if (tmp == index)
        {
            return *k;
        }
        tmp++;
    }
    NS_ASSERT(false);
    return nullptr;
}

void
Ipv4GlobalRouting::RemoveRoute(uint32_t index)
{
    if (index < m_hostRoutes.size())
    {
        uint32_t tmp = 0;
        for (auto i = m_hostRoutes.begin(); i != m_hostRoutes.end(); i++)
        {
            if (tmp == index)
            {
                delete *i;
                m_hostRoutes.erase(i);
                return;
            }
            tmp++;
        }
    }
    index -= m_hostRoutes.size();
    uint32_t tmp = 0;
    for (auto j = m_networkRoutes.begin(); j != m_networkRoutes.end(); j++)
    {
        if (tmp == index)
        {
            delete *j;
            m_networkRoutes.erase(j);
            return;
        }
        tmp++;
    }
    index -= m_networkRoutes.size();
    tmp = 0;
    for (auto k = m_ASexternalRoutes.begin(); k != m_ASexternalRoutes.end(); k++)
    {
        if (tmp == index)
        {
            delete *k;
            m_ASexternalRoutes.erase(k);
            return;
        }
        tmp++;
    }
    NS_ASSERT(false);
}

int64_t
Ipv4GlobalRouting::AssignStreams(int64_t stream)
{
    m_rand->SetStream(stream);
    return 1;
}

void
Ipv4GlobalRouting::DoDispose()
{
    for (auto i = m_hostRoutes.begin(); i != m_hostRoutes.end(); i = m_hostRoutes.erase(i))
    {
        delete (*i);
    }
    for (auto j = m_networkRoutes.begin(); j != m_networkRoutes.end(); j = m_networkRoutes.erase(j))
    {
        delete (*j);
    }
    for (auto l = m_ASexternalRoutes.begin(); l != m_ASexternalRoutes.end();
         l = m_ASexternalRoutes.erase(l))
    {
        delete (*l);
    }

    Ipv4RoutingProtocol::DoDispose();
}

void
Ipv4GlobalRouting::PrintRoutingTable(Ptr<OutputStreamWrapper> stream, Time::Unit unit) const
{
    std::ostream* os = stream->GetStream();
    std::ios oldState(nullptr);
    oldState.copyfmt(*os);

    *os << std::resetiosflags(std::ios::adjustfield) << std::setiosflags(std::ios::left);

    *os << "Node: " << m_ipv4->GetObject<Node>()->GetId() << ", Time: " << Now().As(unit)
        << ", Local time: " << m_ipv4->GetObject<Node>()->GetLocalTime().As(unit)
        << ", Ipv4GlobalRouting table" << std::endl;

    if (GetNRoutes() > 0)
    {
        *os << "Destination     Gateway         Genmask         Flags Metric Ref    Use Iface"
            << std::endl;
        for (uint32_t j = 0; j < GetNRoutes(); j++)
        {
            std::ostringstream dest;
            std::ostringstream gw;
            std::ostringstream mask;
            std::ostringstream flags;
            Ipv4RoutingTableEntry route = GetRoute(j);
            dest << route.GetDest();
            *os << std::setw(16) << dest.str();
            gw << route.GetGateway();
            *os << std::setw(16) << gw.str();
            mask << route.GetDestNetworkMask();
            *os << std::setw(16) << mask.str();
            flags << "U";
            if (route.IsHost())
            {
                flags << "H";
            }
            else if (route.IsGateway())
            {
                flags << "G";
            }
            *os << std::setw(6) << flags.str();
            *os << "-" << "      " << "-" << "      " << "-" << "   ";
            if (!Names::FindName(m_ipv4->GetNetDevice(route.GetInterface())).empty())
            {
                *os << Names::FindName(m_ipv4->GetNetDevice(route.GetInterface()));
            }
            else
            {
                *os << route.GetInterface();
            }
            *os << std::endl;
        }
    }
    *os << std::endl;
    (*os).copyfmt(oldState);
}

Ptr<Ipv4Route>
Ipv4GlobalRouting::RouteOutput(Ptr<Packet> p,
                               const Ipv4Header& header,
                               Ptr<NetDevice> oif,
                               Socket::SocketErrno& sockerr)
{
    if (header.GetDestination().IsMulticast())
    {
        return nullptr;
    }
    Ptr<Ipv4Route> rtentry = LookupGlobal(header.GetDestination(), oif, &header, p);
    if (rtentry)
    {
        sockerr = Socket::ERROR_NOTERROR;
    }
    else
    {
        sockerr = Socket::ERROR_NOROUTETOHOST;
    }
    return rtentry;
}

bool
Ipv4GlobalRouting::RouteInput(Ptr<const Packet> p,
                              const Ipv4Header& header,
                              Ptr<const NetDevice> idev,
                              const UnicastForwardCallback& ucb,
                              const MulticastForwardCallback& mcb,
                              const LocalDeliverCallback& lcb,
                              const ErrorCallback& ecb)
{
    NS_ASSERT(m_ipv4->GetInterfaceForDevice(idev) >= 0);
    uint32_t iif = m_ipv4->GetInterfaceForDevice(idev);

    if (m_ipv4->IsDestinationAddress(header.GetDestination(), iif))
    {
        if (!lcb.IsNull())
        {
            lcb(p, header, iif);
            return true;
        }
        return false;
    }

    if (!m_ipv4->IsForwarding(iif))
    {
        ecb(p, header, Socket::ERROR_NOROUTETOHOST);
        return true;
    }

    Ptr<Ipv4Route> rtentry = LookupGlobal(header.GetDestination(), nullptr, &header, p);
    if (rtentry)
    {
        ucb(rtentry, p, header);
        return true;
    }
    return false;
}

void
Ipv4GlobalRouting::NotifyInterfaceUp(uint32_t i)
{
    if (m_respondToInterfaceEvents && Simulator::Now().GetSeconds() > 0)
    {
        GlobalRouteManager::DeleteGlobalRoutes();
        GlobalRouteManager::BuildGlobalRoutingDatabase();
        GlobalRouteManager::InitializeRoutes();
    }
}

void
Ipv4GlobalRouting::NotifyInterfaceDown(uint32_t i)
{
    if (m_respondToInterfaceEvents && Simulator::Now().GetSeconds() > 0)
    {
        GlobalRouteManager::DeleteGlobalRoutes();
        GlobalRouteManager::BuildGlobalRoutingDatabase();
        GlobalRouteManager::InitializeRoutes();
    }
}

void
Ipv4GlobalRouting::NotifyAddAddress(uint32_t interface, Ipv4InterfaceAddress address)
{
    if (m_respondToInterfaceEvents && Simulator::Now().GetSeconds() > 0)
    {
        GlobalRouteManager::DeleteGlobalRoutes();
        GlobalRouteManager::BuildGlobalRoutingDatabase();
        GlobalRouteManager::InitializeRoutes();
    }
}

void
Ipv4GlobalRouting::NotifyRemoveAddress(uint32_t interface, Ipv4InterfaceAddress address)
{
    if (m_respondToInterfaceEvents && Simulator::Now().GetSeconds() > 0)
    {
        GlobalRouteManager::DeleteGlobalRoutes();
        GlobalRouteManager::BuildGlobalRoutingDatabase();
        GlobalRouteManager::InitializeRoutes();
    }
}

void
Ipv4GlobalRouting::SetIpv4(Ptr<Ipv4> ipv4)
{
    NS_ASSERT(!m_ipv4 && ipv4);
    m_ipv4 = ipv4;
}

} // namespace ns3

