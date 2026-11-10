#!/usr/bin/env python3
# See LICENSE for license details.
"""Check compile-time loopback's native driver adapter against the real API.

The unchanged SFC genHeader is expected to reject its unchanged shared driver:
its done offset/sizeof assertions describe a singleton, but simplenic_t owns
five configuration words as well. Keep this explicit reference defect separate
from the independently compared SFC singleton-MMIO and token/queue semantics.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

DRIVER = r'''
#define GET_INCLUDES
#include @HEADER@
#undef GET_INCLUDES
#include "core/config.h"
#include <cstdio>
#include <stdexcept>
#include <type_traits>
// Mock only the physical simulator. Compile the real widget and bridge driver
// implementations; any MMIO action reaches these counters. No streams exist.
simif_t::simif_t(const TargetConfig &c):config(c){}
simif_t::~simif_t()=default;
CPUManagedStreamIO &simif_t::get_cpu_managed_stream_io(){throw std::runtime_error("unexpected CPU stream access");}
FPGAManagedStreamIO &simif_t::get_fpga_managed_stream_io(){throw std::runtime_error("unexpected FPGA stream access");}
int simif_t::run(simulation_t &){throw std::runtime_error("unexpected simulator run");}
struct Physical final:simif_t{
 static const TargetConfig target; unsigned reads=0,writes=0;
 Physical():simif_t(target){}
 void write(size_t,uint32_t)override{++writes;}
 uint32_t read(size_t)override{++reads;return 0;}
};
const TargetConfig Physical::target{{1,32,32},{1,64,512},1,std::nullopt,std::nullopt,{0,0},"loopback-driver-test"};
static_assert(std::is_base_of_v<bridge_driver_t,simplenic_loopback_t>);
static_assert(!std::is_base_of_v<streaming_bridge_driver_t,simplenic_loopback_t>);
int main(){try{Physical simif;widget_registry_t registry;const std::vector<std::string> args{
 "+nic-loopback1","+linklatency0=6405","+shmemportname0=unused"};
#define GET_BRIDGE_CONSTRUCTOR
#include @HEADER@
#undef GET_BRIDGE_CONSTRUCTOR
 auto &widgets=registry.get_all_bridges();auto kinds=registry.get_bridges<simplenic_loopback_t>();
 if(widgets.size()!=2||kinds.size()!=2)throw std::runtime_error("widget count differs");
 for(unsigned i=0;i<2;++i){auto *w=widgets[i];
  if(kinds[i]!=w||kinds[i]->widget_index!=i)
   throw std::runtime_error("widget kind/index differs");
  w->init();for(unsigned cycle=0;cycle<1000;++cycle)w->tick();w->finish();
  if(w->terminate()||w->exit_code())throw std::runtime_error("loopback unexpectedly terminates");}
 if(simif.reads||simif.writes)throw std::runtime_error("free-running loopback driver accessed MMIO");
 std::puts("PASS two independent compile-time loopback driver registrations; 2000 ticks; no MMIO or CPU streams");return 0;
 }catch(const std::exception &e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native-header', type=Path, required=True)
    p.add_argument('--sfc-header', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cxx', default='g++')
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    compiler = Path(__file__).resolve().parents[2]
    sim = compiler.parent
    repo = sim.parents[2]
    includes = [sim / 'midas/src/main/cc',
                repo / 'generators/firechip/bridgestubs/src/main/cc', compiler / 'runtime']
    commands = []
    def run(cmd, name, expected=0):
        cmd = list(map(str, cmd)); commands.append(cmd)
        (out / 'commands.json').write_text(json.dumps(commands, indent=2)+'\n')
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / (name+'.log')).write_bytes(result.stdout)
        assert (result.returncode == 0) == (expected == 0), (name,result.returncode,result.stdout.decode())
        return result
    common = [a.cxx,'-std=c++17',*[x for path in includes for x in ('-I',str(path))]]
    sfc = out / 'unchanged-sfc.cpp'
    sfc.write_text('#include "bridges/simplenic.h"\n#define GET_SUBSTRUCT_CHECKS\n#include '+json.dumps(str(a.sfc_header.resolve()))+'\n')
    rejected = run([*common,'-fsyntax-only',sfc], 'unchanged-sfc-driver-abi', 1)
    assert b'invalid done' in rejected.stdout and b'invalid structure' in rejected.stdout
    src = out / 'native.cpp'
    src.write_text(DRIVER.replace('@HEADER@',json.dumps(str(a.native_header.resolve()))))
    real = [sim/'midas/src/main/cc/core/widget.cc',sim/'midas/src/main/cc/core/bridge_driver.cc',sim/'midas/src/main/cc/core/widget_registry.cc']
    run([*common,'-ffunction-sections','-fdata-sections',src,*real,'-Wl,--gc-sections','-o',out/'native'], 'native-build')
    run([out/'native'], 'native-run')
    # Actual adapter source corruption must trigger the no-host-action check.
    corrupted = out / 'fault/bridges/simplenic_loopback.h'
    corrupted.parent.mkdir(parents=True)
    original = compiler/'runtime/bridges/simplenic_loopback.h'
    text = original.read_text().replace('  const unsigned widget_index;', '  void tick() override { write(0, 1); }\n  const unsigned widget_index;')
    assert text != original.read_text()
    corrupted.write_text(text)
    run([a.cxx,'-I',out/'fault',*common[1:],'-ffunction-sections','-fdata-sections',src,*real,'-Wl,--gc-sections','-o',out/'fault-driver'], 'fault-build')
    fault = run([out/'fault-driver'], 'fault-run',1)
    assert b'free-running loopback driver accessed MMIO' in fault.stdout
    files = [a.native_header.resolve(),a.sfc_header.resolve(),original,*real,
             repo/'generators/firechip/goldengateimplementations/src/main/scala/SimpleNICBridge.scala',
             repo/'generators/firechip/bridgestubs/src/main/cc/bridges/simplenic.h',
             repo/'generators/firechip/bridgestubs/src/main/cc/bridges/simplenic.cc',Path(__file__).resolve()]
    result = dict(passed=True,full_feature_acceptance=False,
        scope='native compile-time-loopback driver contract correction; unchanged SFC genHeader/shared-driver incompatibility retained',
        reference_defect={'done_offset':{'sfc':0,'shared_driver':40},'struct_size':{'sfc':8,'shared_driver':48},
                          'normal_driver_effects':'five absent configuration writes and disabled CPU-stream initialization'},
        production_adapter='free-running distinct driver kind; no MMIO or CPU-stream actions; two widget identities',
        positive='two registrations, 2000 ticks, no host actions',negative='actual unexpected MMIO write rejected',
        remaining=['three undriven SFC pauser fields','full production F2 assembly','separate complete LEC and SEC','enabled F2 Linux packet traffic and clean poweroff'],
        sha256={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in files})
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__ == '__main__':
    main()
