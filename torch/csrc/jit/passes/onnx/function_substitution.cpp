#include <torch/csrc/jit/passes/onnx/function_substitution.h>

#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/passes/onnx/helper.h>
#include <torch/csrc/jit/passes/onnx/naming.h>

namespace torch {
namespace jit {

namespace {

const std::string top_module_variable_name = "";

std::string TidyClassNameFromTorchScript(
    const c10::optional<c10::QualifiedName>& class_name) {
  if (!class_name) {
    return "UNKNOWN_CLASS";
  }
  std::string out = "";
  for (const auto& atom : class_name->atoms()) {
    bool isInternalTorchAtom = (atom == "__torch__");
    bool isMangleAtom = (atom.find("__torch_mangle") != std::string::npos);
    if (!isInternalTorchAtom && !isMangleAtom) {
      if (!out.empty()) {
        out += ".";
      }
      out += atom;
    }
  }
  return out;
}

std::string GetCallNodeVariableName(const Node* call_node) {
  TORCH_INTERNAL_ASSERT(
      call_node->kind() == prim::CallFunction ||
      call_node->kind() == prim::CallMethod);
  auto module_node = call_node->input(0)->node();

  if (!module_node->hasAttribute(attr::name)) {
    return "";
  }
  std::string module_name = module_node->s(attr::name);
  if (module_node->inputs().size() == 0) {
    return module_name;
  }
  // If module is from container, attr::name in module node only carries
  // index info. Need to check parent node (container) for variable name.
  auto parent_module_value = module_node->input(0);
  while (parent_module_value) {
    auto parent_module_type = parent_module_value->type()->cast<ClassType>();
    if (parent_module_type &&
        parent_module_type->name() ==
            "__torch__.torch.nn.modules.container.ModuleList") {
      auto parent_module_node = parent_module_value->node();
      module_name = parent_module_node->s(attr::name) + "." + module_name;
      parent_module_value = parent_module_node->inputs().size() > 0
          ? parent_module_node->input(0)
          : nullptr;
    } else {
      break;
    }
  }

  return module_name;
}

WithCurrentScope SetScopeGuardForCall(Graph& graph, Node* call_node) {
  TORCH_INTERNAL_ASSERT(
      call_node->kind() == prim::CallFunction ||
      call_node->kind() == prim::CallMethod);
  const auto type = call_node->input(0)->type()->expect<c10::NamedType>();
  const auto class_name = TidyClassNameFromTorchScript(type->name());
  const auto variable_name = GetCallNodeVariableName(call_node);
  const auto scope_name =
      onnx::ONNXScopeName::createFullScopeName(class_name, variable_name);
  auto call_scope = graph.current_scope()->push(Symbol::scope(scope_name));
  return WithCurrentScope(graph, call_scope);
}

void functionCallSubstitution(Block* block) {
  auto graph = block->owningGraph();
  for (auto it = block->nodes().begin(), end = block->nodes().end();
       it != end;) {
    Node* cur = *it++;
    switch (cur->kind()) {
      case prim::CallFunction: {
        AT_ASSERT(cur->input(0)->node()->kind() == prim::Constant);
        auto function_constant = cur->input(0)->node();
        auto fun_type =
            function_constant->output()->type()->expect<FunctionType>();

        if ((fun_type->function()->qualname().qualifiedName().find(
                 "torch.nn.functional") != std::string::npos) &&
            (fun_type->function()->qualname().qualifiedName().find(
                 "interpolate") != std::string::npos)) {
          // Remove input[0] and the node that feeds into it
          auto input_node_0 = cur->input(0)->node();
          cur->removeInput(0);
          if (!input_node_0->hasUses()) {
            input_node_0->destroy();
          }
          Node* interpolate_node = block->owningGraph()->create(
              Symbol::fromQualString("aten::__interpolate"),
              {cur->inputs()},
              cur->outputs().size());
          interpolate_node->output()->copyMetadata(cur->output());
          interpolate_node->insertAfter(cur);
          interpolate_node->copyMetadata(cur);
          cur->replaceAllUsesWith(interpolate_node);
          cur->removeAllInputs();
          cur->destroy();
          GRAPH_UPDATE(
              "ONNX function call substitution function: '",
              fun_type->function()->name(),
              "' to aten::__interpolate");
          GRAPH_UPDATE(
              "Function in ONNX function call substitution body: ",
              toGraphFunction(*fun_type->function()).optimized_graph());
        } else {
          // Remove input[0] and the node that feeds into it
          auto input_node_0 = cur->input(0)->node();
          cur->removeInput(0);
          if (!input_node_0->hasUses()) {
            input_node_0->destroy();
          }
          auto& graphFunction = toGraphFunction(*fun_type->function());
          functionCallSubstitution(graphFunction.graph()->block());
          inlineCallTo(cur, &graphFunction, false);
        }
      } break;
      case prim::CallMethod: {
        const std::string& name = cur->s(attr::name);
        if (auto class_type = cur->input(0)->type()->cast<ClassType>()) {
          Function& function = class_type->getMethod(name);
          WithCurrentScope scope_guard = SetScopeGuardForCall(*graph, cur);
          if (auto graphFunction = tryToGraphFunction(function)) {
            WithCurrentScope inner_graph_scope_guard = WithCurrentScope(
                *graphFunction->graph(), graph->current_scope());
            functionCallSubstitution(graphFunction->graph()->block());
            inlineCallTo(cur, graphFunction, false);
          }
        }
      } break;
      default: {
        if (!graph->current_scope()->isBlank()) {
          cur->setScope(graph->current_scope());
        }
        for (auto b : cur->blocks()) {
          functionCallSubstitution(b);
        }
      } break;
    }
  }
}

WithCurrentScope ONNXGraphTopLevelScopeGuard(Graph& graph) {
  if (graph.inputs().size() == 0) {
    return WithCurrentScope(graph, graph.current_scope());
  }
  if (auto top_module_type = graph.inputs().at(0)->type()->cast<ClassType>()) {
    auto scope_name = ::torch::jit::onnx::ONNXScopeName::createFullScopeName(
        TidyClassNameFromTorchScript(top_module_type->name()),
        top_module_variable_name);
    auto scope = graph.current_scope()->push(Symbol::scope(scope_name));
    return WithCurrentScope(graph, scope);
  }
  return WithCurrentScope(graph, graph.current_scope());
}

} // namespace

// This pass is to be used for ONNX conversion only. The ONNX converter depends
// on a number of deprecated aten operators. These operators are removed from IR
// and replaced by the compiled python function code. However, in-order to
// maintain the behavior for ONNX conversion, we replace these function calls
// with the aten symbolic which can still be used by the ONNX converter.
void ONNXFunctionCallSubstitution(Graph& graph) {
  GRAPH_DUMP("Before function call substitution calls: ", &graph);
  WithCurrentScope top_level_scope_guard = ONNXGraphTopLevelScopeGuard(graph);
  functionCallSubstitution(graph.block());
  GRAPH_DUMP("After function call substitution calls: ", &graph);
}

} // namespace jit
} // namespace torch
