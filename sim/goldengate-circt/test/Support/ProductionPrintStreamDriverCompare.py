#!/usr/bin/env python3
"""Replay complete captured SFC/native Print beats through the real host driver.

Only the simulator MMIO endpoint and FPGA stream endpoint are mocked. The
unchanged synthesized_prints.cc, BridgeDriver, StreamEngine and actual generated
Print constructors execute. Normal DMA bytes and the bytes produced after the
narrow-packet flush are separate inputs. This supporting test does not establish
that supplied captures came from RTL; retain their producer receipts separately.
It never changes source RTL, drivers or golden archives.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


CONSTRUCTORS = re.compile(r'#ifdef (GET_(?:CORE|BRIDGE|MANAGED_STREAM)_CONSTRUCTOR)\n'
                          r'(.*?)#endif // \1', re.S)

DRIVER = r'''
#include "core/config.h"
#include "bridges/synthesized_prints.h"
#include "bridges/cpu_managed_stream.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

// Actual generated declarations/factories, including the native Print helper.
#define GET_INCLUDES
#include "selected.h"
#undef GET_INCLUDES

simif_t::simif_t(const TargetConfig &c):config(c){}
simif_t::~simif_t()=default;
CPUManagedStreamIO &simif_t::get_cpu_managed_stream_io(){std::abort();}
FPGAManagedStreamIO &simif_t::get_fpga_managed_stream_io(){std::abort();}
int simif_t::run(simulation_t &){std::abort();}

struct Trace {
  std::vector<char> normal, narrow;
  size_t npos=0, fpos=0, requested_stream=0, pulls=0, hints=0, writes=0;
  size_t physical_reads=0, acknowledgements=0, ack_readbacks=0;
  int delay=0;
  size_t short_read=0;
  bool ack_supported=true;
  bool flushing=false;
  std::ofstream consumed;
  Trace(const char *run,const char *flush,const char *bytes,int latency)
      :delay(latency),consumed(bytes,std::ios::binary){
    std::ifstream a(run,std::ios::binary),b(flush,std::ios::binary);
    if(!a||!b||!consumed)throw std::runtime_error("capture/output open failed");
    normal.assign(std::istreambuf_iterator<char>(a),{});
    narrow.assign(std::istreambuf_iterator<char>(b),{});
    if(normal.size()%64||narrow.size()%64)throw std::runtime_error("capture is not complete 64-byte beats");
  }
};
#ifdef USE_CPU_MANAGED
struct RealHostIO final:CPUManagedStreamIO{
  Trace &t;size_t count_address=~size_t(0);bool pending=false;
  explicit RealHostIO(Trace &trace):t(trace){}
  bool supports_explicit_ack()const override{return t.ack_supported;}
  void pop(){
    if(t.npos<t.normal.size())t.npos+=64;
    else if(t.flushing&&t.fpos<t.narrow.size())t.fpos+=64;
    else throw std::runtime_error("ACK without a retained device head");
  }
  uint32_t mmio_read(size_t address)override{
    if(!t.ack_supported){std::cerr<<"ACK_IO_BEFORE_HOST_SUPPORT\n";throw std::runtime_error("unsupported backend touched by ACK driver");}
    if(count_address==~size_t(0))count_address=address;
    if(address!=count_address)throw std::runtime_error("wrong stream count address");
    if(pending){pop();pending=false;++t.ack_readbacks;}
    if(t.npos<t.normal.size())return (t.normal.size()-t.npos)/64;
    if(!t.flushing)return 0;
    if(t.delay>0){--t.delay;return 0;}
    return (t.narrow.size()-t.fpos)/64;
  }
  void mmio_write(size_t address,uint32_t value)override{
    if(!t.ack_supported){std::cerr<<"ACK_IO_BEFORE_HOST_SUPPORT\n";throw std::runtime_error("unsupported backend touched by ACK driver");}
    if(t.short_read){std::cerr<<"ACK_AFTER_SHORT_READ\n";throw std::runtime_error("host attempted ACK after a short read");}
    if(address!=count_address||value!=1||pending)throw std::runtime_error("wrong or undrained explicit stream ACK");
    pending=true;++t.acknowledgements;
  }
  size_t cpu_managed_axi4_read(size_t address, char *dest,size_t bytes)override{
    if(!t.ack_supported){std::cerr<<"ACK_IO_BEFORE_HOST_SUPPORT\n";throw std::runtime_error("unsupported backend touched by ACK driver");}
    t.requested_stream=address>>19;
    if(bytes!=64||pending)throw std::runtime_error("F2 host must finish a 64B read and drain its ACK before the next head");
    auto *data=&t.normal;size_t offset=t.npos;
    if(offset==data->size()){if(!t.flushing)throw std::runtime_error("read without data");data=&t.narrow;offset=t.fpos;}
    if(offset+64>data->size())throw std::runtime_error("read beyond retained stream data");
    // Physical reads, including repeated/aligned/speculative ones, have no
    // dequeue side effects. Only the actual generic driver's OCL ACK pops.
#ifdef ACK_COALESCED
    ++t.physical_reads;std::memcpy(dest,data->data()+offset,64);
#else
    for(unsigned lane=0;lane<16;++lane){++t.physical_reads;std::memcpy(dest+4*lane,data->data()+offset+4*lane,4);}
#endif
    t.physical_reads+=ACK_EXTRA_READS;
    size_t received=t.short_read?t.short_read:64;
    if(t.short_read)std::cerr<<"SHORT_DEVICE_READ head_retained=1 bytes="<<received<<"\n";
    t.consumed.write(dest,received);t.consumed.flush();return received;
  }
  size_t cpu_managed_axi4_write(size_t,const char *,size_t)override{throw std::runtime_error("unexpected stream payload write");}
  uint64_t get_beat_bytes()const override{return 64;}
};
#endif
struct Endpoint final:FPGAToCPUStreamDriver {
  Trace &t; unsigned index;
  Endpoint(Trace &trace,unsigned i):t(trace),index(i){}
  void init()override{}
  size_t pull(void *dest,size_t maximum,size_t minimum)override{
    ++t.pulls; t.requested_stream=index;
    auto *bytes=&t.normal;auto *pos=&t.npos;
    if(t.npos==t.normal.size()){
      if(!t.flushing)return 0;
      if(t.delay>0){--t.delay;return 0;}
      bytes=&t.narrow;pos=&t.fpos;
    }
    size_t n=std::min(maximum,bytes->size()-*pos);
    n-=n%64;
    if(n<minimum)return 0;
    if(n){std::memcpy(dest,bytes->data()+*pos,n);t.consumed.write(bytes->data()+*pos,n);t.consumed.flush();*pos+=n;}
    return n;
  }
  void flush()override{++t.hints;}
};
struct Engine:StreamEngine {
  Engine(Trace &t){for(unsigned i=0;i<64;++i)fpga_to_cpu_streams.push_back(std::make_unique<Endpoint>(t,i));}
};
struct Sim:simif_t {
  Trace &t;
#ifdef USE_CPU_MANAGED
  RealHostIO io;
  Sim(const TargetConfig &c,Trace &trace):simif_t(c),t(trace),io(trace){}
  CPUManagedStreamIO &get_cpu_managed_stream_io()override{return io;}
#else
  Sim(const TargetConfig &c,Trace &trace):simif_t(c),t(trace){}
#endif
  void write(size_t addr,uint32_t data)override{
    // The real driver's init emits the five start/end/doneInit writes; its
    // next MMIO write is the narrow flush request. Never invent flush bytes.
    std::cout<<"mmio "<<addr<<" "<<data<<"\n";
    if(t.writes==5&&data==1)t.flushing=true;
    ++t.writes;
  }
  uint32_t read(size_t)override{throw std::runtime_error("unexpected Print MMIO read");}
};
struct Registry {
  Engine &engine;std::unique_ptr<CPUManagedStreamWidget> managed;std::vector<std::unique_ptr<synthesized_prints_t>> prints;
  explicit Registry(Engine &e):engine(e){}
  StreamEngine *get_stream_engine(){if(managed)return managed.get();return &engine;}
  void add_widget(synthesized_prints_t *p){prints.emplace_back(p);}
  void add_widget(CPUManagedStreamWidget *p){managed.reset(p);}
};
int main(int argc,char **argv){
  if(argc<8||argc>10)return 2;
  Trace trace(argv[1],argv[2],argv[4],std::stoi(argv[5]));
  if(argc>=9)trace.short_read=std::stoul(argv[8]);
  if(argc==10)trace.ack_supported=std::stoi(argv[9])!=0;
  TargetConfig config{};Sim simif(config,trace);Engine engine(trace);Registry registry(engine);
  std::vector<std::string> args{std::string("+print-file=")+argv[3]};
#ifdef USE_CPU_MANAGED
#define GET_MANAGED_STREAM_CONSTRUCTOR
#include "selected.h"
#undef GET_MANAGED_STREAM_CONSTRUCTOR
  if(!registry.managed)throw std::runtime_error("actual CPU-managed stream constructor missing");
#endif
#define GET_BRIDGE_CONSTRUCTOR
#include "selected.h"
#undef GET_BRIDGE_CONSTRUCTOR
  unsigned which=std::stoul(argv[6]);
  if(which>=registry.prints.size())throw std::runtime_error("selected Print constructor missing");
  auto &driver=*registry.prints[which];driver.init();
  driver.tick();driver.tick();driver.finish();
  if(trace.npos!=trace.normal.size()||trace.fpos!=trace.narrow.size()||trace.writes!=6)
    throw std::runtime_error("real Print driver did not consume complete captures and flush");
#ifdef USE_CPU_MANAGED
  if(trace.acknowledgements!=(trace.normal.size()+trace.narrow.size())/64||trace.ack_readbacks!=trace.acknowledgements)
    throw std::runtime_error("actual generic driver omitted or duplicated a committed head ACK");
#else
  if(trace.hints!=2)throw std::runtime_error("Print flush hint count differs");
#endif
  int expected=std::stoi(argv[7]);
  if(expected>=0&&trace.requested_stream!=static_cast<size_t>(expected))throw std::runtime_error("actual generated Print stream index differs");
  std::cout<<"driver-complete stream="<<trace.requested_stream<<" normal="<<trace.npos<<" narrow="<<trace.fpos
           <<" pulls="<<trace.pulls<<" flush-hints="<<trace.hints<<" physical-reads="<<trace.physical_reads
           <<" ACKs="<<trace.acknowledgements<<" committed-readbacks="<<trace.ack_readbacks<<"\n";
}
'''


def record(path):
    path = Path(path).resolve()
    return dict(path=str(path), bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def selected_header(path, cpu_managed=False):
    text = path.read_text()
    def select(match):
        body = match[2]
        enabled=('new synthesized_prints_t(' in body or 'goldengate_print::make_print_bridge_' in body
                 or (cpu_managed and 'new CPUManagedStreamWidget(' in body))
        return match[0] if enabled else ''
    selected = CONSTRUCTORS.sub(select, text)
    if not any('synthesized_prints_t(' in m[2] or 'make_print_bridge_' in m[2]
               for m in CONSTRUCTORS.finditer(text)):
        raise ValueError('Actual enabled Print constructor missing: ' + str(path))
    return selected


def capture(path, hex_input):
    if not hex_input:
        value = path.read_bytes()
    else:
        rows = [line.strip().removeprefix('0x') for line in path.read_text().splitlines() if line.strip()]
        if any(re.fullmatch('[0-9a-fA-F]{128}', row) is None for row in rows):
            raise ValueError('Capture needs one conventional 512-bit SV hex word per line')
        value = b''.join(int(row, 16).to_bytes(64, 'little') for row in rows)
    if len(value) % 64:
        raise ValueError('Capture must contain complete 64-byte DMA beats: ' + str(path))
    return value


def command(argv, directory, timeout=180):
    directory.mkdir(parents=True)
    argv = list(map(str, argv))
    with (directory / 'stdout.log').open('wb') as out, (directory / 'stderr.log').open('wb') as err:
        completed = subprocess.run(argv, stdout=out, stderr=err, timeout=timeout)
    result = dict(argv=argv, returncode=completed.returncode, timed_out=False,
                  stdout=record(directory / 'stdout.log'), stderr=record(directory / 'stderr.log'))
    (directory / 'command.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    for name in ('sfc-header', 'circt-header', 'sfc-run', 'circt-run', 'sfc-flush', 'circt-flush', 'output-dir'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--hex-input', action='store_true')
    parser.add_argument('--expected-decoded', type=Path)
    parser.add_argument('--reference-kind', choices=('unverified_capture','sfc_rtl_capture','independent_protocol','native_direct_queue_capture'),
                        default='unverified_capture',help='Capture provenance scope; never infer an SFC producer from equal bytes')
    parser.add_argument('--expected-stream-index', type=int, default=-1)
    parser.add_argument('--print-index', type=int, default=0)
    parser.add_argument('--flush-delay-polls', type=int, default=0)
    parser.add_argument('--token-bytes', type=int, default=16, help='Used only for the argument corruption negative control')
    parser.add_argument('--cpu-managed-host',action='store_true',help='ExecuteactualCPUManagedStreamWidget/FPGAToCPUDriver andexplicitheadACK throughmockonlyIO')
    parser.add_argument('--coalesced-physical-reads',action='store_true',help='Mockonephysical64BPCIreadperhostheadinsteadof16DWORDrequests')
    parser.add_argument('--extra-physical-reads',type=int,default=0,help='Mock0..2extraspeculative/repeatedphysicalreadsperhead; noACKsideeffects')
    args = parser.parse_args()
    if not 0 <= args.flush_delay_polls <= 4 or args.token_bytes < 1:
        parser.error('flush delay must be 0..4 and token bytes positive')
    if not 0<=args.extra_physical_reads<=2:parser.error('extra physical reads must be0..2')
    repo, output = args.repo.resolve(), args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    include = repo / 'sims/firesim/sim/midas/src/main/cc'
    sources = [include / 'bridges/synthesized_prints.cc', include / 'core/widget.cc',
               include / 'core/bridge_driver.cc', include / 'core/stream_engine.cc']
    if args.cpu_managed_host:sources.append(include/'bridges/cpu_managed_stream.cc')
    inputs = [args.sfc_header, args.circt_header, args.sfc_run, args.circt_run, args.sfc_flush,
              args.circt_flush, *sources, Path(__file__)]
    inputs += [include / relative for relative in ('bridges/synthesized_prints.h','bridges/cpu_managed_stream.h', 'core/widget.h',
               'core/bridge_driver.h', 'core/stream_engine.h', 'core/clock_info.h', 'core/simif.h', 'core/config.h')]
    for header in (args.sfc_header,args.circt_header):
        for name in re.findall(r'#include\s+"([^"/]+)"',header.read_text()):
            dependency=header.parent/name
            if dependency.is_file() and dependency not in inputs: inputs.append(dependency)
    if args.expected_decoded: inputs.append(args.expected_decoded)
    before = [record(path) for path in inputs]
    result = dict(passed=False, full_feature_acceptance=False,
        scope='Captured complete SFC/native Print stream byte and actual unchanged host-driver decode/flush comparison; mocked stream/MMIO endpoint, no FPGA/Jasper claim; capture producer must retain separate RTL provenance',
        reference_kind=args.reference_kind,cpu_managed_host=args.cpu_managed_host,
        coalesced_physical_reads=args.coalesced_physical_reads,extra_physical_reads=args.extra_physical_reads,
        originals=before, commands=[])
    decoded, payloads, binaries, prints = {}, {}, {}, {}
    try:
        for kind in ('sfc', 'circt'):
            directory = output / kind
            directory.mkdir()
            header = directory / 'selected.h'
            header.write_text(selected_header(getattr(args, kind + '_header'),args.cpu_managed_host))
            source = directory / 'driver.cpp'
            source.write_text(DRIVER)
            binary = directory / 'driver'
            argv = [repo / '.conda-env/bin/c++', '-std=c++20', '-O1', '-I', directory, '-I', include,
                    '-I', getattr(args,kind+'_header').resolve().parent,
                    '-I', repo / 'generators/firechip/bridgestubs/src/main/cc',
                    '-I', repo / '.conda-env/include', '-I', repo / '.conda-env/riscv-tools/include',
                    source, *sources, '-L', repo / '.conda-env/lib',
                    '-Wl,-rpath,' + str(repo / '.conda-env/lib'), '-lgmp', '-o', binary]
            if args.cpu_managed_host:
                argv+=['-DUSE_CPU_MANAGED','-DACK_EXTRA_READS='+str(args.extra_physical_reads)]
                if args.coalesced_physical_reads:argv.append('-DACK_COALESCED')
            built = command(argv, directory / 'build')
            result['commands'].append(built)
            if built['returncode']:
                raise ValueError(kind + ' real driver compilation failed; inspect ' + str(directory / 'build/stderr.log'))
            run = directory / 'run.bin'
            flush = directory / 'flush.bin'
            run.write_bytes(capture(getattr(args, kind + '_run'), args.hex_input))
            flush.write_bytes(capture(getattr(args, kind + '_flush'), args.hex_input))
            payloads[kind] = (run.read_bytes(), flush.read_bytes())
            prefix = directory / 'PRINT'
            consumed = directory / 'consumed.bin'
            executed = command([binary, run, flush, prefix, consumed, str(args.flush_delay_polls),
                                str(args.print_index), str(args.expected_stream_index)], directory / 'run')
            result['commands'].append(executed)
            prints[kind] = directory / ('PRINT' + str(args.print_index))
            decoded[kind] = prints[kind].read_bytes()
            binaries[kind] = binary
            result[kind] = dict(driver_returncode=executed['returncode'], normal=record(run), narrow_flush=record(flush),
                                decoded=record(prints[kind]), consumed=record(consumed))
        result['complete_bytes_match'] = payloads['sfc'] == payloads['circt']
        result['decoded_records_match'] = decoded['sfc'] == decoded['circt'] and b'CYCLE:' in decoded['sfc']
        result['driver_flush_completed'] = all(result[kind]['driver_returncode'] == 0 for kind in ('sfc', 'circt'))
        result['expected_decoded_match'] = args.expected_decoded is None or decoded['sfc'] == args.expected_decoded.read_bytes()
        # The same real driver must reject missing final narrow beats. A
        # successful shared decoder must also notice a corrupted argument bit.
        negative = output / 'negative-controls'
        negative.mkdir()
        empty = negative / 'missing-flush.bin'; empty.write_bytes(b'')
        directory = output / 'sfc'
        missing = command([binaries['sfc'], directory / 'run.bin', empty, negative / 'MISSING',
                           negative / 'missing-consumed.bin', '0', str(args.print_index),
                           str(args.expected_stream_index)], negative / 'missing-flush')
        result['commands'].append(missing)
        result['missing_flush_rejected'] = missing['returncode'] != 0 and b'narrow packet not flushed' in Path(missing['stderr']['path']).read_bytes()
        data = bytearray(b''.join(payloads['sfc']))
        index = next((i for i in range(0, len(data), args.token_bytes) if data[i] & 3 == 3), None)
        if index is None:
            raise ValueError('Capture has no enabled first-record token for corruption control')
        data[index] ^= 4
        corrupt_run = negative / 'corrupt-run.bin'; corrupt_flush = negative / 'corrupt-flush.bin'
        split = len(payloads['sfc'][0])
        corrupt_run.write_bytes(data[:split]); corrupt_flush.write_bytes(data[split:])
        corrupt = command([binaries['sfc'], corrupt_run, corrupt_flush, negative / 'CORRUPT',
                           negative / 'corrupt-consumed.bin', '0', str(args.print_index),
                           str(args.expected_stream_index)], negative / 'corrupt-argument')
        result['commands'].append(corrupt)
        corrupted = negative / ('CORRUPT' + str(args.print_index))
        result['argument_corruption_rejected'] = corrupt['returncode'] != 0 or corrupted.read_bytes() != decoded['sfc']
        if args.cpu_managed_host:
            short=command([binaries['sfc'],directory/'run.bin',directory/'flush.bin',negative/'SHORT',
                           negative/'short-consumed.bin','0',str(args.print_index),
                           str(args.expected_stream_index),'60'],negative/'short-read')
            result['commands'].append(short)
            diagnostics=Path(short['stderr']['path']).read_text()
            result['short_read_not_acknowledged']=(short['returncode']!=0 and 'SHORT_DEVICE_READ' in diagnostics
                                                  and 'ACK_AFTER_SHORT_READ' not in diagnostics)
            unsupported=command([binaries['sfc'],directory/'run.bin',directory/'flush.bin',negative/'UNSUPPORTED',
                           negative/'unsupported-consumed.bin','0',str(args.print_index),
                           str(args.expected_stream_index),'0','0'],negative/'unsupported-ack-host')
            result['commands'].append(unsupported)
            result['unsupported_ack_host_rejected_before_io']=(unsupported['returncode']!=0
                and 'ACK_IO_BEFORE_HOST_SUPPORT' not in Path(unsupported['stderr']['path']).read_text())
        result['passed'] = all(result[key] for key in ('complete_bytes_match', 'decoded_records_match',
            'driver_flush_completed', 'expected_decoded_match', 'missing_flush_rejected', 'argument_corruption_rejected'))
        if args.cpu_managed_host:result['passed']&=result['short_read_not_acknowledged'] and result['unsupported_ack_host_rejected_before_io']
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        result['error'] = type(error).__name__ + ': ' + str(error)
    result['originals_unchanged'] = before == [record(path) for path in inputs]
    result['passed'] &= result['originals_unchanged']
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: value for key, value in result.items() if key not in ('originals', 'commands')}, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
