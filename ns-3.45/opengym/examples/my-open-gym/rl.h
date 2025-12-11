#ifndef RL_H
#define RL_H

#include "ns3/object.h"
#include "ns3/ptr.h"
#include <cstdint>

namespace ns3 {

class MySocketBase;
class MyGymEnv;
class MySocketDerived;

class MySocketDerived : public MySocketBase
{
public:
  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId () const;
  
  MySocketDerived (void);
};

class MyRlBase : public Object
{
public:
  static TypeId GetTypeId (void);
  
  MyRlBase (void);
  MyRlBase (const MyRlBase& sock);
  virtual ~MyRlBase (void);
  
  uint64_t GenerateUuid ();
  void CreateGymEnv();
  void ConnectSocketCallbacks();
  
private:
  Ptr<MySocketBase> m_MySocket;
  Ptr<MyGymEnv> m_MyGymEnv;
};

} // namespace ns3

#endif /* RL_H */