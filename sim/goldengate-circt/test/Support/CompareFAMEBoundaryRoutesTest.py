#!/usr/bin/env python3
"""A renamed instance is acceptable only after its actual routes match."""
import json
from pathlib import Path
import tempfile

import CompareFAMEBoundaryRoutes as compare


def main():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        golden = root / "golden.fir"
        candidate = root / "candidate.mlir"
        annotations = root / "channels.json"
        golden.write_text("""circuit Wrapper :
  module Wrapper :
    output source : UInt<8>
    input sink : UInt<8>
    output channel_clock : Clock
    output other_clock : Clock
    inst design_ of Design
    inst model of Model
    source <= design_.data
    model.data <= sink
    channel_clock <= design_.clock
    other_clock <= design_.other
  module Design :
    output data : UInt<8>
    output clock : Clock
    output other : Clock
  module Model :
    input data : UInt<8>
""")
        native = """module {
  firrtl.circuit "Wrapper" {
    firrtl.module @Wrapper(out %source: !firrtl.uint<8>, in %sink: !firrtl.uint<8>, out %channel_clock: !firrtl.clock, out %other_clock: !firrtl.clock) {
      %design_data, %design_clock, %design_other = firrtl.instance design @Design(out data: !firrtl.uint<8>, out clock: !firrtl.clock, out other: !firrtl.clock)
      %model_data = firrtl.instance model @Model(in data: !firrtl.uint<8>)
      firrtl.connect %source, %design_data : !firrtl.uint<8>, !firrtl.uint<8>
      firrtl.connect %model_data, %sink : !firrtl.uint<8>, !firrtl.uint<8>
      firrtl.connect %channel_clock, %design_clock : !firrtl.clock, !firrtl.clock
      firrtl.connect %other_clock, %design_other : !firrtl.clock, !firrtl.clock
    }
    firrtl.module @Design(out %data: !firrtl.uint<8>, out %clock: !firrtl.clock, out %other: !firrtl.clock) {}
    firrtl.module @Model(in %data: !firrtl.uint<8>) {}
  }
}
"""
        candidate.write_text(native)
        annotations.write_text(json.dumps([dict(
            **{"class": "midas.passes.fame.FAMEChannelConnectionAnnotation"},
            globalName="pipe", channelInfo={"class": "midas.passes.fame.PipeChannel", "latency": 0},
            clock="~Wrapper|Wrapper>channel_clock", sources=["~Wrapper|Wrapper>source"],
            sinks=["~Wrapper|Wrapper>sink"])]))
        run = lambda: compare.compare(golden, candidate, annotations, annotations)
        result = run()
        assert result["match"] and result["instance_bijection"] == {"model": "model", "design_": "design"}
        # Metadata is unchanged. The actual selected clock output changes.
        candidate.write_text(native.replace("%channel_clock, %design_clock", "%channel_clock, %design_other"))
        assert not run()["match"], "different physical clock route was accepted"
        # A second source cannot be hidden by undirected alias traversal.
        candidate.write_text(native.replace("      firrtl.connect %source, %design_data", "      firrtl.connect %other_clock, %design_clock : !firrtl.clock, !firrtl.clock\n      firrtl.connect %source, %design_data"))
        try:
            run()
        except ValueError as error:
            assert "exactly one driver" in str(error)
        else:
            raise AssertionError("multiple drivers were accepted")
        # Two unmatched instances of the same module cannot be paired by order.
        candidate.write_text(native.replace("      %model_data", "      %other_data, %other_clock2, %other_other = firrtl.instance other @Design(out data: !firrtl.uint<8>, out clock: !firrtl.clock, out other: !firrtl.clock)\n      %model_data", 1))
        try:
            run()
        except ValueError as error:
            assert "ambiguous instance role" in str(error)
        else:
            raise AssertionError("ambiguous instance matching was accepted")
    print("FAME route audit accepts proven aliases and rejects changed clocks, multiple drivers and ambiguous roles")


if __name__ == "__main__":
    main()
