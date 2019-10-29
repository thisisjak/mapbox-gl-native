#pragma once
#include <mbgl/layout/layout.hpp>
#include <mbgl/renderer/bucket_parameters.hpp>
#include <mbgl/geometry/feature_index.hpp>
#include <mbgl/renderer/render_layer.hpp>
#include <mbgl/style/layer_properties.hpp>

namespace mbgl {

using PatternLayerMap = std::map<std::string, PatternDependency>;

class CircleFeature  {
public:
    friend bool operator < (const CircleFeature& lhs, const CircleFeature& rhs) {
        return lhs.sortKey < rhs.sortKey;
    }

    uint32_t i;
    std::unique_ptr<GeometryTileFeature> feature;
    float sortKey;
};

class CircleLayout : public Layout {
public:
    CircleLayout(const BucketParameters& parameters,
                  const std::vector<Immutable<style::LayerProperties>>& group,
                  std::unique_ptr<GeometryTileLayer> sourceLayer_)
                  : sourceLayer(std::move(sourceLayer_)),
                    zoom(parameters.tileID.overscaledZ),
                    mode(parameters.mode),
                    hasPattern(false) {
        assert(!group.empty());
        auto leaderLayerProperties = staticImmutableCast<style::CircleLayerProperties>(group.front());
        unevaluatedLayout = leaderLayerProperties->layerImpl().layout;
        layout = unevaluatedLayout.evaluate(PropertyEvaluationParameters(zoom));
        sourceLayerID = leaderLayerProperties->layerImpl().sourceLayer;
        bucketLeaderID = leaderLayerProperties->layerImpl().id;

        for (const auto& layerProperties : group) {
            const std::string& layerId = layerProperties->baseImpl->id;
            layerPropertiesMap.emplace(layerId, layerProperties);
        }

        const size_t featureCount = sourceLayer->featureCount();
        for (size_t i = 0; i < featureCount; ++i) {
            auto feature = sourceLayer->getFeature(i);
            if (!leaderLayerProperties->layerImpl().filter(style::expression::EvaluationContext { this->zoom, feature.get() }))
                continue;

            auto sortKey = evaluateSortKey(*feature);
            features.push_back({static_cast<uint32_t>(i), std::move(feature), sortKey});
        }

        std::sort(features.begin(), features.end());
    };

    ~CircleLayout() final = default;

    bool hasDependencies() const override {
        return hasPattern;
    }

    void createBucket(const ImagePositions&, std::unique_ptr<FeatureIndex>& featureIndex, std::unordered_map<std::string, LayerRenderData>& renderData, const bool, const bool) override {
        auto bucket = std::make_shared<CircleBucket>(layerPropertiesMap, mode, zoom);

        for (auto & circleFeature : features) {
            const auto i = circleFeature.i;
            std::unique_ptr<GeometryTileFeature> feature = std::move(circleFeature.feature);
            const GeometryCollection& geometries = feature->getGeometries();

            bucket->addFeature(*feature, geometries, {}, {}, i);
            featureIndex->insert(geometries, i, sourceLayerID, bucketLeaderID);
        }
        if (bucket->hasData()) {
            for (const auto& pair : layerPropertiesMap) {
                renderData.emplace(pair.first, LayerRenderData {bucket, pair.second});
            }
        }
    };

private:
    float evaluateSortKey(const GeometryTileFeature& sourceFeature) {
        const auto sortKeyProperty = layout.template get<style::CircleSortKey>();
        return sortKeyProperty.evaluate(sourceFeature, zoom, style::CircleSortKey::defaultValue());
    }

    std::map<std::string, Immutable<style::LayerProperties>> layerPropertiesMap;
    std::string bucketLeaderID;

    const std::unique_ptr<GeometryTileLayer> sourceLayer;
    std::vector<CircleFeature> features;
    typename style::CircleLayoutProperties::Unevaluated unevaluatedLayout;
    typename style::CircleLayoutProperties::PossiblyEvaluated layout;

    const float zoom;
    const MapMode mode;
    std::string sourceLayerID;
    bool hasPattern;
};

} // namespace mbgl

