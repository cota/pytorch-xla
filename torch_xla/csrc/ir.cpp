#include "torch_xla/csrc/ir.h"

#include <torch/csrc/lazy/core/config.h>
#include <torch/csrc/lazy/core/hash.h>
#include <torch/csrc/lazy/core/ir_metadata.h>
#include <torch/csrc/lazy/python/python_util.h>

#include <functional>
#include <sstream>

#include "absl/strings/str_cat.h"
#include "torch_xla/csrc/lowering_context.h"
#include "torch_xla/csrc/runtime/cache.h"
#include "torch_xla/csrc/runtime/debug_macros.h"
#include "torch_xla/csrc/runtime/sys_util.h"

namespace torch_xla {
namespace {

using ShapeCache = runtime::util::Cache<torch::lazy::hash_t, xla::Shape,
                                        torch::lazy::HashReducer>;

ShapeCache* GetShapeCache() {
  static int64_t shape_cache_size =
      runtime::sys_util::GetEnvInt("XLA_IR_SHAPE_CACHE_SIZE", 12288);
  static ShapeCache* cache = new ShapeCache(shape_cache_size);
  return cache;
}

torch::lazy::hash_t GetOperandHashes(const torch::lazy::OpList& operands,
                                     const torch::lazy::hash_t& node_hash) {
  torch::lazy::hash_t hash = node_hash;
  for (auto& operand : operands) {
    if (!operand) {
      hash = torch::lazy::HashCombine(
          hash, static_cast<uint64_t>(torch::lazy::kNullOpt));
      continue;
    }
    hash = torch::lazy::HashCombine(hash, operand.hash());
  }
  return hash;
}

}  // namespace

XlaNode::XlaNode(torch::lazy::OpKind op, torch::lazy::OpList operands,
                 std::vector<torch::lazy::Shape>&& shapes, xla::Shape xla_shape,
                 size_t num_outputs, torch::lazy::hash_t hash_seed)
    : torch::lazy::Node(op, operands, std::move(shapes), num_outputs),
      xla_shape_(std::move(xla_shape)),
      node_hash_(torch::lazy::HashCombine(op.hash(), hash_seed)),
      dag_hash_(GetOperandHashes(operands, node_hash_)) {}

XlaNode::XlaNode(torch::lazy::OpKind op, torch::lazy::OpList operands,
                 std::vector<torch::lazy::Shape>&& shapes,
                 const std::function<xla::Shape()>& xla_shape_fn,
                 size_t num_outputs, torch::lazy::hash_t hash_seed)
    : torch::lazy::Node(op, operands, std::move(shapes), num_outputs),
      node_hash_(torch::lazy::HashCombine(op.hash(), hash_seed)),
      dag_hash_(GetOperandHashes(operands, node_hash_)) {
  xla_shape_ = GetOpShape(xla_shape_fn);
}

XlaNode::XlaNode(torch::lazy::OpKind op, torch::lazy::OpList operands,
                 torch::lazy::Shape shape, xla::Shape xla_shape,
                 size_t num_outputs, torch::lazy::hash_t hash_seed)
    : torch::lazy::Node(op, operands, std::vector<torch::lazy::Shape>{shape},
                        num_outputs),
      xla_shape_(std::move(xla_shape)),
      node_hash_(torch::lazy::HashCombine(op.hash(), hash_seed)),
      dag_hash_(GetOperandHashes(operands, node_hash_)) {}

XlaNode::XlaNode(torch::lazy::OpKind op, torch::lazy::OpList operands,
                 xla::Shape xla_shape, size_t num_outputs,
                 torch::lazy::hash_t hash_seed)
    : XlaNode(op, operands, std::vector<torch::lazy::Shape>{}, xla_shape,
              num_outputs, hash_seed) {}

XlaNode::XlaNode(torch::lazy::OpKind op, torch::lazy::OpList operands,
                 const std::function<torch::lazy::Shape()>& shape_fn,
                 const std::function<xla::Shape()>& xla_shape_fn,
                 size_t num_outputs, torch::lazy::hash_t hash_seed)
    : XlaNode(std::move(op), operands, xla::Shape(), num_outputs, hash_seed) {
  // Forward the constructor to the one above (with empty shape), so we have the
  // full hash information, then fetch/compute the real shape.
  addComputedShape(shape_fn);
  xla_shape_ = GetOpShape(xla_shape_fn);
}

XlaNode::XlaNode(torch::lazy::OpKind op, torch::lazy::OpList operands,
                 const std::function<xla::Shape()>& xla_shape_fn,
                 size_t num_outputs, torch::lazy::hash_t hash_seed)
    : XlaNode(std::move(op), operands, xla::Shape(), num_outputs, hash_seed) {
  // Forward the constructor to the one above (with empty shape), so we have the
  // full hash information, then fetch/compute the real shape.
  xla_shape_ = GetOpShape(xla_shape_fn);
}

XlaNode::XlaNode(torch::lazy::OpKind op, torch::lazy::Shape shape,
                 xla::Shape xla_shape, size_t num_outputs,
                 torch::lazy::hash_t hash_seed)
    : torch::lazy::Node(op, shape, num_outputs),
      xla_shape_(std::move(xla_shape)),
      node_hash_(GetOpHash(op, xla_shape_, hash_seed)),
      dag_hash_(node_hash_) {}

XlaNode::XlaNode(torch::lazy::OpKind op, xla::Shape xla_shape,
                 size_t num_outputs, torch::lazy::hash_t hash_seed)
    : XlaNode(op, torch::lazy::Shape(), xla_shape, num_outputs, hash_seed) {}

XlaNode::~XlaNode() {}

const xla::Shape& XlaNode::xla_shape(size_t output_index) const {
  if (xla_shape_.IsTuple()) {
    return xla_shape_.tuple_shapes(output_index);
  }
  XLA_CHECK_EQ(output_index, 0);
  return xla_shape_;
}

XlaOpVector XlaNode::ReturnOp(xla::XlaOp op, LoweringContext* loctx) const {
  XLA_CHECK_EQ(num_outputs(), 1);
  loctx->AssignOutputOp(torch::lazy::Output(this), op);
  return XlaOpVector({std::move(op)});
}

XlaOpVector XlaNode::ReturnOps(absl::Span<const xla::XlaOp> ops,
                               LoweringContext* loctx) const {
  XLA_CHECK_EQ(num_outputs(), ops.size());
  XlaOpVector result;
  for (size_t i = 0; i < ops.size(); ++i) {
    loctx->AssignOutputOp(torch::lazy::Output(this, i), ops[i]);
    result.push_back(ops[i]);
  }
  return result;
}

torch::lazy::NodePtr XlaNode::Clone(torch::lazy::OpList operands) const {
  XLA_ERROR() << "Cloning not implemented for node: " << *this;
}

XlaOpVector XlaNode::Lower(LoweringContext* loctx) const {
  XLA_ERROR() << "Lowering not implemented for node: " << *this;
}

torch::lazy::hash_t XlaNode::GetOpHash(torch::lazy::OpKind op,
                                       const xla::Shape& shape,
                                       torch::lazy::hash_t hash_seed) {
  torch::lazy::hash_t h =
      torch::lazy::HashCombine(op.hash(), torch::lazy::Hash(shape.ToString()));
  return torch::lazy::HashCombine(h, hash_seed);
}

void XlaNode::SetSharding(const xla::OpSharding& sharding, size_t index) {
  if (output_shardings_.size() == 0) {
    output_shardings_ =
        std::vector<std::shared_ptr<xla::OpSharding>>(num_outputs());
  }
  output_shardings_[index] = std::make_shared<xla::OpSharding>(sharding);
  // TODO(JackCaoG): fix this hashing
  UpdateShardingHash();
}

xla::Shape XlaNode::GetOpShape(
    const std::function<xla::Shape()>& shape_fn) const {
  ShapeCache* shape_cache = GetShapeCache();
  auto shape = shape_cache->Get(hash());
  if (shape == nullptr) {
    shape = shape_cache->Add(hash(), std::make_shared<xla::Shape>(shape_fn()));
  }
  return *shape;
}

std::string XlaNode::ToString() const {
  std::stringstream ss;
  ss << torch::lazy::Node::ToString() << ", xla_shape=" << xla_shape_;
  ss << ", dynamic_dims: (" << absl::StrJoin(unbounded_dynamic_dims_, ", ")
     << ')';
  return ss.str();
}

const xla::Shape& GetXlaShape(const torch::lazy::Value& value) {
  XlaNode* casted = dynamic_cast<XlaNode*>(value.node.get());
  return casted->xla_shape(value.index);
}

// The sharding hash is only based on relevant fields from the xla::OpSharding
// object. We skip the field that's irrelevant, which is the layout.
void XlaNode::UpdateShardingHash() {
  sharding_hash_ = node_hash_;
  for (size_t i = 0; i < output_shardings_.size(); i++) {
    // keep the index as part of the hash
    sharding_hash_ = torch::lazy::HashCombine(sharding_hash_, (uint32_t)i);
    std::shared_ptr<xla::OpSharding> sharding = output_shardings_[i];
    // skip the hash compute for empty sharding
    if (!sharding) {
      continue;
    }
    for (const auto& tile_assignment_dimension :
         sharding->tile_assignment_dimensions()) {
      sharding_hash_ = torch::lazy::HashCombine(
          sharding_hash_, (uint32_t)tile_assignment_dimension);
    }
    {
      const int64_t* data = sharding->tile_assignment_devices().data();
      const size_t size_in_bytes =
          sharding->tile_assignment_devices().size() * sizeof(*data);
      sharding_hash_ =
          torch::lazy::HashBlock(data, size_in_bytes, sharding_hash_);
    }
    for (const auto& last_tile_dim : sharding->last_tile_dims()) {
      sharding_hash_ =
          torch::lazy::HashCombine(sharding_hash_, (uint32_t)last_tile_dim);
    }
    sharding_hash_ =
        torch::lazy::HashCombine(sharding_hash_, (uint32_t)sharding->type());
    sharding_hash_ = torch::lazy::HashCombine(
        sharding_hash_, (uint32_t)sharding->replicate_on_last_tile_dim());

    xla::ShapeProto shape_proto = sharding->tile_shape();
    sharding_hash_ = torch::lazy::HashCombine(
        sharding_hash_, (uint32_t)shape_proto.element_type());
    for (const auto& dim : shape_proto.dimensions()) {
      sharding_hash_ = torch::lazy::HashCombine(sharding_hash_, (uint32_t)dim);
    }
    for (const auto& is_dyn_dim : shape_proto.is_dynamic_dimension()) {
      sharding_hash_ =
          torch::lazy::HashCombine(sharding_hash_, (uint32_t)is_dyn_dim);
    }
  }
}

void XlaNode::SetCustomOpName(const std::string& op_name) {
  custom_op_name_ = op_name;
}

}  // namespace torch_xla
