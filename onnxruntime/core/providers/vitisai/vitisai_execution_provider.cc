// Copyright (c) Xilinx Inc. All rights reserved.
// Licensed under the MIT License.

#include <fstream>

#include <pyxir/pyxir.hpp>
#include <pyxir/frontend/onnx.hpp>

#include "core/common/common.h"
#include "core/common/logging/logging.h"
#include "core/framework/compute_capability.h"
#include "core/framework/allocatormgr.h"
#include "core/framework/kernel_registry.h"
#include "core/graph/graph_viewer.h"
#include "core/graph/model.h"
#include "vitisai_execution_provider.h"
#include "vitisai_custom_op.h"

#define MEMCPY_S(dest, src, destsz, srcsz) memcpy(dest, src, std::min(destsz, srcsz))

using namespace ONNX_NAMESPACE;
using namespace ::onnxruntime::logging;

namespace onnxruntime {

constexpr const char* PREFIX = "VITISAI";

typedef std::shared_ptr<pyxir::graph::XGraph> XGraphHolder;
typedef std::shared_ptr<pyxir::graph::XLayer> XLayerHolder;

VitisAIExecutionProvider::VitisAIExecutionProvider(const VitisAIExecutionProviderInfo& info)
    : IExecutionProvider{onnxruntime::kVitisAIExecutionProvider}, device_id_(info.device_id) {
  std::cout << "ANDBG backend_type: " << info.backend_type << std::endl;
  //ORT_ENFORCE(info.backend_type == "CPU", "Vitis-AI Execution Provider for onnxruntime currently is only supported for CPU backend.");

  auto default_allocator_factory = [](int) {
    auto memory_info = onnxruntime::make_unique<OrtMemoryInfo>(PREFIX, OrtAllocatorType::OrtDeviceAllocator);
    return onnxruntime::make_unique<CPUAllocator>(std::move(memory_info));
  };

  DeviceAllocatorRegistrationInfo default_memory_info{
    OrtMemTypeDefault,
    std::move(default_allocator_factory),
    std::numeric_limits<size_t>::max()
  };

  InsertAllocator(CreateAllocator(default_memory_info));
}


static std::vector<NodeIndex>
GetUnsupportedNodeIndices(const XGraphHolder &xg, const std::string &target, 
                          const GraphViewer& graph_viewer,
                          /*out*/ std::unordered_set<std::string>& required_initializers) {
  // const auto supported_ops = GetSupportedOps(GetOnnxOpSet(graph_viewer));

  // Retrieve 
  std::set<std::string> supported_tensors;
  for (auto &xl_name : xg->get_layer_names()) {
    XLayerHolder xl = xg->get(xl_name);
    if (xl->target == target)
      supported_tensors.insert(xl->get_attr("onnx_id").get_string());
  }

  std::vector<NodeIndex> unsupported_nodes_idx;

  for (const auto& node_idx : graph_viewer.GetNodesInTopologicalOrder()) {
    ConstPointerContainer<std::vector<NodeArg*>> node_args
      = graph_viewer.GetNode(node_idx)->OutputDefs();
    
    bool is_node_supported = false;
    for (ConstPointerContainer<std::vector<NodeArg*>>::ConstIterator it = 
         node_args.begin(); it != node_args.end(); ++it) {
      // std::cout << "Out node arg name: " << (*it)->Name() << std::endl;
      if (supported_tensors.find((*it)->Name()) != supported_tensors.end()) {
        // std::cout << "Out node arg in supported tensors!" << std::endl;
        is_node_supported = true;
      } else if (is_node_supported) {
        // Some output tensors are supported but not others,
        //  should not happen
        LOGS_DEFAULT(FATAL) << "VITIS-AI EP: Found node output tensor "
          << (*it)->Name() << " which is partially supported by "
          << " DPU accelerator. This is an invalid case";
      }
    }

    if (is_node_supported) {
      // Collect inputs that are initializers
      graph_viewer.GetNode(node_idx)->ForEachDef([&required_initializers, &graph_viewer](const onnxruntime::NodeArg& node_arg, bool is_input) {
        if(is_input && graph_viewer.GetAllInitializedTensors().count(node_arg.Name())) {
          required_initializers.insert(node_arg.Name());
        } }, true);
    } else {
      unsupported_nodes_idx.push_back(node_idx);
    }
  }

  return unsupported_nodes_idx;
}

/**
 * Returns a vector clusters(or node_idx). For each unsupported node, the graph is split into 3 parts.
 * supported_cluster + (UNsupported_node + rest_of_the_graph). This functions returns vector of all supported_clusters by DPU
 */
static std::vector<std::vector<NodeIndex>>
GetPartitionedClusters(const std::vector<NodeIndex>& topological_order, const std::vector<NodeIndex>& unsupported_nodes) {
  std::vector<std::vector<NodeIndex>> clusters;

  auto prev = topological_order.begin();

  for (const auto& unsup_node : unsupported_nodes) {
    auto it = std::find(prev, topological_order.end(), unsup_node);
    // Create a cluster vector[supported_node_idx, unsupported_node_idx) and append it to return list.
    std::vector<NodeIndex> this_cluster{prev, it};
    if (!this_cluster.empty()) {
      clusters.push_back(std::move(this_cluster));
    }
    // Point prev to node idx past this unsuported node.
    prev = ++it;
  }

  //Tail
  std::vector<NodeIndex> this_cluster{prev, topological_order.end()};
  if (!this_cluster.empty()) {
    clusters.push_back(std::move(this_cluster));
  }

  return clusters;
}

static void GetInputsOutputsOfCluster(const GraphViewer& graph_viewer,
                                      const std::vector<NodeIndex>& cluster,
                                      const std::unordered_set<std::string>& ng_required_initializers,
                                      /*out*/ std::vector<std::string>& cluster_inputs,
                                      /*out*/ std::vector<std::string>& cluster_outputs) {
  std::unordered_set<std::string> input_args;
  std::vector<std::string> ordered_input_args;
  std::unordered_set<std::string> output_args;
  std::unordered_set<std::string> external_output_args;

  for (const auto& node_idx : cluster) {
    const auto& node = graph_viewer.GetNode(node_idx);

    // Collect all inputs and outputs
    node->ForEachDef(
        [&input_args, &ordered_input_args, &output_args](const NodeArg& node_arg, bool is_input) {
          if (is_input) {
            if (!input_args.count(node_arg.Name())) {
              ordered_input_args.push_back(node_arg.Name());
            }
            input_args.insert(node_arg.Name());
          } else {
            output_args.insert(node_arg.Name());
          }
        },
        true);

    // Check if output of this node is used by nodes outside this_cluster. If yes add this to cluster outputs
    for (auto it = node->OutputNodesBegin(); it != node->OutputNodesEnd(); ++it) {
      const auto& ext_node = graph_viewer.GetNode((*it).Index());

      if (std::find(cluster.begin(), cluster.end(), ext_node->Index()) == cluster.end()) {
        // Node is external to this_cluster. Search through its inputs to find the output that is generated by this_cluster.
        std::set<std::string> ext_node_inputs;
        ext_node->ForEachDef(
            [&ext_node_inputs](const onnxruntime::NodeArg& arg, bool is_input) {
              if (is_input) {
                ext_node_inputs.insert(arg.Name());
              }
            },
            true);

        for (const auto& out_def : node->OutputDefs()) {
          if (ext_node_inputs.find(out_def->Name()) != ext_node_inputs.end()) {
            external_output_args.insert(out_def->Name());
          }
        }
      }
    }
  }

  //Extract initializers used by this_cluster.
  std::unordered_set<std::string> original_graph_inputs;
  for (const auto& node_arg : graph_viewer.GetInputsIncludingInitializers()) {
    original_graph_inputs.insert(node_arg->Name());
  }

  const auto& initializers = graph_viewer.GetAllInitializedTensors();
  std::vector<std::string> const_inputs;
  for (const auto& in_arg : ordered_input_args) {
    if ((initializers.count(in_arg) && !original_graph_inputs.count(in_arg)) ||
        ng_required_initializers.count(in_arg)) {
      const_inputs.push_back(in_arg);
    }
  }

  for (const auto& in_arg : ordered_input_args) {
    if (!output_args.count(in_arg) &&
        !((initializers.count(in_arg) && !original_graph_inputs.count(in_arg)) ||
        ng_required_initializers.count(in_arg))) {
      cluster_inputs.push_back(in_arg);
    }
  }

  for (const auto& in_arg : const_inputs) {
    cluster_inputs.push_back(in_arg);
  }

  std::copy(external_output_args.begin(), external_output_args.end(), std::back_inserter(cluster_outputs));
  for (const auto& node_arg : graph_viewer.GetOutputs()) {
    const auto& name = node_arg->Name();
    if (output_args.count(name) && !external_output_args.count(name)) {
      cluster_outputs.push_back(name);
    }
  }
}

static void AppendClusterToSubGraph(const std::vector<NodeIndex>& nodes,
                                    const std::vector<std::string>& inputs,
                                    const std::vector<std::string>& outputs,
                                    std::vector<std::unique_ptr<ComputeCapability>>& result) {
  static size_t op_counter = 0;

  auto meta_def = onnxruntime::make_unique<IndexedSubGraph::MetaDef>();
  meta_def->name = "VitisAICustomOp_" + std::to_string(++op_counter);
  meta_def->domain = kVitisAIDomain;
  meta_def->since_version = 1;
  meta_def->status = ONNX_NAMESPACE::EXPERIMENTAL;
  meta_def->inputs = inputs;
  meta_def->outputs = outputs;

  std::unique_ptr<IndexedSubGraph> sub_graph = onnxruntime::make_unique<IndexedSubGraph>();
  sub_graph->nodes = nodes;
  sub_graph->SetMetaDef(meta_def);
  result.push_back(onnxruntime::make_unique<ComputeCapability>(std::move(sub_graph)));
}


std::vector<std::unique_ptr<ComputeCapability>>
VitisAIExecutionProvider::GetCapability(const onnxruntime::GraphViewer& graph,
                                         const std::vector<const KernelRegistry*>& kernel_registries) const {                                  
  ORT_UNUSED_PARAMETER(kernel_registries);

  std::vector<std::unique_ptr<ComputeCapability>> result;

  // Dump model Proto to file to pass it to pyxir
  auto logger = *GetLogger();

  const Graph& node_graph = graph.GetGraph();
  const std::string& name_ = node_graph.Name();
  onnxruntime::Model model{name_, true, ModelMetaData{},
                           IOnnxRuntimeOpSchemaRegistryList{},
                           node_graph.DomainToVersionMap(),
                           std::vector<ONNX_NAMESPACE::FunctionProto>(),
                           logger};

  ONNX_NAMESPACE::ModelProto model_proto = model.ToProto();
  model_proto.set_ir_version(ONNX_NAMESPACE::Version::IR_VERSION);

  *(model_proto.mutable_graph()) = node_graph.ToGraphProto();

  std::string file_path = name_ + ".onnx";
  std::fstream dump(file_path,
                    std::ios::out | std::ios::trunc | std::ios::binary);
  model_proto.SerializeToOstream(&dump);
  dump.flush();

  // Transform ONNX into Pyxir XGraph data structure 
  XGraphHolder xg = pyxir::onnx::import_onnx_model(file_path);

  // Annotate the subgraph layers in the XGraph that can be executed on the
  //  `dpuv1` target
  std::string target = "dpuv1";
  pyxir::partition(xg, std::vector<std::string>{target}, "");

  // Next stuff
  if (graph.IsSubgraph()) {
    return result;
  }

  // Need access to model_path_
  for (const auto& tensor : graph.GetAllInitializedTensors()) {
    if (tensor.second->has_data_location() && tensor.second->data_location() == ONNX_NAMESPACE::TensorProto_DataLocation_EXTERNAL) {
      LOGS_DEFAULT(WARNING) << "VITIS-AI EP: Initializers with external data location are not currently supported";
      return result;
    }
  }

  std::unordered_set<std::string> required_initializers;
  const auto unsupported_nodes = GetUnsupportedNodeIndices(xg, target, graph, required_initializers);

  const auto clusters = GetPartitionedClusters(graph.GetNodesInTopologicalOrder(), unsupported_nodes);

  for (const auto& this_cluster : clusters) {
    std::vector<std::string> cluster_inputs, cluster_outputs;
    GetInputsOutputsOfCluster(graph, this_cluster, required_initializers, cluster_inputs, cluster_outputs);

    if (!cluster_inputs.empty()) {
      AppendClusterToSubGraph(this_cluster, cluster_inputs, cluster_outputs, result);
    }
  }

  return result;
}

common::Status VitisAIExecutionProvider::Compile(const std::vector<onnxruntime::Node*>& fused_nodes,
                                                  std::vector<NodeComputeInfo>& node_compute_funcs) {
  for (const auto& fused_node : fused_nodes) 
  {
    NodeComputeInfo compute_info;
    // model_proto = GetModelProtoFromFusedNode(fused_node, *GetLogger())
    compute_info.create_state_func = [fused_node, logger=GetLogger()](ComputeContext* context, FunctionState* state) {
      auto* p = new vitisai_ep::VitisAICustomOp(context, fused_node, logger); // model_proto
      *state = p;
      return 0;
    };

    compute_info.release_state_func = [](FunctionState state) {
      if (state)
        delete reinterpret_cast<onnxruntime::vitisai_ep::VitisAICustomOp*>(state);
    };

    compute_info.compute_func = [](FunctionState state, const OrtApi* api, OrtKernelContext* context) {
      onnxruntime::vitisai_ep::VitisAICustomOp* custom_op = reinterpret_cast<onnxruntime::vitisai_ep::VitisAICustomOp*>(state);
      return custom_op->Compute(api, context);
    };

    node_compute_funcs.push_back(compute_info);
  }

  return Status::OK();
}
}  // namespace onnxruntime
