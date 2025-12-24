build_lib_example(
  NAME opengym
  SOURCE_FILES opengym/sim.cc
               opengym/rl-env.cc
  LIBRARIES_TO_LINK
    ${libapplications}
    ${libcore}
    ${libflow-monitor}
    ${libinternet}
    ${libopengym}
    ${libpoint-to-point-layout}
    ${libptp}
    ${libpfc}
    ${libgeneric-topology}
    ${libnetwork}
    ${libpoint-to-point}
    ${libtraffic-control}
    ${libcsma}
    ${libmobility}
    ${libinternet-apps}
    ${libnetanim}
)