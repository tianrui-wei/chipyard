import "DPI-C" function void cospike_set_sysinfo(
						 input string  isa,
						 input int     pmpregions,
						 input longint mem0_base,
						 input longint mem0_size,
						 input int     nharts,
						 input string  bootrom
						 );

import "DPI-C" function void cospike_cosim(input longint cycle,
                                           input longint hartid,
					   input bit	 has_wdata,
					   input bit	 has_vwdata,
					   input bit	 valid,
					   input longint iaddr,
					   input int	 insn,
					   input bit	 raise_exception,
					   input bit	 raise_interrupt,
					   input longint cause,
					   input longint wdata,
					   input longint vwdata_0,
					   input longint vwdata_1,
					   input longint vwdata_2,
					   input longint vwdata_3,
					   input longint vwdata_4,
					   input longint vwdata_5,
					   input longint vwdata_6,
					   input longint vwdata_7
					   );


module SpikeCosim  #(
		     parameter ISA,
		     parameter PMPREGIONS,
		     parameter MEM0_BASE,
		     parameter MEM0_SIZE,
		     parameter NHARTS,
		     parameter BOOTROM) (
					 input	      clock,
					 input	      reset,

					 input [63:0] cycle,

					 input [63:0] hartid,

					 input	      trace_0_valid,
					 input [63:0] trace_0_iaddr,
					 input [31:0] trace_0_insn,
					 input	      trace_0_exception,
					 input	      trace_0_interrupt,
					 input [63:0] trace_0_cause,
					 input	      trace_0_has_wdata,
					 input	      trace_0_has_vwdata,
					 input [63:0] trace_0_wdata,
					 input [63:0] trace_0_vwdata_0,
					 input [63:0] trace_0_vwdata_1,
					 input [63:0] trace_0_vwdata_2,
					 input [63:0] trace_0_vwdata_3,
					 input [63:0] trace_0_vwdata_4,
					 input [63:0] trace_0_vwdata_5,
					 input [63:0] trace_0_vwdata_6,
					 input [63:0] trace_0_vwdata_7,

					 input	      trace_1_valid,
					 input [63:0] trace_1_iaddr,
					 input [31:0] trace_1_insn,
					 input	      trace_1_exception,
					 input	      trace_1_interrupt,
					 input [63:0] trace_1_cause,
					 input	      trace_1_has_wdata,
					 input	      trace_1_has_vwdata,
					 input [63:0] trace_1_wdata,
					 input [63:0] trace_1_vwdata_0,
					 input [63:0] trace_1_vwdata_1,
					 input [63:0] trace_1_vwdata_2,
					 input [63:0] trace_1_vwdata_3,
					 input [63:0] trace_1_vwdata_4,
					 input [63:0] trace_1_vwdata_5,
					 input [63:0] trace_1_vwdata_6,
					 input [63:0] trace_1_vwdata_7
					 );

   initial begin
      cospike_set_sysinfo(ISA, PMPREGIONS, MEM0_BASE, MEM0_SIZE, NHARTS, BOOTROM);
   end;

   always @(posedge clock) begin
      if (!reset) begin
	 if (trace_0_valid || trace_0_exception || trace_0_cause) begin
	    cospike_cosim(cycle, hartid, trace_0_has_wdata, trace_0_has_vwdata, trace_0_valid, trace_0_iaddr,
			  trace_0_insn, trace_0_exception, trace_0_interrupt, trace_0_cause,
			  trace_0_wdata, 
			  trace_0_vwdata_0,trace_0_vwdata_1,trace_0_vwdata_2,trace_0_vwdata_3,trace_0_vwdata_4,trace_0_vwdata_5,trace_0_vwdata_6,trace_0_vwdata_7);
	 end
	 if (trace_1_valid || trace_1_exception || trace_1_cause) begin
	    cospike_cosim(cycle, hartid, trace_1_has_wdata, trace_1_has_vwdata, trace_1_valid, trace_1_iaddr,
			  trace_1_insn, trace_1_exception, trace_1_interrupt, trace_1_cause,
			  trace_1_wdata,
			  trace_1_vwdata_0,trace_1_vwdata_1,trace_1_vwdata_2,trace_1_vwdata_3,trace_1_vwdata_4,trace_1_vwdata_5,trace_1_vwdata_6,trace_1_vwdata_7);
	 end
      end
   end
endmodule; // CospikeCosim
