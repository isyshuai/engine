// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/render_pass.h"
#include "fml/status.h"
#include "impeller/base/validation.h"
#include "impeller/core/vertex_buffer.h"

namespace impeller {

RenderPass::RenderPass(std::shared_ptr<const Context> context,
                       const RenderTarget& target)
    : context_(std::move(context)),
      sample_count_(target.GetSampleCount()),
      pixel_format_(target.GetRenderTargetPixelFormat()),
      has_depth_attachment_(target.GetDepthAttachment().has_value()),
      has_stencil_attachment_(target.GetStencilAttachment().has_value()),
      render_target_size_(target.GetRenderTargetSize()),
      render_target_(target),
      orthographic_(Matrix::MakeOrthographic(render_target_size_)) {}

RenderPass::~RenderPass() {}

SampleCount RenderPass::GetSampleCount() const {
  return sample_count_;
}

PixelFormat RenderPass::GetRenderTargetPixelFormat() const {
  return pixel_format_;
}

bool RenderPass::HasDepthAttachment() const {
  return has_depth_attachment_;
}

bool RenderPass::HasStencilAttachment() const {
  return has_stencil_attachment_;
}

const RenderTarget& RenderPass::GetRenderTarget() const {
  return render_target_;
}

ISize RenderPass::GetRenderTargetSize() const {
  return render_target_size_;
}

const Matrix& RenderPass::GetOrthographicTransform() const {
  return orthographic_;
}

void RenderPass::SetLabel(std::string_view label) {
  if (label.empty()) {
    return;
  }
  OnSetLabel(label);
}

bool RenderPass::AddCommand(Command&& command) {
  if (!command.IsValid()) {
    VALIDATION_LOG << "Attempted to add an invalid command to the render pass.";
    return false;
  }

  if (command.scissor.has_value()) {
    auto target_rect = IRect::MakeSize(render_target_.GetRenderTargetSize());
    if (!target_rect.Contains(command.scissor.value())) {
      VALIDATION_LOG << "Cannot apply a scissor that lies outside the bounds "
                        "of the render target.";
      return false;
    }
  }

  if (command.element_count == 0u || command.instance_count == 0u) {
    // Essentially a no-op. Don't record the command but this is not necessary
    // an error either.
    return true;
  }

  commands_.emplace_back(std::move(command));
  return true;
}

bool RenderPass::EncodeCommands() const {
  return OnEncodeCommands(*context_);
}

const std::shared_ptr<const Context>& RenderPass::GetContext() const {
  return context_;
}

void RenderPass::SetPipeline(
    const std::shared_ptr<Pipeline<PipelineDescriptor>>& pipeline) {
  pending_.pipeline = pipeline;
}

void RenderPass::SetCommandLabel(std::string_view label) {
#ifdef IMPELLER_DEBUG
  pending_.label = std::string(label);
#endif  // IMPELLER_DEBUG
}

void RenderPass::SetStencilReference(uint32_t value) {
  pending_.stencil_reference = value;
}

void RenderPass::SetBaseVertex(uint64_t value) {
  pending_.base_vertex = value;
}

void RenderPass::SetViewport(Viewport viewport) {
  pending_.viewport = viewport;
}

void RenderPass::SetScissor(IRect scissor) {
  pending_.scissor = scissor;
}

void RenderPass::SetElementCount(size_t count) {
  pending_.element_count = count;
}

void RenderPass::SetInstanceCount(size_t count) {
  pending_.instance_count = count;
}

bool RenderPass::SetVertexBuffer(VertexBuffer buffer) {
  if (!SetVertexBuffer(&buffer.vertex_buffer, 1u)) {
    return false;
  }
  if (!SetIndexBuffer(buffer.index_buffer, buffer.index_type)) {
    return false;
  }
  SetElementCount(buffer.vertex_count);

  return true;
}

bool RenderPass::SetVertexBuffer(BufferView vertex_buffer) {
  return SetVertexBuffer(&vertex_buffer, 1);
}

bool RenderPass::SetVertexBuffer(std::vector<BufferView> vertex_buffers) {
  return SetVertexBuffer(vertex_buffers.data(), vertex_buffers.size());
}

bool RenderPass::SetVertexBuffer(BufferView vertex_buffers[],
                                 size_t vertex_buffer_count) {
  if (!ValidateVertexBuffers(vertex_buffers, vertex_buffer_count)) {
    return false;
  }

  pending_.vertex_buffer_count = vertex_buffer_count;
  for (size_t i = 0; i < vertex_buffer_count; i++) {
    pending_.vertex_buffers[i] = std::move(vertex_buffers[i]);
  }
  return true;
}

bool RenderPass::SetIndexBuffer(BufferView index_buffer, IndexType index_type) {
  if (!ValidateIndexBuffer(index_buffer, index_type)) {
    return false;
  }

  pending_.index_buffer = std::move(index_buffer);
  pending_.index_type = index_type;
  return true;
}

bool RenderPass::ValidateVertexBuffers(const BufferView vertex_buffers[],
                                       size_t vertex_buffer_count) {
  if (vertex_buffer_count > kMaxVertexBuffers) {
    VALIDATION_LOG << "Attempted to bind " << vertex_buffer_count
                   << " vertex buffers, but the maximum is "
                   << kMaxVertexBuffers << ".";
    return false;
  }

  for (size_t i = 0; i < vertex_buffer_count; i++) {
    if (!vertex_buffers[i]) {
      VALIDATION_LOG << "Attempted to bind an invalid vertex buffer.";
      return false;
    }
  }

  return true;
}

bool RenderPass::ValidateIndexBuffer(const BufferView& index_buffer,
                                     IndexType index_type) {
  if (index_type == IndexType::kUnknown) {
    VALIDATION_LOG << "Cannot bind an index buffer with an unknown index type.";
    return false;
  }

  if (index_type != IndexType::kNone && !index_buffer) {
    VALIDATION_LOG << "Attempted to bind an invalid index buffer.";
    return false;
  }

  return true;
}

fml::Status RenderPass::Draw() {
  auto result = AddCommand(std::move(pending_));
  pending_ = Command{};
  if (result) {
    return fml::Status();
  }
  return fml::Status(fml::StatusCode::kInvalidArgument,
                     "Failed to encode command");
}

// |ResourceBinder|
bool RenderPass::BindResource(ShaderStage stage,
                              DescriptorType type,
                              const ShaderUniformSlot& slot,
                              const ShaderMetadata* metadata,
                              BufferView view) {
  return pending_.BindResource(stage, type, slot, metadata, view);
}

// |ResourceBinder|
bool RenderPass::BindResource(ShaderStage stage,
                              DescriptorType type,
                              const SampledImageSlot& slot,
                              const ShaderMetadata* metadata,
                              std::shared_ptr<const Texture> texture,
                              const std::unique_ptr<const Sampler>& sampler) {
  return pending_.BindResource(stage, type, slot, metadata, std::move(texture),
                               sampler);
}

bool RenderPass::BindDynamicResource(ShaderStage stage,
                                     DescriptorType type,
                                     const ShaderUniformSlot& slot,
                                     std::unique_ptr<ShaderMetadata> metadata,
                                     BufferView view) {
  return pending_.BindDynamicResource(stage, type, slot, std::move(metadata),
                                      std::move(view));
}

bool RenderPass::BindDynamicResource(
    ShaderStage stage,
    DescriptorType type,
    const SampledImageSlot& slot,
    std::unique_ptr<ShaderMetadata> metadata,
    std::shared_ptr<const Texture> texture,
    const std::unique_ptr<const Sampler>& sampler) {
  return pending_.BindDynamicResource(stage, type, slot, std::move(metadata),
                                      std::move(texture), sampler);
}

}  // namespace impeller
