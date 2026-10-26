#include <mln/gfx/drawable_custom_layer_host_tweaker.hpp>
#include <mln/renderer/paint_parameters.hpp>
#include <mln/renderer/render_target.hpp>
#include <mln/gfx/renderer_backend.hpp>
#include <mln/gfx/renderable.hpp>

#include <mln/gfx/drawable.hpp>
#include <mln/gfx/context.hpp>

#if MLN_RENDER_BACKEND_METAL
#include <mln/style/layers/mtl/custom_layer_render_parameters.hpp>
#include <mln/mtl/render_pass.hpp>
#elif MLN_RENDER_BACKEND_VULKAN
#include <mln/style/layers/vulkan/custom_layer_render_parameters.hpp>
#include <mln/vulkan/render_pass.hpp>
#endif

#include <mln/util/constants.hpp>
#include <mln/util/mat4.hpp>

#include <cmath>
#include <memory>

namespace mln {
namespace gfx {

void DrawableCustomLayerHostTweaker::execute([[maybe_unused]] gfx::Drawable& drawable,
                                             mln::PaintParameters& paintParameters) {
    // custom drawing
    auto& context = paintParameters.context;
    // With 3D terrain the layer draws in the 3D layers' depth range, as the terrain mesh does, so what it
    // draws can test against the ground (else, on GL, the range is its 2D sublayer's single depth, behind
    // all of it). (SatSquatch)
    auto depthMode = paintParameters.depthModeForSublayer(0, gfx::DepthMaskType::ReadOnly);
    if (paintParameters.terrain) {
        depthMode = paintParameters.depthModeFor3D();
        depthMode.mask = gfx::DepthMaskType::ReadOnly;
    }
    context.resetState(depthMode, paintParameters.colorModeForRenderPass());

#if MLN_RENDER_BACKEND_METAL
    const auto& mtlRenderPass = static_cast<mtl::RenderPass*>(paintParameters.renderPass.get());
    mtlRenderPass->resetState();

    style::mtl::CustomLayerRenderParameters parameters(paintParameters);
#elif MLN_RENDER_BACKEND_VULKAN
    style::vulkan::CustomLayerRenderParameters parameters(paintParameters);
#else
    style::CustomLayerRenderParameters parameters(paintParameters);
#endif

    host->render(parameters);

    // Reset the view back to our original one, just in case the CustomLayer
    // changed the viewport or Framebuffer.
    paintParameters.backend.getDefaultRenderable().getResource<gfx::RenderableResource>().bind();

    context.setDirtyState();
    context.bindGlobalUniformBuffers(*paintParameters.renderPass);
}

void DrawableCustomLayerDrapeTweaker::execute(gfx::Drawable& drawable, mln::PaintParameters& paintParameters) {
    // The drape target being rendered: {z, x with its world copy, y}
    const auto& target = paintParameters.currentDrapeTile;
    if (target[3] == 0) {
        return;
    }
    const auto z = static_cast<uint8_t>(target[0]);
    const double n = std::exp2(static_cast<double>(z));
    const double gx = target[1];
    const double y = target[2];

    // Only the target's own drawable draws: the host draws the whole target, and the drawables of the cover's tiles
    // that overlap it (the flat map's dilated cover holds a tile's ancestors and descendants too) would draw it
    // again, compounding a translucent layer
    const auto& tileID = drawable.getTileID();
    if (!tileID) {
        return;
    }
    const auto own = RenderTarget::drapeTile(tileID->toUnwrapped(), paintParameters.state.isGlobeRendering());
    if (own.canonical.z != z || own.canonical.y != y ||
        static_cast<double>(own.canonical.x) + static_cast<double>(own.wrap) * n != gx) {
        return;
    }

    // Flat into the tile's texture, with no depth or stencil test (as the 2D draped layers)
    auto& context = paintParameters.context;
    context.resetState(gfx::DepthMode::disabled(), paintParameters.colorModeForRenderPass());

#if MLN_RENDER_BACKEND_METAL
    const auto& mtlRenderPass = static_cast<mtl::RenderPass*>(paintParameters.renderPass.get());
    mtlRenderPass->resetState();
    style::mtl::CustomLayerRenderParameters parameters(paintParameters);
#elif MLN_RENDER_BACKEND_VULKAN
    style::vulkan::CustomLayerRenderParameters parameters(paintParameters);
#else
    style::CustomLayerRenderParameters parameters(paintParameters);
#endif

    // The draped layers' tile matrix (LayerTweaker::getTileMatrix: an orthographic 0..EXTENT tile, its clip z in
    // the backend's range), from world Mercator
    mat4 matrix;
    matrix::ortho(matrix, 0, util::EXTENT, util::EXTENT, 0, 0, 1);
#if !MLN_RENDER_BACKEND_OPENGL
    matrix[2] = 0.5 * (matrix[2] + matrix[3]);
    matrix[6] = 0.5 * (matrix[6] + matrix[7]);
    matrix[10] = 0.5 * (matrix[10] + matrix[11]);
    matrix[14] = 0.5 * (matrix[14] + matrix[15]);
#endif
    matrix::scale(matrix, matrix, util::EXTENT * n, util::EXTENT * n, 1);
    matrix::translate(matrix, matrix, -gx / n, -y / n, 0);

    const auto wrappedX = static_cast<uint32_t>(std::fmod(std::fmod(gx, n) + n, n));
    host->renderToTile(z, wrappedX, static_cast<uint32_t>(y), matrix, parameters);

    context.setDirtyState();
    context.bindGlobalUniformBuffers(*paintParameters.renderPass);
}

} // namespace gfx
} // namespace mln
