#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/renderer_backend.hpp>
#include <mln/style/layers/custom_layer_impl.hpp>
#include <mln/renderer/layers/render_custom_layer.hpp>
#include <mln/map/transform_state.hpp>
#include <mln/math/angles.hpp>
#include <mln/renderer/bucket.hpp>
#include <mln/renderer/paint_parameters.hpp>
#include <mln/util/mat4.hpp>

#include <mln/gfx/context.hpp>
#include <mln/renderer/layer_group.hpp>
#include <mln/renderer/render_terrain.hpp>
#include <mln/gfx/drawable_custom_layer_host_tweaker.hpp>
#include <mln/gfx/drawable_builder.hpp>

#if MLN_RENDER_BACKEND_METAL
#include <mln/style/layers/mtl/custom_layer_render_parameters.hpp>
#include <mln/mtl/render_pass.hpp>
#elif MLN_RENDER_BACKEND_VULKAN
#include <mln/vulkan/context.hpp>
#include <mln/vulkan/renderer_backend.hpp>
#include <mln/style/layers/vulkan/custom_layer_init_parameters.hpp>
#include <mln/style/layers/vulkan/custom_layer_render_parameters.hpp>
#include <mln/vulkan/render_pass.hpp>
#endif

// TODO: platform agnostic error checks
#define MBGL_CHECK_ERROR(cmd) (cmd)

namespace mln {

using namespace style;

namespace {

inline const CustomLayer::Impl& impl(const Immutable<style::Layer::Impl>& impl) {
    assert(impl->getTypeInfo() == CustomLayer::Impl::staticTypeInfo());
    return static_cast<const CustomLayer::Impl&>(*impl);
}

void initializeHost(const std::shared_ptr<style::CustomLayerHost>& host, [[maybe_unused]] gfx::Context& context) {
#if MLN_RENDER_BACKEND_VULKAN
    {
        auto& vkBackend = static_cast<mln::vulkan::Context&>(context).getBackend();
        style::vulkan::CustomLayerInitParameters params(
            vkBackend.getDispatcher(), vkBackend.getDevice().get(), vkBackend.getPhysicalDevice());
        host->initialize(params);
    }
#else
    {
        style::CustomLayerInitParameters params;
        host->initialize(params);
    }
#endif
}

/// Calls preRender, and asks a host on a map with terrain whether it drapes (SatSquatch)
std::optional<uint64_t> callPreRender(const std::shared_ptr<style::CustomLayerHost>& host,
                                      gfx::Context& context,
                                      const PaintParameters& paintParameters) {
#if MLN_RENDER_BACKEND_METAL
    style::mtl::CustomLayerRenderParameters parameters(paintParameters);
#elif MLN_RENDER_BACKEND_VULKAN
    style::vulkan::CustomLayerRenderParameters parameters(paintParameters);
#else
    style::CustomLayerRenderParameters parameters(paintParameters);
#endif
    host->preRender(context, parameters);
    if (!paintParameters.terrain || !paintParameters.terrain->getFrameMeshCover()) {
        return std::nullopt;
    }
    return host->drapeRevision(parameters);
}

} // namespace

RenderCustomLayer::RenderCustomLayer(Immutable<style::CustomLayer::Impl> _impl)
    : RenderLayer(makeMutable<CustomLayerProperties>(std::move(_impl))),
      host(impl(baseImpl).host) {
    assert(gfx::BackendScope::exists());
}

RenderCustomLayer::~RenderCustomLayer() {
    assert(gfx::BackendScope::exists());
    if (contextDestroyed) {
        host->contextLost();
    } else {
        MBGL_CHECK_ERROR(host->deinitialize());
    }
}

void RenderCustomLayer::evaluate(const PropertyEvaluationParameters&) {
    passes = RenderPass::Translucent;
    // It is fine to not update `evaluatedProperties`, as `baseImpl` should never be updated for this layer.
}

bool RenderCustomLayer::hasTransition() const {
    return false;
}
bool RenderCustomLayer::hasCrossfade() const {
    return false;
}

void RenderCustomLayer::markContextDestroyed() {
    contextDestroyed = true;
}

void RenderCustomLayer::prepare(const LayerPrepareParameters&) {}

void RenderCustomLayer::update([[maybe_unused]] gfx::ShaderRegistry& shaders,
                               gfx::Context& context,
                               [[maybe_unused]] const TransformState& state,
                               const std::shared_ptr<UpdateParameters>&,
                               [[maybe_unused]] const PaintParameters& paintParameters,
                               [[maybe_unused]] const RenderTree& renderTree,
                               [[maybe_unused]] UniqueChangeRequestVec& changes) {
    // check if host changed and update
    bool hostChanged = (host != impl(baseImpl).host);
    if (hostChanged) {
        // If the context changed, deinitialize the previous one before initializing the new one.
        if (host && !contextDestroyed) {
            MBGL_CHECK_ERROR(host->deinitialize());
        }
        host = impl(baseImpl).host;
        needsInitialize = true;
    }

    if (needsInitialize) {
        MBGL_CHECK_ERROR(initializeHost(host, context));
        needsInitialize = false;
    }

    // call the pre-render; draped onto the terrain while the host asks (SatSquatch)
    const std::optional<uint64_t> drape = callPreRender(host, context, paintParameters);
    const bool draped = drape.has_value();
    if (layerGroup && draped != layerGroupDraped) {
        removeAllDrawables();
        setLayerGroup(nullptr, changes);
    }

    // create layer group: drawn over the map, or into the terrain's tile textures (a tile group, a drawable per
    // drape target)
    if (!layerGroup) {
        LayerGroupBasePtr layerGroup_;
        if (draped) {
            layerGroup_ = context.createTileLayerGroup(layerIndex, /*initialCapacity=*/64, getID(), true);
        } else {
            layerGroup_ = context.createLayerGroup(layerIndex, /*initialCapacity=*/1, getID(), false);
        }
        if (!layerGroup_) {
            return;
        }
        setLayerGroup(std::move(layerGroup_), changes);
        layerGroupDraped = draped;
        hostChanged = true;
    }

    if (draped) {
        auto* tileGroup = static_cast<TileLayerGroup*>(layerGroup.get());
        tileGroup->setDrapeRevision(drape);
        const auto& cover = *paintParameters.terrain->getFrameMeshCover();
        // A drawable per drape target, as the cover changes (the new host's anew)
        if (hostChanged) {
            tileGroup->clearDrawables();
        }
        stats.drawablesRemoved += tileGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
            return !drawable.getTileID() || !cover.contains(drawable.getTileID()->toUnwrapped());
        });
        std::set<UnwrappedTileID> drawn;
        tileGroup->visitDrawables([&](const gfx::Drawable& drawable) {
            if (drawable.getTileID()) {
                drawn.insert(drawable.getTileID()->toUnwrapped());
            }
        });
        const auto tweaker = std::make_shared<gfx::DrawableCustomLayerDrapeTweaker>(host);
        for (const auto& tile : cover) {
            if (drawn.contains(tile)) {
                continue;
            }
            std::unique_ptr<gfx::DrawableBuilder> builder = context.createDrawableBuilder(getID());
            auto& drawable = builder->getCurrentDrawable(true);
            drawable->setIsCustom(true);
            drawable->setRenderPass(RenderPass::Translucent);
            drawable->addTweaker(tweaker);
            const OverscaledTileID tileID(tile.canonical.z, tile.wrap, tile.canonical);
            drawable->setTileID(tileID);
            tileGroup->addDrawable(RenderPass::Translucent, tileID, std::move(drawable));
            ++stats.drawablesAdded;
        }
        return;
    }

    auto* localLayerGroup = static_cast<LayerGroup*>(layerGroup.get());

    // create drawable
    if (localLayerGroup->getDrawableCount() == 0 || hostChanged) {
        localLayerGroup->clearDrawables();

        // create tweaker
        auto tweaker = std::make_shared<gfx::DrawableCustomLayerHostTweaker>(host);

        // create empty drawable using a builder
        std::unique_ptr<gfx::DrawableBuilder> builder = context.createDrawableBuilder(getID());
        auto& drawable = builder->getCurrentDrawable(true);
        drawable->setIsCustom(true);
        drawable->setRenderPass(RenderPass::Translucent);

        // assign tweaker to drawable
        drawable->addTweaker(tweaker);

        // add drawable to layer group
        localLayerGroup->addDrawable(std::move(drawable));
        ++stats.drawablesAdded;
    }
}

} // namespace mln
