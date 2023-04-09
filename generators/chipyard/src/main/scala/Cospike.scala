package chipyard

import chisel3._
import chisel3.experimental.{IntParam, StringParam, IO}
import chisel3.util._

import org.chipsalliance.cde.config.{Parameters, Field, Config}
import freechips.rocketchip.subsystem._
import freechips.rocketchip.diplomacy._
import freechips.rocketchip.devices.tilelink._
import freechips.rocketchip.util._

import testchipip.TileTraceIO

case class SpikeCosimConfig(
  isa: String,
  pmpregions: Int,
  mem0_base: BigInt,
  mem0_size: BigInt,
  nharts: Int,
  bootrom: String,
  has_dtm: Boolean
)

class SpikeCosim(cfg: SpikeCosimConfig) extends BlackBox(Map(
  "ISA" -> StringParam(cfg.isa),
  "PMPREGIONS" -> IntParam(cfg.pmpregions),
  "MEM0_BASE" -> IntParam(cfg.mem0_base),
  "MEM0_SIZE" -> IntParam(cfg.mem0_size),
  "NHARTS" -> IntParam(cfg.nharts),
  "BOOTROM" -> StringParam(cfg.bootrom)
)) with HasBlackBoxResource
{
  addResource("/csrc/cospike.cc")
  addResource("/vsrc/cospike.v")
  if (cfg.has_dtm) addResource("/csrc/cospike_dtm.h")
  val io = IO(new Bundle {
    val clock = Input(Clock())
    val reset = Input(Bool())
    val cycle = Input(UInt(64.W))
    val hartid = Input(UInt(64.W))
    val trace = Input(Vec(2, new Bundle {
      val valid = Bool()
      val iaddr = UInt(64.W)
      val insn = UInt(32.W)
      val exception = Bool()
      val interrupt = Bool()
      val cause = UInt(64.W)
      val has_wdata = Bool()
      val has_vwdata = Bool()
      val wdata = UInt(64.W)
      val priv = UInt(3.W)
      val vwdata = Vec(8, UInt(64.W))
    }))
  })
}

object SpikeCosim
{
  def apply(trace: TileTraceIO, hartid: Int, cfg: SpikeCosimConfig) = {
    val cosim = Module(new SpikeCosim(cfg))
    val cycle = withClockAndReset(trace.clock, trace.reset) {
      val r = RegInit(0.U(64.W))
      r := r + 1.U
      r
    }
    cosim.io.clock := trace.clock
    cosim.io.reset := trace.reset
    require(trace.numInsns <= 2)
    cosim.io.cycle := cycle
    cosim.io.trace.map(t => {
      t := DontCare
      t.valid := false.B
    })
    cosim.io.hartid := hartid.U
    for (i <- 0 until trace.numInsns) {
      val insn = trace.trace.insns(i)
      cosim.io.trace(i).valid := insn.valid
      val signed = Wire(SInt(64.W))
      signed := insn.iaddr.asSInt
      cosim.io.trace(i).iaddr := signed.asUInt
      cosim.io.trace(i).insn := trace.insns(i).insn
      cosim.io.trace(i).exception := trace.insns(i).exception
      cosim.io.trace(i).interrupt := trace.insns(i).interrupt
      cosim.io.trace(i).cause := trace.insns(i).cause
      cosim.io.trace(i).has_wdata := trace.insns(i).swb && trace.insns(i).wdata.isDefined.B
      cosim.io.trace(i).has_vwdata := trace.insns(i).vwb && trace.insns(i).vwdata.isDefined.B
      cosim.io.trace(i).wdata := trace.insns(i).wdata.getOrElse(0.U)
      cosim.io.trace(i).vwdata := trace.insns(i).vwdata.getOrElse(0.U).asTypeOf(Vec(8, UInt(64.W)))
      cosim.io.trace(i).priv := insn.priv
    }
  }
}
