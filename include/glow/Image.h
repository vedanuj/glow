#ifndef GLOW_IMAGE_H
#define GLOW_IMAGE_H

#include "glow/Node.h"
#include "glow/Tensor.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

namespace glow {

class PNGNode final : public NodeBase {
public:
  explicit PNGNode(Network *N) {}

  virtual std::string getName() const override { return "PNGNode"; }

  bool writeImage(const char *filename);

  bool readImage(const char *filename);

  void init(Context *ctx) const override {}

  void forward(Context *ctx, PassKind kind) const override {}

  void backward(Context *ctx) const override {}

  void visit(NodeVisitor *visitor) override;
};

} // namespace glow

#endif // GLOW_IMAGE_H