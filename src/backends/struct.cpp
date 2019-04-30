#include "../ir.h"
#include "../program.h"
#include "struct.h"

TLANG_NAMESPACE_BEGIN

StructCompiler::StructCompiler() : CodeGenBase() {
  snode_count = 0;
  creator = nullptr;
  if (get_current_program().config.arch == Arch::x86_64)
    suffix = "cpp";
  else
    suffix = "cu";
  if (get_current_program().config.debug) {
    emit("#define TL_DEBUG");
  }
  emit("#define TL_HOST");
  emit("#include <kernel.h>");
  emit(" namespace taichi {{");
  emit(" namespace Tlang {{");
  emit("\n");
}

void StructCompiler::visit(SNode &snode) {
  snodes.push_back(&snode);
  // TC_P(snode.type_name());
  for (int ch_id = 0; ch_id < (int)snode.ch.size(); ch_id++) {
    auto &ch = snode.ch[ch_id];
    for (int i = 0; i < max_num_indices; i++) {
      bool found = false;
      for (int k = 0; k < max_num_indices; k++) {
        if (snode.physical_index_position[k] == i) {
          found = true;
          break;
        }
      }
      if (found)
        continue;
      if (snode.extractors[i].active) {
        snode.physical_index_position[snode.num_active_indices++] = i;
      }
    }
    /*
    TC_TAG;
    for (int i = 0; i < max_num_indices; i++) {
      fmt::print("{}, ", snode.physical_index_position[i]);
    }
    fmt::print("\n");
    */
    std::memcpy(ch->physical_index_position, snode.physical_index_position,
                sizeof(snode.physical_index_position));
    ch->num_active_indices = snode.num_active_indices;
    visit(*ch);

    // TC_P(ch->type_name());
    int total_bits_start_inferred = ch->total_bit_start + ch->total_num_bits;
    // TC_P(ch->total_bit_start);
    // TC_P(ch->total_num_bits);
    if (ch_id == 0) {
      snode.total_bit_start = total_bits_start_inferred;
    } else if (snode.parent != nullptr) {  // root is ok
      // TC_ASSERT(snode.total_bit_start == total_bits_start_inferred);
    }
    // infer extractors
    int acc_offsets = 0;
    for (int i = max_num_indices - 1; i >= 0; i--) {
      int inferred = ch->extractors[i].start + ch->extractors[i].num_bits;
      if (ch_id == 0) {
        snode.extractors[i].start = inferred;
        snode.extractors[i].acc_offset = acc_offsets;
      } else if (snode.parent != nullptr) {  // root is OK
        /*
        TC_ASSERT_INFO(snode.extractors[i].start == inferred,
                       "Inconsistent bit configuration");
        TC_ASSERT_INFO(snode.extractors[i].dest_offset ==
                           snode.total_bit_start + acc_offsets,
                       "Inconsistent bit configuration");
                       */
      }
      acc_offsets += snode.extractors[i].num_bits;
    }
  }

  snode.total_num_bits = 0;
  for (int i = 0; i < max_num_indices; i++) {
    snode.total_num_bits += snode.extractors[i].num_bits;
  }

  emit("");
  snode.node_type_name = create_snode();
  auto type = snode.type;

  if (snode.type != SNodeType::indirect && snode.type != SNodeType::place &&
      snode.ch.empty()) {
    TC_ERROR("Non-place node should have at least one child.");
  }

  // create children type that supports forking...
  emit("struct {}_ch {{", snode.node_type_name);
  for (int i = 0; i < (int)snode.ch.size(); i++) {
    emit("{} member{};", snode.ch[i]->node_type_name, i);
  }
  if (snode.ch.size() == 1 && snode.ch[0]->type == SNodeType::place) {
    emit("TC_DEVICE {}_ch({} v) {{*get0()=v;}}", snode.node_type_name,
         snode.ch[0]->node_type_name);
    emit("TC_DEVICE {}_ch() = default;", snode.node_type_name);
  }
  for (int i = 0; i < (int)snode.ch.size(); i++) {
    emit("TC_DEVICE {} *get{}() {{return &member{};}} ",
         snode.ch[i]->node_type_name, i, i);
  }
  emit("}};");

  if (type == SNodeType::dense) {
    emit("using {} = dense<{}_ch, {}>;", snode.node_type_name,
         snode.node_type_name, snode.n);
  } else if (type == SNodeType::root) {
    emit("using {} = layout_root<{}_ch>;", snode.node_type_name,
         snode.node_type_name);
  } else if (type == SNodeType::dynamic) {
    emit("using {} = dynamic<{}_ch, {}>;", snode.node_type_name,
         snode.node_type_name, snode.n);
  } else if (type == SNodeType::indirect) {
    emit("using {} = indirect<{}_ch>;", snode.node_type_name, snode.n);
  } else if (type == SNodeType::pointer) {
    emit("using {} = pointer<{}_ch>;", snode.node_type_name,
         snode.node_type_name);
  } else if (type == SNodeType::hashed) {
    emit("using {} = hashed<{}_ch>;", snode.node_type_name,
         snode.node_type_name);
  } else if (type == SNodeType::place) {
    emit(
        "struct {} {{ using val_type = {}; val_type val; TC_DEVICE operator "
        "{}() {{return val;}} "
        "TC_DEVICE {}(){{}} TC_DEVICE {}({} val) "
        ": val(val){{ }} }};",
        snode.node_type_name, snode.data_type_name(), snode.data_type_name(),
        snode.node_type_name, snode.node_type_name, snode.data_type_name());
  } else {
    TC_P(snode.type_name());
    TC_NOT_IMPLEMENTED;
  }

  if (snode.has_ambient) {
    emit("{} {}_ambient({});", snode.node_type_name, snode.node_type_name,
         snode.ambient_val.stringify());
  }
}

void StructCompiler::generate_leaf_accessors(SNode &snode) {
  auto type = snode.type;
  stack.push_back(&snode);

  bool is_leaf = type == SNodeType::place || type == SNodeType ::indirect;

  if (!is_leaf) {
    // Chain accessors for non-leaf nodes
    TC_ASSERT(snode.ch.size() > 0);
    for (int i = 0; i < (int)snode.ch.size(); i++) {
      auto ch = snode.ch[i];
      emit(
          "TLANG_ACCESSOR {} *access_{}({} *parent, int i "
          ") {{",
          ch->node_type_name, ch->node_type_name, snode.node_type_name);
      // emit("#if defined(TC_STRUCT)");
      // emit("parent->activate(i, index);");
      // emit("#endif");
      emit("auto lookup = parent->look_up(i); ");
      emit("if ({}::has_null && lookup == nullptr) ", snode.node_type_name);
      emit("return nullptr;");
      emit("return lookup->get{}();", i);
      emit("}}");
    }
    emit("");
  }
  // SNode::place & indirect
  // emit end2end accessors for leaf (place) nodes, using chain accessors
  TC_ASSERT(max_num_indices == 4);
  constexpr int mode_access = 0;
  constexpr int mode_activate = 1;

  for (auto mode : {mode_access, mode_activate}) {
    auto verb = mode == mode_activate ? "activate" : "access";
    emit(
        "TLANG_ACCESSOR TC_EXPORT {} * {}_{}(void *root, int i0=0, int i1=0, "
        "int "
        "i2=0, "
        "int i3=0) {{",
        snode.node_type_name, verb, snode.node_type_name);
    if (snode._verbose) {
      emit(
          "std::cout << \"accessing node {} at \" << i0 << ' ' << i1 << ' ' "
          "<< i2 << ' ' << i3 << std::endl;",
          snode.node_type_name);
    }
    emit("int tmp;");
    emit("auto n0 = ({} *)root;", root_type);
    for (int i = 0; i + 1 < (int)stack.size(); i++) {
      emit("tmp = 0;", i);
      for (int j = 0; j < max_num_indices; j++) {
        auto e = stack[i]->extractors[j];
        int b = e.num_bits;
        if (b) {
          if (e.num_bits == e.start || max_num_indices != 1) {
            emit("tmp = (tmp << {}) + ((i{} >> {}) & ((1 << {}) - 1));",
                 e.num_bits, j, e.start, e.num_bits);
          } else {
            TC_WARN("Emitting shortcut indexing");
            emit("tmp = i{};", j);
          }
        }
      }
      bool force_activate = true;
      if (mode != mode_activate) {
        if (force_activate)
          emit("#if 1");
        else
          emit("#if defined(TC_STRUCT)");
      }
      if (stack[i]->type != SNodeType::place)
        emit("n{}->activate(tmp, {{i0, i1, i2, i3}});", i);
      if (mode != mode_activate) {
        emit("#endif");
      }
      emit("auto n{} = access_{}(n{}, tmp);", i + 1,
           stack[i + 1]->node_type_name, i);
      if (mode == mode_access)
        if (snode.has_ambient && stack[i + 1] != &snode)
          emit("if ({}::has_null && n{} == nullptr) return &{}_ambient;",
               stack[i]->node_type_name, i + 1, snode.node_type_name);
    }
    emit("return n{};", (int)stack.size() - 1);
    emit("}}");
    emit("");
  }

  for (auto ch : snode.ch) {
    generate_leaf_accessors(*ch);
  }

  stack.pop_back();
}

void StructCompiler::load_accessors(SNode &snode) {
  for (auto ch : snode.ch) {
    load_accessors(*ch);
  }
  if (snode.type == SNodeType::place) {
    snode.access_func = load_function<SNode::AccessorFunction>(
        fmt::format("access_{}", snode.node_type_name));
  } else {
    snode.stat_func = load_function<SNode::StatFunction>(
        fmt::format("stat_{}", snode.node_type_name));
  }
  if (snode.type == SNodeType::pointer) {
    snode.clear_func = load_function<SNode::ClearFunction>(
        fmt::format("clear_{}", snode.node_type_name));
  }
}

void StructCompiler::set_parents(SNode &snode) {
  for (auto &c : snode.ch) {
    set_parents(*c);
    c->parent = &snode;
  }
}

void StructCompiler::run(SNode &node) {
  set_parents(node);
  // bottom to top
  visit(node);

  for (int i = 0; i < snodes.size(); i++) {
    // if (snodes[i]->type != SNodeType::place)
    emit(
        "template <> struct SNodeID<{}> {{static constexpr int value = "
        "{};}};",
        snodes[i]->node_type_name, snodes[i]->id);
  }

  for (int i = 0; i < snodes.size(); i++) {
    auto snode = snodes[i];
    emit(
        "template <> __host__ __device__ void "
        "get_corner_coord<{}>(const "
        "PhysicalIndexGroup &indices, PhysicalIndexGroup &output) {{",
        snode->node_type_name);
    for (int j = 0; j < max_num_indices; j++) {
      auto e = snode->extractors[j];
      emit("output[{}] = indices[{}] & (~((1 << {}) - 1));", j, j,
           e.start + e.num_bits);
    }
    emit("}}");
  }

  for (int i = 0; i < snodes.size(); i++) {
    if (snodes[i]->type != SNodeType::place)
      emit(
          "TC_EXPORT AllocatorStat stat_{}() {{return "
          "Managers::get_allocator<{}>()->get_stat();}} ",
          snodes[i]->node_type_name, snodes[i]->node_type_name);
    if (snodes[i]->type == SNodeType::pointer) {
      emit(
          "TC_EXPORT void clear_{}(int flags) {{"
          "Managers::get_allocator<{}>()->clear(flags);}} ",
          snodes[i]->node_type_name, snodes[i]->node_type_name);
    }
  }

  root_type = node.node_type_name;
  generate_leaf_accessors(node);
  emit("#if defined(TC_STRUCT)");
  emit("TC_EXPORT void *create_data_structure() {{");

  emit("Managers::initialize();");

  TC_ASSERT(snodes.size() <= max_num_snodes);
  for (int i = 0; i < snodes.size(); i++) {
    // if (snodes[i]->type == SNodeType::pointer ||
    // snodes[i]->type == SNodeType::hashed) {
    if (snodes[i]->type != SNodeType::place) {
      emit(
          "Managers::get<{}>() = "
          "create_unified<SNodeManager<{}>>();",
          snodes[i]->node_type_name, snodes[i]->node_type_name);
    }
  }

  emit(
      "auto p = Managers::get_allocator<{}>()->allocate_node({{0, 0, 0, "
      "0}})->ptr;",
      root_type);

  emit("return p;}}");

  emit("#if !defined(TLANG_GPU)");
  // emit("CPUProfiler profiler;");
  emit("#endif");

  emit("TC_EXPORT void release_data_structure(void *ds) {{delete ({} *)ds;}}",
       root_type);
  emit("TC_EXPORT void profiler_print()");
  emit("{{");
  emit("#if defined(TLANG_GPU)");
  emit("GPUProfiler::get_instance().print();");
  emit("#else");
  // emit("profiler.print();");
  emit("#endif");
  emit("}}");
  emit("#endif");
  emit("#if !defined(TLANG_GPU)");
  // emit("extern CPUProfiler profiler;");
  emit("#endif");
  emit("}} }}");
  write_source();

  generate_binary("-DTC_STRUCT");
  load_dll();
  creator = load_function<void *(*)()>("create_data_structure");
  profiler_print = load_function<void (*)()>("profiler_print");
  load_accessors(node);
}

TLANG_NAMESPACE_END
