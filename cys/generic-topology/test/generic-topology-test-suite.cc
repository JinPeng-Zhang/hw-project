/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "ns3/test.h"

using namespace ns3;

class GenericTopologyTestCase1 : public TestCase
{
public:
  GenericTopologyTestCase1 ();
  virtual ~GenericTopologyTestCase1 ();

private:
  virtual void DoRun (void);
};

GenericTopologyTestCase1::GenericTopologyTestCase1 ()
  : TestCase ("GenericTopology test case (does nothing)")
{
}

GenericTopologyTestCase1::~GenericTopologyTestCase1 ()
{
}

void
GenericTopologyTestCase1::DoRun (void)
{
  NS_TEST_ASSERT_MSG_EQ (true, true, "true doesn't equal true for some reason");
}

class GenericTopologyTestSuite : public TestSuite
{
public:
  GenericTopologyTestSuite ();
};

GenericTopologyTestSuite::GenericTopologyTestSuite ()
  : TestSuite ("generic-topology", UNIT)
{
  AddTestCase (new GenericTopologyTestCase1, TestCase::QUICK);
}

static GenericTopologyTestSuite s_genericTopologyTestSuite;
