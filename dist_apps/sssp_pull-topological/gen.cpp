/** SSSP -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2013, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @section Description
 *
 * Compute SSSP pull on distributed Galois.
 *
 * @author Gurbinder Gill <gurbinder533@gmail.com>
 * @author Roshan Dathathri <roshan@cs.utexas.edu>
 * @author Loc Hoang <l_hoang@utexas.edu> (sanity check operators)
 */

#include <iostream>
#include <limits>
#include "Galois/Galois.h"
#include "Galois/gstl.h"
#include "Lonestar/BoilerPlate.h"
#include "Galois/Runtime/CompilerHelperFunctions.h"

#include "Galois/Runtime/dGraph_edgeCut.h"
#include "Galois/Runtime/dGraph_cartesianCut.h"
#include "Galois/Runtime/dGraph_hybridCut.h"

#include "Galois/DistAccumulator.h"
#include "Galois/Runtime/Tracer.h"

enum VertexCut {
  PL_VCUT, CART_VCUT
};

#ifdef __GALOIS_HET_CUDA__
#include "Galois/Runtime/Cuda/cuda_device.h"
#include "gen_cuda.h"
struct CUDA_Context *cuda_ctx;

enum Personality {
   CPU, GPU_CUDA, GPU_OPENCL
};
std::string personality_str(Personality p) {
   switch (p) {
   case CPU:
      return "CPU";
   case GPU_CUDA:
      return "GPU_CUDA";
   case GPU_OPENCL:
      return "GPU_OPENCL";
   }
   assert(false&& "Invalid personality");
   return "";
}
#endif

static const char* const name = "SSSP pull - Distributed Heterogeneous";
static const char* const desc = "SSSP pull on Distributed Galois.";
static const char* const url = 0;

/******************************************************************************/
/* Declaration of command line arguments */
/******************************************************************************/

namespace cll = llvm::cl;
static cll::opt<std::string> inputFile(cll::Positional, cll::desc("<input file (Transpose graph)>"), cll::Required);
static cll::opt<std::string> partFolder("partFolder", cll::desc("path to partitionFolder"), cll::init(""));
static cll::opt<bool> transpose("transpose", cll::desc("transpose the graph in memory after partitioning"), cll::init(false));
static cll::opt<unsigned int> maxIterations("maxIterations", cll::desc("Maximum iterations: Default 1000"), cll::init(1000));
static cll::opt<unsigned long long> src_node("srcNodeId", cll::desc("ID of the source node"), cll::init(0));
static cll::opt<bool> verify("verify", cll::desc("Verify ranks by printing to 'page_ranks.#hid.csv' file"), cll::init(false));

static cll::opt<bool> enableVCut("enableVertexCut", cll::desc("Use vertex cut for graph partitioning."), cll::init(false));

static cll::opt<unsigned int> VCutThreshold("VCutThreshold", cll::desc("Threshold for high degree edges."), cll::init(1000));
static cll::opt<VertexCut> vertexcut("vertexcut", cll::desc("Type of vertex cut."),
       cll::values(clEnumValN(PL_VCUT, "pl_vcut", "Powerlyra Vertex Cut"), clEnumValN(CART_VCUT , "cart_vcut", "Cartesian Vertex Cut"), clEnumValEnd),
       cll::init(PL_VCUT));

#ifdef __GALOIS_HET_CUDA__
static cll::opt<int> gpudevice("gpu", cll::desc("Select GPU to run on, default is to choose automatically"), cll::init(-1));
static cll::opt<Personality> personality("personality", cll::desc("Personality"),
      cll::values(clEnumValN(CPU, "cpu", "Galois CPU"), clEnumValN(GPU_CUDA, "gpu/cuda", "GPU/CUDA"), clEnumValN(GPU_OPENCL, "gpu/opencl", "GPU/OpenCL"), clEnumValEnd),
      cll::init(CPU));
static cll::opt<unsigned> scalegpu("scalegpu", cll::desc("Scale GPU workload w.r.t. CPU, default is proportionally equal workload to CPU and GPU (1)"), cll::init(1));
static cll::opt<unsigned> scalecpu("scalecpu", cll::desc("Scale CPU workload w.r.t. GPU, default is proportionally equal workload to CPU and GPU (1)"), cll::init(1));
static cll::opt<int> num_nodes("num_nodes", cll::desc("Num of physical nodes with devices (default = num of hosts): detect GPU to use for each host automatically"), cll::init(-1));
static cll::opt<std::string> personality_set("pset", cll::desc("String specifying personality for hosts on each physical node. 'c'=CPU,'g'=GPU/CUDA and 'o'=GPU/OpenCL"), cll::init("c"));
#endif

/******************************************************************************/
/* Graph structure declarations + other initialization */
/******************************************************************************/

const uint32_t infinity = std::numeric_limits<uint32_t>::max()/4;

struct NodeData {
  uint32_t dist_current;
};

Galois::DynamicBitSet bitset_dist_current;

typedef hGraph<NodeData, unsigned int> Graph;
typedef hGraph_edgeCut<NodeData, unsigned int> Graph_edgeCut;
typedef hGraph_vertexCut<NodeData, unsigned int> Graph_vertexCut;
typedef hGraph_cartesianCut<NodeData, unsigned int> Graph_cartesianCut;

typedef typename Graph::GraphNode GNode;

#include "gen_sync.hh"

/******************************************************************************/
/* Algorithm structures */
/******************************************************************************/

struct InitializeGraph {
  const uint32_t &local_infinity;
  cll::opt<unsigned long long> &local_src_node;
  Graph *graph;

  InitializeGraph(cll::opt<unsigned long long> &_src_node, 
                  const uint32_t &_infinity, Graph* _graph) : 
                    local_infinity(_infinity), local_src_node(_src_node), 
                    graph(_graph){}

  void static go(Graph& _graph){
    #ifdef __GALOIS_HET_CUDA__
      if (personality == GPU_CUDA) {
        std::string impl_str("CUDA_DO_ALL_IMPL_InitializeGraph_" + 
                             (_graph.get_run_identifier()));
        Galois::StatTimer StatTimer_cuda(impl_str.c_str());
        StatTimer_cuda.start();
        InitializeGraph_all_cuda(infinity, src_node, cuda_ctx);
        StatTimer_cuda.stop();
      } else if (personality == CPU)
    #endif
    {
    Galois::do_all(_graph.begin(), _graph.end(), 
                   InitializeGraph {src_node, infinity, &_graph}, 
                   Galois::loopname("InitializeGraph"), 
                   Galois::numrun(_graph.get_run_identifier()));
    }
    _graph.sync<writeSource, readAny, Reduce_set_dist_current, 
                Broadcast_dist_current, Bitset_dist_current>("InitializeGraph");
    
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    sdata.dist_current = (graph->getGID(src) == local_src_node) ? 0 : local_infinity;
    bitset_dist_current.set(src);
  }
};

struct SSSP {
  Graph* graph;
  static Galois::DGAccumulator<int> DGAccumulator_accum;

  SSSP(Graph* _graph) : graph(_graph){}

  void static go(Graph& _graph){
    unsigned _num_iterations = 0;

    do{
      _graph.set_num_iter(_num_iterations);
      DGAccumulator_accum.reset();
      #ifdef __GALOIS_HET_CUDA__
        if (personality == GPU_CUDA) {
          std::string impl_str("CUDA_DO_ALL_IMPL_SSSP_" + (_graph.get_run_identifier()));
          Galois::StatTimer StatTimer_cuda(impl_str.c_str());
          StatTimer_cuda.start();
          int __retval = 0;
          SSSP_all_cuda(__retval, cuda_ctx);
          DGAccumulator_accum += __retval;
          StatTimer_cuda.stop();
        } else if (personality == CPU)
      #endif
      {
        //Galois::do_all(_graph.begin(), _graph.end(), SSSP (&_graph), 
        //  Galois::loopname("SSSP"), 
        //  Galois::numrun(_graph.get_run_identifier()));
        Galois::do_all_choice(
          Galois::Runtime::makeStandardRange(
            _graph.begin(), 
            _graph.end()
          ), 
          SSSP{ &_graph }, 
          std::make_tuple(Galois::loopname("SSSP"), 
            Galois::thread_range(_graph.get_thread_ranges()),
            Galois::numrun(_graph.get_run_identifier())
        ));
      }
      _graph.sync<writeSource, readDestination, Reduce_min_dist_current, 
                  Broadcast_dist_current, Bitset_dist_current>("SSSP");

      Galois::Runtime::reportStat("(NULL)", 
        "NUM_WORK_ITEMS_" + (_graph.get_run_identifier()), 
        (unsigned long)DGAccumulator_accum.read_local(), 0);
      ++_num_iterations;
    } while ((_num_iterations < maxIterations) && DGAccumulator_accum.reduce());

    if (Galois::Runtime::getSystemNetworkInterface().ID == 0) {
      Galois::Runtime::reportStat("(NULL)", 
        "NUM_ITERATIONS_" + std::to_string(_graph.get_run_num()), 
        (unsigned long)_num_iterations, 0);
    }
  }

  void operator()(GNode src) const {
    NodeData& snode = graph->getData(src);

    for (auto jj = graph->edge_begin(src), ee = graph->edge_end(src); jj != ee; ++jj) {
      GNode dst = graph->getEdgeDst(jj);
      auto& dnode = graph->getData(dst);
      uint32_t new_dist = dnode.dist_current + graph->getEdgeData(jj);
      uint32_t old_dist = Galois::min(snode.dist_current, new_dist);
      if (old_dist > new_dist){
        bitset_dist_current.set(src);
        DGAccumulator_accum += 1;
      }
    }
  }
};

Galois::DGAccumulator<int> SSSP::DGAccumulator_accum;

/******************************************************************************/
/* Sanity check operators */
/******************************************************************************/

/* Prints total number of nodes visited + max distance */
struct SSSPSanityCheck {
  const uint32_t &local_infinity;
  Graph* graph;

  static uint32_t current_max;

  static Galois::DGAccumulator<uint64_t> DGAccumulator_sum;
  static Galois::DGAccumulator<uint32_t> DGAccumulator_max;

  SSSPSanityCheck(const uint32_t _infinity, Graph* _graph) : 
    local_infinity(_infinity), graph(_graph){}

  void static go(Graph& _graph) {
  #ifdef __GALOIS_HET_CUDA__
    if (personality == GPU_CUDA) {
      // TODO currently no GPU support for sanity check operator
      printf("Warning: No GPU support for sanity check; might get "
             "wrong results.\n");
    }
  #endif

    DGAccumulator_sum.reset();
    DGAccumulator_max.reset();

    Galois::do_all(_graph.begin(), _graph.end(), 
                   SSSPSanityCheck(infinity, &_graph), 
                   Galois::loopname("SSSPSanityCheck"));


    uint64_t num_visited = DGAccumulator_sum.reduce();

    DGAccumulator_max = current_max;
    uint32_t max_distance = DGAccumulator_max.reduce_max();

    // Only node 0 will print the info
    if (_graph.id == 0) {
      printf("Number of nodes visited is %lu\n", num_visited);
      printf("Max distance is %u\n", max_distance);
    }
  }

  void operator()(GNode src) const {
    NodeData& src_data = graph->getData(src);

    if (graph->isOwned(graph->getGID(src)) && 
        src_data.dist_current < local_infinity) {
      DGAccumulator_sum += 1;

      if (current_max < src_data.dist_current) {
        current_max = src_data.dist_current;
      }
    }
  }

};
Galois::DGAccumulator<uint64_t> SSSPSanityCheck::DGAccumulator_sum;
Galois::DGAccumulator<uint32_t> SSSPSanityCheck::DGAccumulator_max;
uint32_t SSSPSanityCheck::current_max = 0;

/******************************************************************************/
/* Main */
/******************************************************************************/

int main(int argc, char** argv) {
  try {
    LonestarStart(argc, argv, name, desc, url);
    Galois::StatManager statManager(statOutputFile);
    {
    auto& net = Galois::Runtime::getSystemNetworkInterface();
    if (net.ID == 0) {
      Galois::Runtime::reportStat("(NULL)", "Max Iterations", 
                                  (unsigned long)maxIterations, 0);
      Galois::Runtime::reportStat("(NULL)", "Source Node ID", 
                                  (unsigned long long)src_node, 0);
    }
    Galois::StatTimer StatTimer_init("TIMER_GRAPH_INIT"), 
                      StatTimer_total("TIMER_TOTAL"), 
                      StatTimer_hg_init("TIMER_HG_INIT");

    StatTimer_total.start();

    std::vector<unsigned> scalefactor;
#ifdef __GALOIS_HET_CUDA__
    const unsigned my_host_id = Galois::Runtime::getHostID();
    int gpu_device = gpudevice;
    //Parse arg string when running on multiple hosts and update/override personality
    //with corresponding value.
    if (num_nodes == -1) num_nodes = net.Num;
    assert((net.Num % num_nodes) == 0);
    if (personality_set.length() == (net.Num / num_nodes)) {
      switch (personality_set.c_str()[my_host_id % num_nodes]) {
      case 'g':
        personality = GPU_CUDA;
        break;
      case 'o':
        assert(0);
        personality = GPU_OPENCL;
        break;
      case 'c':
      default:
        personality = CPU;
        break;
      }
      if ((personality == GPU_CUDA) && (gpu_device == -1)) {
        gpu_device = get_gpu_device_id(personality_set, num_nodes);
      }
      if ((scalecpu > 1) || (scalegpu > 1)) {
        for (unsigned i=0; i<net.Num; ++i) {
          if (personality_set.c_str()[i % num_nodes] == 'c') 
            scalefactor.push_back(scalecpu);
          else
            scalefactor.push_back(scalegpu);
        }
      }
    }
#endif

    StatTimer_hg_init.start();
    Graph* hg = nullptr;

    if (enableVCut) {
      if (vertexcut == CART_VCUT)
        hg = new Graph_cartesianCut(inputFile, partFolder, net.ID, net.Num, 
                                    scalefactor, transpose);
      else if (vertexcut == PL_VCUT)
        hg = new Graph_vertexCut(inputFile, partFolder, net.ID, net.Num, 
                                 scalefactor, transpose, VCutThreshold);
    }
    else {
      hg = new Graph_edgeCut(inputFile, partFolder, net.ID, net.Num, 
                             scalefactor, transpose);
    }

#ifdef __GALOIS_HET_CUDA__
    if (personality == GPU_CUDA) {
      cuda_ctx = get_CUDA_context(my_host_id);
      if (!init_CUDA_context(cuda_ctx, gpu_device))
        return -1;
      MarshalGraph m = (*hg).getMarshalGraph(my_host_id);
      load_graph_CUDA(cuda_ctx, m, net.Num);
    } else if (personality == GPU_OPENCL) {
      //Galois::OpenCL::cl_env.init(cldevice.Value);
    }
#endif
    bitset_dist_current.resize(hg->get_local_total_nodes());
    StatTimer_hg_init.stop();

    std::cout << "[" << net.ID << "] InitializeGraph::go called\n";
    StatTimer_init.start();
      InitializeGraph::go((*hg));
    StatTimer_init.stop();


    for(auto run = 0; run < numRuns; ++run){
      std::cout << "[" << net.ID << "] SSSP::go run " << run << " called\n";
      std::string timer_str("TIMER_" + std::to_string(run));
      Galois::StatTimer StatTimer_main(timer_str.c_str());

      StatTimer_main.start();
        SSSP::go((*hg));
      StatTimer_main.stop();

      // sanity check
      SSSPSanityCheck::current_max = 0;
      SSSPSanityCheck::go(*hg);

      if((run + 1) != numRuns){
      #ifdef __GALOIS_HET_CUDA__
        if (personality == GPU_CUDA) { 
          bitset_dist_current_reset_cuda(cuda_ctx);
        } else
      #endif
        bitset_dist_current.reset();

        //Galois::Runtime::getHostBarrier().wait();
        (*hg).reset_num_iter(run+1);
        InitializeGraph::go((*hg));
      }
    }

    StatTimer_total.stop();

    // Verify
    if(verify){
#ifdef __GALOIS_HET_CUDA__
      if (personality == CPU) { 
#endif
        for (auto ii = (*hg).begin(); ii != (*hg).end(); ++ii) {
          if ((*hg).isOwned((*hg).getGID(*ii))) 
            Galois::Runtime::printOutput("% %\n", (*hg).getGID(*ii), 
                                         (*hg).getData(*ii).dist_current);
        }
#ifdef __GALOIS_HET_CUDA__
      } else if(personality == GPU_CUDA)  {
        for (auto ii = (*hg).begin(); ii != (*hg).end(); ++ii) {
          if ((*hg).isOwned((*hg).getGID(*ii))) 
            Galois::Runtime::printOutput("% %\n", (*hg).getGID(*ii), 
                                     get_node_dist_current_cuda(cuda_ctx, *ii));
        }
      }
#endif
    }

    }
    statManager.reportStat();

    return 0;
  } catch(const char* c) {
    std::cerr << "Error: " << c << "\n";
      return 1;
  }
}