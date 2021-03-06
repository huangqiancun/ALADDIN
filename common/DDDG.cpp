#include <cstring>
#include <stdio.h>
#include <sys/stat.h>

#include <boost/tokenizer.hpp>

#include "BaseDatapath.h"
#include "DDDG.h"
#include "DynamicEntity.h"
#include "ExecNode.h"
#include "ProgressTracker.h"
#include "SourceManager.h"

using namespace SrcTypes;

static const char kHexTable[16] = {
  '0', '1', '2', '3', '4', '5', '6', '7',
  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
};

static uint8_t nibbleVal(char nib) {
  for (uint8_t i = 0; i < 16; i++) {
    if (nib == kHexTable[i])
      return i;
  }
  assert("Invalid character!\n");
  return 0;
}

uint8_t* hexStrToBytes(const char* str) {
  unsigned len = strlen(str);
  assert(len % 2 == 0 && "String must be an even length!");
  unsigned start = 0;
  unsigned byte_buf_len = len/2;
  // Ignore the starting 0x.
  if (strncmp(str, "0x", 2) == 0) {
    start += 2;
    byte_buf_len--;
  }
  uint8_t* byte_buf = new uint8_t[byte_buf_len];
  for (unsigned i = start; i < len; i += 2) {
    uint8_t val = (nibbleVal(str[i]) << 4) + nibbleVal(str[i + 1]);
    byte_buf[(i - start) / 2] = val;
  }
  return byte_buf;
}

char* bytesToHexStr(uint8_t* data, int size, bool separate32) {
  unsigned str_len = size * 2 + 3;
  if (separate32)
    str_len += size / 4;

  unsigned sep_count = 0;
  const unsigned sep_threshold = separate32 ? 4 : size + 1;

  char* str_buf = new char[str_len];
  char* buf_ptr = str_buf;
  sprintf(buf_ptr, "0x");
  buf_ptr += 2;

  for (int i = 0; i < size; i++) {
    buf_ptr += sprintf(buf_ptr, "%02x", data[i]);
    sep_count++;
    if (sep_count == sep_threshold && i != size - 1) {
      buf_ptr += sprintf(buf_ptr, "_");
      sep_count = 0;
    }
  }
  *buf_ptr = 0;

  return str_buf;
}

// TODO: Eventual goal is to remove datapath as an argument entirely and rely
// only on Program.
DDDG::DDDG(BaseDatapath* _datapath, Program* _program, gzFile& _trace_file)
    : datapath(_datapath), program(_program), trace_file(_trace_file),
      srcManager(_datapath->get_source_manager()) {
  num_of_reg_dep = 0;
  num_of_mem_dep = 0;
  num_of_ctrl_dep = 0;
  num_of_instructions = -1;
  current_node_id = -1;
  last_parameter = 0;
  last_dma_fence = -1;
  prev_bblock = "-1";
  curr_bblock = "-1";
  current_loop_depth = 0;
  callee_function = nullptr;
  last_ret = nullptr;
}

int DDDG::num_edges() {
  return num_of_reg_dep + num_of_mem_dep + num_of_ctrl_dep;
}

int DDDG::num_nodes() { return num_of_instructions + 1; }
int DDDG::num_of_register_dependency() { return num_of_reg_dep; }
int DDDG::num_of_memory_dependency() { return num_of_mem_dep; }
int DDDG::num_of_control_dependency() { return num_of_ctrl_dep; }

void DDDG::output_dddg() {
  for (auto it = register_edge_table.begin(); it != register_edge_table.end();
       ++it) {
    program->addEdge(it->first, it->second.sink_node, it->second.par_id);
  }

  for (auto source_it = memory_edge_table.begin();
       source_it != memory_edge_table.end();
       ++source_it) {
    unsigned source = source_it->first;
    std::set<unsigned>& sink_list = source_it->second;
    for (unsigned sink_node : sink_list)
      program->addEdge(source, sink_node, MEMORY_EDGE);
  }

  for (auto source_it = control_edge_table.begin();
       source_it != control_edge_table.end();
       ++source_it) {
    unsigned source = source_it->first;
    std::set<unsigned>& sink_list = source_it->second;
    for (unsigned sink_node : sink_list)
      program->addEdge(source, sink_node, CONTROL_EDGE);
  }
}

void DDDG::handle_post_write_dependency(Addr start_addr,
                                        size_t size,
                                        unsigned sink_node) {
  Addr addr = start_addr;
  while (addr < start_addr + size) {
    // Get the last node to write to this address.
    auto addr_it = address_last_written.find(addr);
    if (addr_it != address_last_written.end()) {
      unsigned source_inst = addr_it->second;

      if (memory_edge_table.find(source_inst) == memory_edge_table.end())
        memory_edge_table[source_inst] = std::set<unsigned>();
      std::set<unsigned>& sink_list = memory_edge_table[source_inst];

      // No need to check if the node already exists - if it does, an insertion
      // will not happen, and insertion would require searching for the place
      // to place the new entry anyways.
      auto result = sink_list.insert(sink_node);
      if (result.second)
        num_of_mem_dep++;
    }
    addr++;
  }
}

void DDDG::insert_control_dependence(unsigned source_node, unsigned dest_node) {
  if (control_edge_table.find(source_node) == control_edge_table.end())
    control_edge_table[source_node] = std::set<unsigned>();
  std::set<unsigned>& dest_nodes = control_edge_table[source_node];
  auto result = dest_nodes.insert(dest_node);
  if (result.second)
    num_of_ctrl_dep++;
}

// Find the original array corresponding to this array in the current function.
//
// The array_name argument may not actually be the real name of the array as it
// was originally declared, so we have to backtrace dynamic variable references
// until we find the original one.
//
// Return a pointer to that Variable.
Variable* DDDG::get_array_real_var(const std::string& array_name) {
  Variable* var = srcManager.get<Variable>(array_name);
  DynamicVariable dyn_var(curr_dynamic_function, var);
  DynamicVariable real_dyn_var = program->call_arg_map.lookup(dyn_var);
  Variable* real_var = real_dyn_var.get_variable();
  return real_var;
}

MemAccess* DDDG::create_mem_access(char* value_str,
                                   double value_dp,
                                   unsigned mem_size_bytes,
                                   ValueType value_type) {
  if (value_type == Vector) {
    uint8_t* bytes = hexStrToBytes(value_str);
    VectorMemAccess* mem_access = new VectorMemAccess();
    mem_access->set_value(bytes);
    mem_access->size = mem_size_bytes;
    return mem_access;
  } else {
    bool is_float = value_type == Float;
    uint64_t bits =
        FP2BitsConverter::Convert(value_dp, mem_size_bytes, is_float);
    ScalarMemAccess* mem_access = new ScalarMemAccess();
    mem_access->set_value(bits);
    mem_access->is_float = is_float;
    mem_access->size = mem_size_bytes;
    return mem_access;
  }
}

// Parse line from the labelmap section.
void DDDG::parse_labelmap_line(std::string line) {
  char label_name[256], function_name[256], callers[256];
  int line_number;
  int num_matches = sscanf(line.c_str(),
                           "%[^/]/%s %d inline %[^\n]",
                           function_name,
                           label_name,
                           &line_number,
                           callers);
  label_name[255] = '\0';  // Just in case...
  function_name[255] = '\0';
  Function* function = srcManager.insert<Function>(function_name);
  Label* label = srcManager.insert<Label>(label_name);
  UniqueLabel unique_label(function, label, line_number);
  program->labelmap.insert(std::make_pair(line_number, unique_label));
  if (num_matches == 4) {
    boost::char_separator<char> sep(" ");
    std::string temp(callers);
    boost::tokenizer<boost::char_separator<char>> tok(temp, sep);
    for (auto it = tok.begin(); it != tok.end(); ++it) {
      std::string caller_name = *it;
      Function* caller_func = srcManager.insert<Function>(caller_name);
      UniqueLabel inlined_label(caller_func, label, line_number);
      program->labelmap.insert(std::make_pair(line_number, inlined_label));
      // Add the inlined labels to another map so that we can associate any
      // unrolling/pipelining directives declared on the original labels with
      // them.
      inline_labelmap[inlined_label] = unique_label;
    }
  }
}

void DDDG::parse_instruction_line(std::string line) {
  char curr_static_function[256];
  char instid[256], bblockid[256], bblockname[256];
  int line_num;
  int microop;
  sscanf(line.c_str(),
         "%d,%[^,],%[^,],%[^,],%d,%lu\n",
         &line_num,
         curr_static_function,
         bblockid,
         instid,
         &microop,
         &current_node_id);

  num_of_instructions++;
  prev_microop = curr_microop;
  curr_microop = (uint8_t)microop;
  curr_instid = instid;

  // Update the current loop depth.
  sscanf(bblockid, "%[^:]:%u", bblockname, &current_loop_depth);
  // If the loop depth is greater than 1000 within this function, we've
  // probably done something wrong.
  assert(current_loop_depth < 1000 &&
         "Loop depth is much higher than expected!");

  Function* curr_function =
      srcManager.insert<Function>(curr_static_function);
  Instruction* curr_inst = srcManager.insert<Instruction>(curr_instid);
  BasicBlock* basicblock = srcManager.insert<BasicBlock>(bblockname);
  curr_node = program->insertNode(current_node_id, microop);
  curr_node->set_line_num(line_num);
  curr_node->set_static_inst(curr_inst);
  curr_node->set_static_function(curr_function);
  curr_node->set_basic_block(basicblock);
  curr_node->set_loop_depth(current_loop_depth);
  datapath->addFunctionName(curr_static_function);

  int func_invocation_count = 0;
  bool curr_func_found = false;

  // Enforce dependences on function call boundaries.
  // Another function cannot be called until all previous nodes in the current
  // function have finished, and a function must execute all nodes before nodes
  // in the parent function can execute. The only exceptions are DMA nodes.
  if (curr_node->is_ret_op() || curr_node->is_call_op()) {
    for (auto node_id : nodes_since_last_ret)
      insert_control_dependence(node_id, current_node_id);
    nodes_since_last_ret.clear();
    if (last_ret && last_ret != curr_node)
      insert_control_dependence(last_ret->get_node_id(), current_node_id);
    last_ret = curr_node;
  } else if (!curr_node->is_dma_op()) {
    nodes_since_last_ret.push_back(current_node_id);
  }

  if (!active_method.empty()) {
    Function* prev_function = active_method.top().get_function();
    unsigned prev_counts = prev_function->get_invocations();
    if (curr_function == prev_function) {
      // calling itself
      if (prev_microop == LLVM_IR_Call && callee_function == curr_function) {
        curr_function->increment_invocations();
        func_invocation_count = curr_function->get_invocations();
        active_method.push(DynamicFunction(curr_function));
        curr_dynamic_function = active_method.top();
      } else {
        func_invocation_count = prev_counts;
        curr_dynamic_function = active_method.top();
      }
      curr_func_found = true;
    }
    if (microop == LLVM_IR_Ret)
      active_method.pop();
  }
  if (!curr_func_found) {
    // This would only be true on a call.
    curr_function->increment_invocations();
    func_invocation_count = curr_function->get_invocations();
    active_method.push(DynamicFunction(curr_function));
    curr_dynamic_function = active_method.top();
  }
  if (microop == LLVM_IR_PHI && prev_microop != LLVM_IR_PHI)
    prev_bblock = curr_bblock;
  if (microop == LLVM_IR_DMAFence) {
    last_dma_fence = current_node_id;
    for (unsigned node_id : last_dma_nodes) {
      insert_control_dependence(node_id, last_dma_fence);
    }
    last_dma_nodes.clear();
  } else if (microop == LLVM_IR_DMALoad || microop == LLVM_IR_DMAStore) {
    if (last_dma_fence != -1)
      insert_control_dependence(last_dma_fence, current_node_id);
    last_dma_nodes.push_back(current_node_id);
  }
  curr_bblock = bblockid;
  curr_node->set_dynamic_invocation(func_invocation_count);
  last_parameter = 0;
  parameter_value_per_inst.clear();
  parameter_size_per_inst.clear();
  parameter_label_per_inst.clear();
}

void DDDG::parse_parameter(std::string line, int param_tag) {
  int size, is_reg;
  char char_value[256];
  char label[256], prev_bbid[256];
  if (curr_microop == LLVM_IR_PHI) {
    sscanf(line.c_str(),
           "%d,%[^,],%d,%[^,],%[^,],\n",
           &size,
           char_value,
           &is_reg,
           label,
           prev_bbid);
    if (prev_bblock.compare(prev_bbid) != 0) {
      return;
    }
  } else {
    sscanf(line.c_str(),
           "%d,%[^,],%d,%[^,],\n",
           &size,
           char_value,
           &is_reg,
           label);
  }
  std::string tmp_value(char_value);
  ValueType value_type = size > 64 ? Vector : 
      tmp_value.find('.') != std::string::npos ? Float : Integer;
  // If the value is a vector type, we need to process it differently.
  double value = value_type == Vector ? 0 : strtod(char_value, NULL);
  if (!last_parameter) {
    num_of_parameters = param_tag;
    if (curr_microop == LLVM_IR_Call)
      callee_function = srcManager.insert<Function>(label);
    if (callee_function) {
      callee_dynamic_function = DynamicFunction(
          callee_function, callee_function->get_invocations() + 1);
    }
  }
  last_parameter = 1;
  last_call_source = -1;
  if (is_reg) {
    Variable* variable = srcManager.insert<Variable>(label);
    DynamicVariable unique_reg_ref(curr_dynamic_function, variable);
    if (curr_microop == LLVM_IR_Call) {
      unique_reg_in_caller_func = unique_reg_ref;
    }
    // Find the instruction that writes the register
    auto reg_it = register_last_written.find(unique_reg_ref);
    if (reg_it != register_last_written.end()) {
      /*Find the last instruction that writes to the register*/
      reg_edge_t tmp_edge = { (unsigned)current_node_id, param_tag };
      register_edge_table.insert(std::make_pair(reg_it->second, tmp_edge));
      num_of_reg_dep++;
      if (curr_microop == LLVM_IR_Call) {
        last_call_source = reg_it->second;
      }
    } else if ((curr_microop == LLVM_IR_Store && param_tag == 2) ||
               (curr_microop == LLVM_IR_Load && param_tag == 1)) {
      /*For the load/store op without a gep instruction before, assuming the
       *load/store op performs a gep which writes to the label register*/
      register_last_written[unique_reg_ref] = current_node_id;
    }
  }
  if (curr_microop == LLVM_IR_Load || curr_microop == LLVM_IR_Store ||
      curr_microop == LLVM_IR_GetElementPtr || curr_node->is_dma_op()) {
    parameter_value_per_inst.push_back(((Addr)value) & ADDR_MASK);
    parameter_size_per_inst.push_back(size);
    parameter_label_per_inst.push_back(label);
    // last parameter
    if (param_tag == 1 && curr_microop == LLVM_IR_Load) {
      // The label is the name of the register that holds the address.
      const std::string& reg_name = parameter_label_per_inst.back();
      Variable* var = srcManager.get<Variable>(reg_name);
      curr_node->set_variable(var);
      curr_node->set_array_label(reg_name);
    } else if (param_tag == 1 && curr_microop == LLVM_IR_Store) {
      // 1st arg of store is the address, and the 2nd arg is the value, but the
      // 2nd arg is parsed first.
      Addr mem_address = parameter_value_per_inst[0];
      size_t mem_size = size / BYTE;
      MemAccess* mem_access =
          create_mem_access(char_value, value, mem_size, value_type);
      mem_access->vaddr = mem_address;
      curr_node->set_mem_access(mem_access);
    } else if (param_tag == 2 && curr_microop == LLVM_IR_Store) {
      Addr mem_address = parameter_value_per_inst[0];
      unsigned mem_size = parameter_size_per_inst.back() / BYTE;

      auto addr_it = address_last_written.find(mem_address);
      if (addr_it != address_last_written.end()) {
        // Check if the last node to write was a DMA load. If so, we must obey
        // this memory ordering, because DMA loads are variable-latency
        // operations.
        int last_node_to_write = addr_it->second;
        if (program->nodes.at(last_node_to_write)->is_dma_load())
          handle_post_write_dependency(
              mem_address, mem_size, current_node_id);
        // Now we can overwrite the last written node id.
        addr_it->second = current_node_id;
      } else {
        address_last_written.insert(
            std::make_pair(mem_address, current_node_id));
      }

      // The label is the name of the register that holds the address.
      const std::string& reg_name = parameter_label_per_inst[0];
      Variable* var = srcManager.get<Variable>(reg_name);
      curr_node->set_variable(var);
      curr_node->set_array_label(reg_name);
    } else if (param_tag == 1 && curr_microop == LLVM_IR_GetElementPtr) {
      Addr base_address = parameter_value_per_inst.back();
      const std::string& base_label = parameter_label_per_inst.back();
      // The variable id should be set to the current perceived array name,
      // since that's how dependencies are locally enforced.
      Variable* var = srcManager.get<Variable>(base_label);
      curr_node->set_variable(var);
      // Only GEPs have an array label we can use to update the base address.
      Variable* real_array = get_array_real_var(base_label);
      const std::string& real_name = real_array->get_name();
      curr_node->set_array_label(real_name);
      datapath->addArrayBaseAddress(real_name, base_address);
    } else if (param_tag == 1 && curr_node->is_dma_op()) {
      // Data dependencies are handled in parse_result(), because we need all
      // the arguments to dmaLoad in order to do this.
    }
  }
}

void DDDG::parse_result(std::string line) {
  int size, is_reg;
  char char_value[256];
  char label[256];

  sscanf(line.c_str(), "%d,%[^,],%d,%[^,],\n", &size, char_value, &is_reg, label);
  std::string tmp_value(char_value);
  ValueType value_type = size > 64 ? Vector :
      tmp_value.find('.') != std::string::npos ? Float : Integer;
  double value = value_type == Vector ? 0 : strtod(char_value, NULL);
  std::string label_str(label);

  if (curr_node->is_fp_op() && (size == 64))
    curr_node->set_double_precision(true);
  assert(is_reg);
  Variable* var = srcManager.insert<Variable>(label_str);
  DynamicVariable unique_reg_ref(curr_dynamic_function, var);
  auto reg_it = register_last_written.find(unique_reg_ref);
  if (reg_it != register_last_written.end())
    reg_it->second = current_node_id;
  else
    register_last_written[unique_reg_ref] = current_node_id;

  if (curr_microop == LLVM_IR_Alloca) {
    curr_node->set_variable(srcManager.get<Variable>(label_str));
    curr_node->set_array_label(label_str);
    datapath->addArrayBaseAddress(label_str, ((Addr)value) & ADDR_MASK);
  } else if (curr_microop == LLVM_IR_Load) {
    Addr mem_address = parameter_value_per_inst.back();
    size_t mem_size = size / BYTE;
    MemAccess* mem_access =
        create_mem_access(char_value, value, mem_size, value_type);
    mem_access->vaddr = mem_address;
    handle_post_write_dependency(mem_address, mem_size, current_node_id);
    curr_node->set_mem_access(mem_access);
  } else if (curr_node->is_dma_op()) {
    Addr base_addr = 0;
    size_t src_off = 0, dst_off = 0, size = 0;
    // Determine DMA interface version.
    if (parameter_value_per_inst.size() == 4) {
      // v1 (src offset = dst offset).
      base_addr = parameter_value_per_inst[1];
      src_off = (size_t) parameter_value_per_inst[2];
      dst_off = src_off;
      size = (size_t) parameter_value_per_inst[3];
    } else if (parameter_value_per_inst.size() == 5) {
      // v2 (src offset is separate from dst offset).
      base_addr = parameter_value_per_inst[1];
      src_off = (size_t) parameter_value_per_inst[2];
      dst_off = (size_t) parameter_value_per_inst[3];
      size = (size_t) parameter_value_per_inst[4];
    } else {
      assert("Unknown DMA interface version!");
    }
    curr_node->set_dma_mem_access(base_addr, src_off, dst_off, size);
    if (curr_microop == LLVM_IR_DMALoad) {
      /* If we're using full/empty bits, then we want loads and stores to
       * issue as soon as their data is available. This means that for nearly
       * all of the loads, the DMA load node would not have completed, so we
       * can't add these memory dependencies.
       */
      if (!datapath->isReadyMode()) {
        // For dmaLoad (which is a STORE from the accelerator's perspective),
        // enforce RAW and WAW dependencies on subsequent nodes.
        Addr start_addr = base_addr + dst_off;
        for (Addr addr = start_addr; addr < start_addr + size; addr += 1) {
          // TODO: Storing an entry for every byte in this range is very inefficient...
          auto addr_it = address_last_written.find(addr);
          if (addr_it != address_last_written.end())
            addr_it->second = current_node_id;
          else
            address_last_written.insert(
                std::make_pair(addr, current_node_id));
        }
      }
    } else {
      // For dmaStore (which is actually a LOAD from the accelerator's
      // perspective), enforce RAW dependencies on this node.
      Addr start_addr = base_addr + src_off;
      handle_post_write_dependency(start_addr, size, current_node_id);
    }
  }
}

void DDDG::parse_forward(std::string line) {
  int size, is_reg;
  double value;
  char label[256];

  // DMA and trig operations are not actually treated as called functions by
  // Aladdin, so there is no need to add any register name mappings.
  if (curr_node->is_dma_op() || curr_node->is_trig_op())
    return;

  sscanf(line.c_str(), "%d,%lf,%d,%[^,],\n", &size, &value, &is_reg, label);
  assert(is_reg);

  assert(curr_node->is_call_op());
  Variable* var = srcManager.insert<Variable>(label);
  DynamicVariable unique_reg_ref(callee_dynamic_function, var);
  // Create a mapping between registers in caller and callee functions.
  if (unique_reg_in_caller_func) {
    program->call_arg_map.add(unique_reg_ref, unique_reg_in_caller_func);
    unique_reg_in_caller_func = DynamicVariable();
  }
  auto reg_it = register_last_written.find(unique_reg_ref);
  int tmp_written_inst = current_node_id;
  if (last_call_source != -1) {
    tmp_written_inst = last_call_source;
  }
  if (reg_it != register_last_written.end())
    reg_it->second = tmp_written_inst;
  else
    register_last_written[unique_reg_ref] = tmp_written_inst;
}

std::string DDDG::parse_function_name(std::string line) {
  char curr_static_function[256];
  char instid[256], bblockid[256];
  int line_num;
  int microop;
  int dyn_inst_count;
  sscanf(line.c_str(),
         "%d,%[^,],%[^,],%[^,],%d,%d\n",
         &line_num,
         curr_static_function,
         bblockid,
         instid,
         &microop,
         &dyn_inst_count);
  return curr_static_function;
}

bool DDDG::is_function_returned(std::string line, std::string target_function) {
  char curr_static_function[256];
  char instid[256], bblockid[256];
  int line_num;
  int microop;
  int dyn_inst_count;
  sscanf(line.c_str(),
         "%d,%[^,],%[^,],%[^,],%d,%d\n",
         &line_num,
         curr_static_function,
         bblockid,
         instid,
         &microop,
         &dyn_inst_count);
  if (microop == LLVM_IR_Ret &&
      (!target_function.compare(curr_static_function)))
    return true;
  return false;
}

size_t DDDG::build_initial_dddg(size_t trace_off, size_t trace_size) {

  std::cout << "-------------------------------" << std::endl;
  std::cout << "      Generating DDDG          " << std::endl;
  std::cout << "-------------------------------" << std::endl;

  long current_trace_off = trace_off;
  // Bigger traces would benefit from having a finer progress report.
  float increment = trace_size > 5e8 ? 0.01 : 0.05;
  // The total progress is the amount of the trace parsed.
  ProgressTracker trace_progress(
      "dddg_parse_progress.out", &current_trace_off, trace_size, increment);
  trace_progress.add_stat("nodes", &num_of_instructions);
  trace_progress.add_stat("bytes", &current_trace_off);

  char buffer[256];
  std::string first_function;
  bool seen_first_line = false;
  bool first_function_returned = false;
  bool in_labelmap_section = false;
  bool labelmap_parsed_or_not_present = false;
  trace_progress.start_epoch();
  while (trace_file && !gzeof(trace_file)) {
    if (gzgets(trace_file, buffer, sizeof(buffer)) == NULL) {
      continue;
    }
    current_trace_off = gzoffset(trace_file);
    if (trace_progress.at_epoch_end()) {
      trace_progress.start_new_epoch();
    }
    std::string wholeline(buffer);

    /* Scan for labelmap section if it has not yet been parsed. */
    if (!labelmap_parsed_or_not_present) {
      if (!in_labelmap_section) {
        if (wholeline.find("%%%% LABEL MAP START %%%%") != std::string::npos) {
          in_labelmap_section = true;
          continue;
        }
      } else {
        if (wholeline.find("%%%% LABEL MAP END %%%%") != std::string::npos) {
          labelmap_parsed_or_not_present = true;
          in_labelmap_section = false;
          continue;
        }
        parse_labelmap_line(wholeline);
      }
    }
    size_t pos_end_tag = wholeline.find(",");

    if (pos_end_tag == std::string::npos) {
      if (first_function_returned)
        break;
      continue;
    }
    // So that we skip that check if we don't have a labelmap.
    labelmap_parsed_or_not_present = true;
    std::string tag = wholeline.substr(0, pos_end_tag);
    std::string line_left = wholeline.substr(pos_end_tag + 1);
    if (tag.compare("0") == 0) {
      if (!seen_first_line) {
        seen_first_line = true;
        first_function = parse_function_name(line_left);
      }
      first_function_returned = is_function_returned(line_left, first_function);
      parse_instruction_line(line_left);
    } else if (tag.compare("r") == 0) {
      parse_result(line_left);
    } else if (tag.compare("f") == 0) {
      parse_forward(line_left);
    } else {
      parse_parameter(line_left, atoi(tag.c_str()));
    }
  }

  if (seen_first_line) {
    output_dddg();

    std::cout << "-------------------------------" << std::endl;
    std::cout << "Num of Nodes: " << program->getNumNodes() << std::endl;
    std::cout << "Num of Edges: " << program->getNumEdges() << std::endl;
    std::cout << "Num of Reg Edges: " << num_of_register_dependency()
              << std::endl;
    std::cout << "Num of MEM Edges: " << num_of_memory_dependency()
              << std::endl;
    std::cout << "Num of Control Edges: " << num_of_control_dependency()
              << std::endl;
    std::cout << "-------------------------------" << std::endl;
    return static_cast<size_t>(current_trace_off);
  } else {
    // The trace (or whatever was left) was empty.
    std::cout << "-------------------------------" << std::endl;
    std::cout << "Reached end of trace." << std::endl;
    std::cout << "-------------------------------" << std::endl;
    return END_OF_TRACE;
  }
}
