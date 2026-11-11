module Probe(
  input         hostClock,
  input         hostReset,
  output        shared_ready,
  input         shared_valid,
  input  [2:0]  shared_bits_addr,
  input         shared_bits_en,
  input         response0_ready,
  output        response0_valid,
  output [16:0] response0_bits,
  output        write_ready,
  input         write_valid,
  input  [16:0] write_bits_data,
  input         write_bits_mask
);
  wire  model_clock;
  wire  model_reset;
  wire  model_channels_read_cmds_0_valid;
  wire [2:0] model_channels_read_cmds_0_bits_addr;
  wire  model_channels_read_resps_0_ready;
  wire  model_channels_read_resps_0_valid;
  wire [16:0] model_channels_read_resps_0_bits;
  wire  model_channels_write_cmds_0_ready;
  wire  model_channels_write_cmds_0_valid;
  wire  model_channels_write_cmds_0_bits_en;
  wire  model_channels_write_cmds_0_bits_mask;
  wire [2:0] model_channels_write_cmds_0_bits_addr;
  wire [16:0] model_channels_write_cmds_0_bits_data;
  RamModel model (
    .clock(model_clock),
    .reset(model_reset),
    .channels_read_cmds_0_valid(model_channels_read_cmds_0_valid),
    .channels_read_cmds_0_bits_addr(model_channels_read_cmds_0_bits_addr),
    .channels_read_resps_0_ready(model_channels_read_resps_0_ready),
    .channels_read_resps_0_valid(model_channels_read_resps_0_valid),
    .channels_read_resps_0_bits(model_channels_read_resps_0_bits),
    .channels_write_cmds_0_ready(model_channels_write_cmds_0_ready),
    .channels_write_cmds_0_valid(model_channels_write_cmds_0_valid),
    .channels_write_cmds_0_bits_en(model_channels_write_cmds_0_bits_en),
    .channels_write_cmds_0_bits_mask(model_channels_write_cmds_0_bits_mask),
    .channels_write_cmds_0_bits_addr(model_channels_write_cmds_0_bits_addr),
    .channels_write_cmds_0_bits_data(model_channels_write_cmds_0_bits_data)
  );
  assign shared_ready = model_channels_write_cmds_0_ready & write_valid & write_valid & shared_valid;
  assign response0_valid = model_channels_read_resps_0_valid;
  assign response0_bits = model_channels_read_resps_0_bits;
  assign write_ready = model_channels_write_cmds_0_ready & write_valid & shared_valid & shared_valid;
  assign model_clock = hostClock;
  assign model_reset = hostReset;
  assign model_channels_read_cmds_0_valid = shared_valid;
  assign model_channels_read_cmds_0_bits_addr = shared_bits_addr;
  assign model_channels_read_resps_0_ready = response0_ready;
  assign model_channels_write_cmds_0_valid = write_valid & shared_valid & shared_valid;
  assign model_channels_write_cmds_0_bits_en = shared_bits_en;
  assign model_channels_write_cmds_0_bits_mask = write_bits_mask;
  assign model_channels_write_cmds_0_bits_addr = shared_bits_addr;
  assign model_channels_write_cmds_0_bits_data = write_bits_data;
endmodule
module RamModel(
  input         clock,
  input         reset,
  input         channels_read_cmds_0_valid, // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 54:20]
  input  [2:0]  channels_read_cmds_0_bits_addr, // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 54:20]
  input         channels_read_resps_0_ready, // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 54:20]
  output        channels_read_resps_0_valid, // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 54:20]
  output [16:0] channels_read_resps_0_bits, // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 54:20]
  output        channels_write_cmds_0_ready, // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 54:20]
  input         channels_write_cmds_0_valid, // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 54:20]
  input         channels_write_cmds_0_bits_en, // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 54:20]
  input         channels_write_cmds_0_bits_mask, // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 54:20]
  input  [2:0]  channels_write_cmds_0_bits_addr, // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 54:20]
  input  [16:0] channels_write_cmds_0_bits_data // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 54:20]
);
`ifdef RANDOMIZE_MEM_INIT
  reg [31:0] _RAND_0;
`endif // RANDOMIZE_MEM_INIT
`ifdef RANDOMIZE_REG_INIT
  reg [31:0] _RAND_1;
  reg [31:0] _RAND_2;
  reg [31:0] _RAND_3;
  reg [31:0] _RAND_4;
`endif // RANDOMIZE_REG_INIT
  reg [16:0] data [0:7]; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 63:30]
  wire  data_read_data_async_en; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 63:30]
  wire [2:0] data_read_data_async_addr; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 63:30]
  wire [16:0] data_read_data_async_data; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 63:30]
  wire [16:0] data_MPORT_data; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 63:30]
  wire [2:0] data_MPORT_addr; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 63:30]
  wire  data_MPORT_mask; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 63:30]
  wire  data_MPORT_en; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 63:30]
  reg [1:0] read_state_0; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 75:28]
  wire  _read_access_req_T = read_state_0 == 2'h0; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 77:87]
  wire  read_access_req_0 = read_state_0 == 2'h0 & channels_read_cmds_0_valid; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 77:97]
  reg [16:0] read_data; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 69:34]
  wire  _T_17 = channels_write_cmds_0_ready & channels_write_cmds_0_valid; // @[src/main/scala/chisel3/util/Decoupled.scala 57:35]
  wire  _active_write_en_T = channels_write_cmds_0_bits_en & channels_write_cmds_0_bits_mask; // @[midas/src/main/scala/midas/models/sram/RegfileModel.scala 25:19]
  wire  active_write_en = _T_17 & _active_write_en_T; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 142:21 150:39 153:25]
  wire  _T_2 = ~reset; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 70:50]
  reg [16:0] read_resp_data_0; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 76:28]
  wire  reads_done = read_state_0 != 2'h0; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 84:92]
  wire  _outputs_responded_or_firing_T_1 = channels_read_resps_0_ready & channels_read_resps_0_valid; // @[src/main/scala/chisel3/util/Decoupled.scala 57:35]
  wire  outputs_responded_or_firing = read_state_0 == 2'h3 | _outputs_responded_or_firing_T_1; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 93:29]
  reg  write_complete_0; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 97:27]
  wire  writes_done_or_finishing = write_complete_0 | _T_17; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 104:53]
  wire  advance_cycle = outputs_responded_or_firing & writes_done_or_finishing; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 107:51]
  wire  _T_6 = advance_cycle | reset; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 111:22]
  wire  _channels_read_resps_0_bits_T = read_state_0 == 2'h1; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 124:55]
  wire  _channels_read_resps_0_valid_T_1 = read_state_0 == 2'h2; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 125:79]
  wire  _GEN_17 = _T_17 | write_complete_0; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 147:45 148:25 97:27]
  assign data_read_data_async_en = 1'h1;
  assign data_read_data_async_addr = channels_read_cmds_0_bits_addr;
  assign data_read_data_async_data = data[data_read_data_async_addr]; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 63:30]
  assign data_MPORT_data = channels_write_cmds_0_bits_data;
  assign data_MPORT_addr = channels_write_cmds_0_bits_addr;
  assign data_MPORT_mask = 1'h1;
  assign data_MPORT_en = active_write_en & _T_2;
  assign channels_read_resps_0_valid = _channels_read_resps_0_bits_T | read_state_0 == 2'h2; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 125:62]
  assign channels_read_resps_0_bits = read_state_0 == 2'h1 ? read_data : read_resp_data_0; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 124:40]
  assign channels_write_cmds_0_ready = reads_done & ~write_complete_0; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 144:58]
  always @(posedge clock) begin
    if (data_MPORT_en & data_MPORT_mask) begin
      data[data_MPORT_addr] <= data_MPORT_data; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 63:30]
    end
    if (_T_6) begin // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 127:41]
      read_state_0 <= 2'h0; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 128:21]
    end else if (_read_access_req_T & read_access_req_0) begin // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 129:67]
      read_state_0 <= 2'h1; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 130:21]
    end else if (_channels_read_resps_0_bits_T) begin // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 131:42]
      if (_outputs_responded_or_firing_T_1) begin // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 132:31]
        read_state_0 <= 2'h3;
      end else begin
        read_state_0 <= 2'h2;
      end
    end else if (_channels_read_resps_0_valid_T_1 & _outputs_responded_or_firing_T_1) begin // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 134:76]
      read_state_0 <= 2'h3; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 135:21]
    end
    read_data <= data_read_data_async_data; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 69:34]
    if (!(_T_6)) begin // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 127:41]
      if (!(_read_access_req_T & read_access_req_0)) begin // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 129:67]
        if (read_state_0 == 2'h1) begin // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 124:40]
          read_resp_data_0 <= read_data;
        end
      end
    end
    if (_T_6) begin // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 145:41]
      write_complete_0 <= 1'h0; // @[midas/src/main/scala/midas/models/sram/AsyncMemModel.scala 146:25]
    end else begin
      write_complete_0 <= _GEN_17;
    end
  end
// Register and memory initialization
`ifdef RANDOMIZE_GARBAGE_ASSIGN
`define RANDOMIZE
`endif
`ifdef RANDOMIZE_INVALID_ASSIGN
`define RANDOMIZE
`endif
`ifdef RANDOMIZE_REG_INIT
`define RANDOMIZE
`endif
`ifdef RANDOMIZE_MEM_INIT
`define RANDOMIZE
`endif
`ifndef RANDOM
`define RANDOM $random
`endif
`ifdef RANDOMIZE_MEM_INIT
  integer initvar;
`endif
`ifndef SYNTHESIS
`ifdef FIRRTL_BEFORE_INITIAL
`FIRRTL_BEFORE_INITIAL
`endif
initial begin
  `ifdef RANDOMIZE
    `ifdef INIT_RANDOM
      `INIT_RANDOM
    `endif
    `ifndef VERILATOR
      `ifdef RANDOMIZE_DELAY
        #`RANDOMIZE_DELAY begin end
      `else
        #0.002 begin end
      `endif
    `endif
`ifdef RANDOMIZE_MEM_INIT
  _RAND_0 = {1{`RANDOM}};
  for (initvar = 0; initvar < 8; initvar = initvar+1)
    data[initvar] = _RAND_0[16:0];
`endif // RANDOMIZE_MEM_INIT
`ifdef RANDOMIZE_REG_INIT
  _RAND_1 = {1{`RANDOM}};
  read_state_0 = _RAND_1[1:0];
  _RAND_2 = {1{`RANDOM}};
  read_data = _RAND_2[16:0];
  _RAND_3 = {1{`RANDOM}};
  read_resp_data_0 = _RAND_3[16:0];
  _RAND_4 = {1{`RANDOM}};
  write_complete_0 = _RAND_4[0:0];
`endif // RANDOMIZE_REG_INIT
  `endif // RANDOMIZE
end // initial
`ifdef FIRRTL_AFTER_INITIAL
`FIRRTL_AFTER_INITIAL
`endif
`endif // SYNTHESIS
endmodule
