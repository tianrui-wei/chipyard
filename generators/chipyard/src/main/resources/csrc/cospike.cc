#include <cstdint>
#include <vector>
#include <string>
#include <riscv/sim.h>
#include <vpi_user.h>
#include <svdpi.h>
#include <sstream>
#include <set>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <fcntl.h>

#if __has_include ("cospike_dtm.h")
#define COSPIKE_DTM
#include "testchip_dtm.h"
extern testchip_dtm_t* dtm;
bool spike_loadarch_done = false;
#endif

#if __has_include ("mm.h")
#define COSPIKE_SIMDRAM
#include "mm.h"
extern std::map<long long int, backing_data_t> backing_mem_data;
#endif

#define CLINT_BASE (0x2000000)
#define CLINT_SIZE (0x1000)

typedef struct system_info_t {
  std::string isa;
  int pmpregions;
  uint64_t mem0_base;
  uint64_t mem0_size;
  int nharts;
  std::vector<char> bootrom;
};

system_info_t* info = NULL;
sim_t* sim = NULL;
bool cospike_debug;
reg_t tohost_addr = 0;
reg_t fromhost_addr = 0;
std::set<reg_t> magic_addrs;
cfg_t* cfg;

static std::vector<std::pair<reg_t, mem_t*>> make_mems(const std::vector<mem_cfg_t> &layout)
{
  std::vector<std::pair<reg_t, mem_t*>> mems;
  mems.reserve(layout.size());
  for (const auto &cfg : layout) {
    mems.push_back(std::make_pair(cfg.get_base(), new mem_t(cfg.get_size())));
  }
  return mems;
}

extern "C" void cospike_set_sysinfo(char* isa, int pmpregions,
                                    long long int mem0_base, long long int mem0_size,
                                    int nharts,
                                    char* bootrom
                                    ) {
  if (!info) {
    info = new system_info_t;
    // technically the targets aren't zicntr compliant, but they implement the zicntr registers
    info->isa = std::string(isa) + "_zicntr";
    info->pmpregions = pmpregions;
    info->mem0_base = mem0_base;
    info->mem0_size = mem0_size;
    info->nharts = nharts;
    std::stringstream ss(bootrom);
    std::string s;
    while (ss >> s) {
      info->bootrom.push_back(std::stoi(s));
    }
  }
}

extern "C" void cospike_cosim(long long int cycle,
                              long long int hartid,
                              int has_wdata,
                              int has_vwdata,
                              int valid,
                              long long int iaddr,
                              unsigned long int insn,
                              int raise_exception,
                              int raise_interrupt,
                              unsigned long long int cause,
                              unsigned long long int wdata,
                              int priv,
                              unsigned long long int vwdata_0,
                              unsigned long long int vwdata_1,
                              unsigned long long int vwdata_2,
                              unsigned long long int vwdata_3,
                              unsigned long long int vwdata_4,
                              unsigned long long int vwdata_5,
                              unsigned long long int vwdata_6,
                              unsigned long long int vwdata_7
							  )
{
  assert(info);
  if (unlikely(!sim)) {
    printf("Configuring spike cosim\n");
    std::vector<mem_cfg_t> mem_cfg;
    std::vector<size_t> hartids;
    mem_cfg.push_back(mem_cfg_t(info->mem0_base, info->mem0_size));
    for (int i = 0; i < info->nharts; i++)
      hartids.push_back(i);

    cfg = new cfg_t(std::make_pair(0, 0),
                    nullptr,
                    info->isa.c_str(),
                    "MSU",
                    "vlen:512,elen:64",
                    false,
                    endianness_little,
                    info->pmpregions,
                    mem_cfg,
                    hartids,
                    false,
                    0
                    );

    std::vector<std::pair<reg_t, mem_t*>> mems = make_mems(cfg->mem_layout());

    rom_device_t *boot_rom = new rom_device_t(info->bootrom);
    mem_t *boot_addr_reg = new mem_t(0x1000);
    uint64_t default_boot_addr = 0x80000000;
    boot_addr_reg->store(0, 8, (const uint8_t*)(&default_boot_addr));

    for (auto& mem : mems) {
      if (mem.first == info->mem0_base) {
        std::string path_name = "chipyard-cosim-" + std::to_string(getpid());
        ssize_t mem_size = mem.second->size();
        int shared_fd = shm_open(path_name.c_str(), O_EXCL | O_RDWR, 0600);
        if (shared_fd < 0) {
          std::perror("[mm_t] shm_open for backing storage failed");
          exit(-1);
        }
        uint8_t *data = (uint8_t *) mmap(
          NULL, mem_size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, shared_fd, 0);
        if (data == MAP_FAILED) {
          std::perror("[mm_t] mmap for backing storage failed");
          exit(-1);
        }
        mem.second->store(0, mem_size,(const uint8_t *) data);
        munmap(data, mem_size);
        close(shared_fd);
      }
    }

    // Don't actually build a clint
    mem_t* clint_mem = new mem_t(CLINT_SIZE);

    std::vector<std::pair<reg_t, abstract_device_t*>> plugin_devices;
    // The device map is hardcoded here for now
    plugin_devices.push_back(std::pair(0x4000, boot_addr_reg));
    plugin_devices.push_back(std::pair(0x10000, boot_rom));
    plugin_devices.push_back(std::pair(CLINT_BASE, clint_mem));

    s_vpi_vlog_info vinfo;
    if (!vpi_get_vlog_info(&vinfo))
      abort();
    std::vector<std::string> htif_args;
    bool in_permissive = false;
    cospike_debug = false;
    for (int i = 1; i < vinfo.argc; i++) {
      std::string arg(vinfo.argv[i]);
      if (arg == "+permissive") {
        in_permissive = true;
      } else if (arg == "+permissive-off") {
        in_permissive = false;
      } else if (arg == "+cospike_debug" || arg == "+cospike-debug") {
        cospike_debug = true;
      } else if (!in_permissive) {
        htif_args.push_back(arg);
      }
    }

    debug_module_config_t dm_config = {
      .progbufsize = 2,
      .max_sba_data_width = 0,
      .require_authentication = false,
      .abstract_rti = 0,
      .support_hasel = true,
      .support_abstract_csr_access = true,
      .support_abstract_fpr_access = true,
      .support_haltgroups = true,
      .support_impebreak = true
    };

    printf("isa string is %s\n", info->isa.c_str());
    for (int i = 0; i < htif_args.size(); i++) {
      printf("%s\n", htif_args[i].c_str());
    }

    sim = new sim_t(cfg, false,
                    mems,
                    plugin_devices,
                    htif_args,
                    dm_config,
                    "cospike.log",
                    false,
                    nullptr,
                    false,
                    nullptr
                    );

#ifdef COSPIKE_SIMDRAM
    // match sim_t's backing memory with the SimDRAM memory
    bus_t temp_mem_bus;
    for (auto& pair : mems) temp_mem_bus.add_device(pair.first, pair.second);

    for (auto& pair : backing_mem_data) {
      size_t base = pair.first;
      size_t size = pair.second.size;
      printf("Matching spike memory initial state for region %lx-%lx\n", base, base + size);
      if (!temp_mem_bus.store(base, size, pair.second.data)) {
        printf("Error, unable to match memory at address %lx\n", base);
        abort();
      }
    }
#endif

    sim->configure_log(true, true);
    // Use our own reset vector
    for (int i = 0; i < info->nharts; i++) {
      sim->get_core(hartid)->get_state()->pc = 0x10040;
    }
    sim->set_debug(cospike_debug);
    printf("Setting up htif for spike cosim\n");
    ((htif_t*)sim)->start();
    printf("Spike cosim started\n");
    tohost_addr = ((htif_t*)sim)->get_tohost_addr();
    fromhost_addr = ((htif_t*)sim)->get_fromhost_addr();
    printf("Tohost  : %lx\n", tohost_addr);
    printf("Fromhost: %lx\n", fromhost_addr);
    printf("Memory base  : %lx\n", info->mem0_base);
    printf("Memory Size  : %lx\n", info->mem0_size);
  }

  if (priv & 0x4) { // debug
    return;
  }

  processor_t* p = sim->get_core(hartid);
  state_t* s = p->get_state();
#ifdef COSPIKE_DTM
  if (dtm && dtm->loadarch_done && !spike_loadarch_done) {
    printf("Restoring spike state from testchip_dtm loadarch\n");
    // copy the loadarch state into the cosim
    loadarch_state_t &ls = dtm->loadarch_state[hartid];
    s->pc  = ls.pc;
    s->prv = ls.prv;
    s->csrmap[CSR_MSTATUS]->write(s->csrmap[CSR_MSTATUS]->read() | MSTATUS_VS | MSTATUS_XS | MSTATUS_FS);
#define RESTORE(CSRID, csr) s->csrmap[CSRID]->write(ls.csr);
    RESTORE(CSR_FCSR     , fcsr);
    RESTORE(CSR_VSTART   , vstart);
    RESTORE(CSR_VXSAT    , vxsat);
    RESTORE(CSR_VXRM     , vxrm);
    RESTORE(CSR_VCSR     , vcsr);
    RESTORE(CSR_VTYPE    , vtype);
    RESTORE(CSR_STVEC    , stvec);
    RESTORE(CSR_SSCRATCH , sscratch);
    RESTORE(CSR_SEPC     , sepc);
    RESTORE(CSR_SCAUSE   , scause);
    RESTORE(CSR_STVAL    , stval);
    RESTORE(CSR_SATP     , satp);
    RESTORE(CSR_MSTATUS  , mstatus);
    RESTORE(CSR_MEDELEG  , medeleg);
    RESTORE(CSR_MIDELEG  , mideleg);
    RESTORE(CSR_MIE      , mie);
    RESTORE(CSR_MTVEC    , mtvec);
    RESTORE(CSR_MSCRATCH , mscratch);
    RESTORE(CSR_MEPC     , mepc);
    RESTORE(CSR_MCAUSE   , mcause);
    RESTORE(CSR_MTVAL    , mtval);
    RESTORE(CSR_MIP      , mip);
    RESTORE(CSR_MCYCLE   , mcycle);
    RESTORE(CSR_MINSTRET , minstret);
    if (ls.VLEN != p->VU.VLEN) {
      printf("VLEN mismatch loadarch: $d != spike: $d\n", ls.VLEN, p->VU.VLEN);
      abort();
    }
    if (ls.ELEN != p->VU.ELEN) {
      printf("ELEN mismatch loadarch: $d != spike: $d\n", ls.ELEN, p->VU.ELEN);
      abort();
    }
    for (size_t i = 0; i < 32; i++) {
      s->XPR.write(i, ls.XPR[i]);
      s->FPR.write(i, { (uint64_t)ls.FPR[i], (uint64_t)-1 });
      memcpy(p->VU.reg_file + i * ls.VLEN / 8, ls.VPR[i], ls.VLEN / 8);
    }
    spike_loadarch_done = true;
    p->clear_waiting_for_interrupt();
  }
#endif
  uint64_t s_pc = s->pc;
  uint64_t interrupt_cause = cause & 0x7FFFFFFFFFFFFFFF;
  bool ssip_interrupt = interrupt_cause == 0x1;
  bool msip_interrupt = interrupt_cause == 0x3;
  bool debug_interrupt = interrupt_cause == 0xe;
  if (raise_interrupt) {
    printf("%d interrupt %lx\n", cycle, cause);

    if (ssip_interrupt) {
      // do nothing
    } else if (msip_interrupt) {
      s->mip->backdoor_write_with_mask(MIP_MSIP, MIP_MSIP);
    } else if (debug_interrupt) {
      return;
    } else {
      printf("Unknown interrupt %lx\n", interrupt_cause);
      abort();
    }
  }
  if (raise_exception)
    printf("%d exception %lx\n", cycle, cause);
  if (valid) {
    p->clear_waiting_for_interrupt();
    printf("%d Cosim: %lx", cycle, iaddr);
    if (has_wdata) {
      printf(" s: %lx", wdata);
    }
    if (has_vwdata) {
      printf(" v:");
      printf(" %llx", vwdata_0);
      printf(" %llx", vwdata_1);
      printf(" %llx", vwdata_2);
      printf(" %llx", vwdata_3);
      printf(" %llx", vwdata_4);
      printf(" %llx", vwdata_5);
      printf(" %llx", vwdata_6);
      printf(" %llx", vwdata_7);
    }
    printf("\n");
  }
  if (valid || raise_interrupt || raise_exception) {
    p->step(1);
    if (unlikely(cospike_debug)) {
      printf("spike pc is %lx\n", s->pc);
      printf("spike mstatus is %lx\n", s->mstatus->read());
      printf("spike mip is %lx\n", s->mip->read());
      printf("spike mie is %lx\n", s->mie->read());
      printf("spike wfi state is %d\n", p->is_waiting_for_interrupt());
    }
  }

  if (valid && !raise_exception) {
    if (s_pc != iaddr) {
      printf("%d PC mismatch spike %llx != DUT %llx\n", cycle, s_pc, iaddr);
      if (unlikely(cospike_debug)) {
        printf("spike mstatus is %lx\n", s->mstatus->read());
        printf("spike mcause is %lx\n", s->mcause->read());
        printf("spike mtval is %lx\n" , s->mtval->read());
        printf("spike mtinst is %lx\n", s->mtinst->read());
      }
      exit(1);
    }


    auto& mem_write = s->log_mem_write;
    auto& log = s->log_reg_write;
    auto& mem_read = s->log_mem_read;

    for (auto memwrite : mem_write) {
      reg_t waddr = std::get<0>(memwrite);
      uint64_t w_data = std::get<1>(memwrite);
      if ((waddr == CLINT_BASE + 4*hartid) && w_data == 0) {
        s->mip->backdoor_write_with_mask(MIP_MSIP, 0);
      }
      // Try to remember magic_mem addrs, and ignore these in the future
      if ( waddr == tohost_addr && w_data >= info->mem0_base && w_data < (info->mem0_base + info->mem0_size)) {
        printf("Probable magic mem %lx\n", w_data);
        magic_addrs.insert(w_data);
      }
    }

    bool scalar_wb = false;
    bool vector_wb = false;
    uint32_t vector_cnt = 0;
    uint32_t vector_pre = 0;

    for (auto &regwrite : log) {

      // if (regwrite.first == 0) continue;

      //TODO: scaling to multi issue reads?
      reg_t mem_read_addr = mem_read.empty() ? 0 : std::get<0>(mem_read[0]);

      int rd = regwrite.first >> 4;
      int type = regwrite.first & 0xf;
      // 0 => int
      // 1 => fp
      // 2 => vec
      // 3 => vec hint
      // 4 => csr

      bool ignore_read = (!mem_read.empty() &&
                          ((magic_addrs.count(mem_read_addr) ||
                            (tohost_addr && mem_read_addr == tohost_addr) ||
                            (fromhost_addr && mem_read_addr == fromhost_addr) ||
                            (CLINT_BASE <= mem_read_addr &&
                             mem_read_addr < (CLINT_BASE + CLINT_SIZE)))));

      // check the type is compliant with writeback first
      if ((type == 0 || type == 1))
        scalar_wb = true;
      if (type == 2) {
        vector_wb = true;
        vector_pre++;
      }
      if (type == 3 && !vector_wb) {
        std::perror("cospike internal error: no vector write back\n");
        exit(-1);
      }


      if ((rd != 0 && type == 0) || type == 1) {
        // Override reads from some CSRs
        uint64_t csr_addr = (insn >> 20) & 0xfff;
        bool csr_read = (insn & 0x7f) == 0x73;
        if (csr_read)
          printf("CSR read %lx\n", csr_addr);
        if (csr_read && ((csr_addr == 0xf13) ||                   // mimpid
                         (csr_addr == 0xf12) ||                   // marchid
                         (csr_addr == 0xf11) ||                   // mvendorid
                         (csr_addr == 0xb00) ||                   // mcycle
                         (csr_addr == 0xb02) ||                   // minstret
                         (csr_addr >= 0x3b0 && csr_addr <= 0x3ef) // pmpaddr
                         )) {
          printf("CSR override\n");
          s->XPR.write(rd, wdata);
        } else if (ignore_read)  {
          // Don't check reads from tohost, reads from magic memory, or reads
          // from clint Technically this could be buggy because log_mem_read
          // only reports vaddrs, but no software ever should access
          // tohost/fromhost/clint with vaddrs anyways
          printf("Read override %lx\n", mem_read_addr);
          s->XPR.write(rd, wdata);
        } else if (wdata != regwrite.second.v[0]) {
          printf("%d wdata mismatch reg %d %lx != %lx\n", cycle, rd,
                 regwrite.second.v[0], wdata);
          exit(1);
        }
      }

      // ignore the case where type is 2
      // a 3 would only be followed by a 2
      if (type == 3) {
        vector_cnt++;
        // type 3 only signals the following groups are vector, we ignore it for now
        int size = p->VU.VLEN;
        if(((size-1) & size) != 0) {
          const uint64_t *arr = (const uint64_t*) &p->VU.elt<uint8_t>(rd, 0);
          for (int idx = size / 64 -1; idx >= 0; --idx) {
            if (idx == 7) {printf("vwdata 0 is %lld, spike commit data is %lld\n", vwdata_0, arr[idx]);}
          }
        }
      }
      if (type == 3) continue;


      if ((rd != 0 && type == 0) || type == 1) {
        // Override reads from some CSRs
        uint64_t csr_addr = (insn >> 20) & 0xfff;
        bool csr_read = (insn & 0x7f) == 0x73;
        if (csr_read)
          printf("CSR read %lx\n", csr_addr);
        if (csr_read && ((csr_addr == 0xf13) ||                      // mimpid
                         (csr_addr == 0xf12) ||                      // marchid
                         (csr_addr == 0xf11) ||                      // mvendorid
                         (csr_addr == 0xb00) ||                      // mcycle
                         (csr_addr == 0xb02) ||                      // minstret
                         (csr_addr >= 0x7a0 && csr_addr <= 0x7aa) || // debug trigger registers
                         (csr_addr >= 0x3b0 && csr_addr <= 0x3ef)    // pmpaddr
                         )) {
          printf("CSR override\n");
          s->XPR.write(rd, wdata);
        } else if (ignore_read)  {
          // Don't check reads from tohost, reads from magic memory, or reads
          // from clint Technically this could be buggy because log_mem_read
          // only reports vaddrs, but no software ever should access
          // tohost/fromhost/clint with vaddrs anyways
          printf("Read override %lx\n", mem_read_addr);
          s->XPR.write(rd, wdata);
        } else if (wdata != regwrite.second.v[0]) {
          printf("%d wdata mismatch reg %d %lx != %lx\n", cycle, rd,
                 regwrite.second.v[0], wdata);
          exit(1);
        }
      }

      // TODO FIX: Rocketchip TracedInstruction.wdata should be Valid(UInt)
      // if (scalar_wb ^ has_wdata) {
      //   printf("Scalar wdata behavior divergence between spike and DUT\n");
      //   exit(-1);
      // }
    }

    if (scalar_wb ^ has_wdata) {
      printf("Scalar behavior divergence between spike and DUT\n");
      exit(-1);
    }

    if (vector_wb ^ has_vwdata) {
      printf("vector behavior divergence between spike and DUT\n");
      exit(-1);
    }
#ifdef SPIKE_DEBUG
    if (vector_wb) {
      printf("vector_cnt = %x\n", vector_cnt);
      printf("vector_pre = %x\n", vector_pre);
    }
#endif
  }
}
