#pragma once
#include <list>
#include <mbgl/geometry/feature_index.hpp>
#include <mbgl/layout/layout.hpp>
#include <mbgl/renderer/bucket_parameters.hpp>
#include <mbgl/renderer/render_layer.hpp>
#include <mbgl/style/expression/image.hpp>
#include <mbgl/style/layer_properties.hpp>

namespace mbgl {

class PatternDependency {
public:
    std::string min;
    std::string mid;
    std::string max;
};

using PatternLayerMap = std::map<std::string, PatternDependency>;

class PatternFeature  {
public:
    using IndexType = size_t;

    PatternFeature(const IndexType& i_,
                   std::unique_ptr<GeometryTileFeature>&& feature_,
                   const PatternLayerMap& patterns_,
                   const float sortKey_)
        : i(i_), feature(std::move(feature_)), patterns(patterns_), sortKey(sortKey_) {}

    PatternFeature(PatternFeature&& pt) noexcept
        : i(pt.i), feature(std::move(pt.feature)), patterns(pt.patterns), sortKey(pt.sortKey) {}

    PatternFeature& operator=(PatternFeature&& pt) noexcept {
        i = pt.i;
        feature = std::move(pt.feature);
        patterns = pt.patterns;
        sortKey = pt.sortKey;
        return *this;
    }

    friend bool operator<(const PatternFeature& lhs, const PatternFeature& rhs) { return lhs.sortKey < rhs.sortKey; }

    IndexType i;
    std::unique_ptr<GeometryTileFeature> feature;
    PatternLayerMap patterns;
    float sortKey;
};

template <class BucketType,
          class LayerPropertiesType,
          class PatternPropertyType,
          class LayoutPropertiesType = typename style::Properties<>>
class PatternLayout : public Layout {
public:
    PatternLayout(const BucketParameters& parameters,
                  const std::vector<Immutable<style::LayerProperties>>& group,
                  std::unique_ptr<GeometryTileLayer> sourceLayer_,
                  const LayoutParameters& layoutParameters)
        : sourceLayer(std::move(sourceLayer_)),
          zoom(parameters.tileID.overscaledZ),
          overscaling(parameters.tileID.overscaleFactor()),
          hasPattern(false) {
        assert(!group.empty());
        auto leaderLayerProperties = staticImmutableCast<LayerPropertiesType>(group.front());
        unevaluatedLayout = leaderLayerProperties->layerImpl().layout;
        layout = unevaluatedLayout.evaluate(PropertyEvaluationParameters(zoom));
        sourceLayerID = leaderLayerProperties->layerImpl().sourceLayer;
        bucketLeaderID = leaderLayerProperties->layerImpl().id;

        for (const auto& layerProperties : group) {
            const std::string& layerId = layerProperties->baseImpl->id;
            const auto& evaluated = style::getEvaluated<LayerPropertiesType>(layerProperties);
            const auto patternProperty = evaluated.template get<PatternPropertyType>();
            const auto constantPattern = patternProperty.constantOr(Faded<style::expression::Image>{"", ""});
            // determine if layer group has any layers that use *-pattern property and add
            // constant pattern dependencies.
            if (!patternProperty.isConstant()) {
                hasPattern = true;
            } else if (!constantPattern.to.id().empty()) {
                hasPattern = true;
                layoutParameters.imageDependencies.emplace(constantPattern.to.id(), ImageType::Pattern);
                layoutParameters.imageDependencies.emplace(constantPattern.from.id(), ImageType::Pattern);
            }
            layerPropertiesMap.emplace(layerId, layerProperties);
        }

        const size_t featureCount = sourceLayer->featureCount();
        for (size_t i = 0; i < featureCount; ++i) {
            auto feature = sourceLayer->getFeature(i);
            if (!leaderLayerProperties->layerImpl().filter(style::expression::EvaluationContext { this->zoom, feature.get() }))
                continue;

            PatternLayerMap patternDependencyMap;
            if (hasPattern) {
                for (const auto& layerProperties : group) {
                    const std::string& layerId = layerProperties->baseImpl->id;
                    const auto it = layerPropertiesMap.find(layerId);
                    if (it != layerPropertiesMap.end()) {
                        const auto paint = static_cast<const LayerPropertiesType&>(*it->second).evaluated;
                        const auto patternProperty = paint.template get<PatternPropertyType>();
                        if (!patternProperty.isConstant()) {
                            // For layers with non-data-constant pattern properties, evaluate their expression and add
                            // the patterns to the dependency vector
                            const auto min = patternProperty.evaluate(*feature,
                                                                      zoom - 1,
                                                                      layoutParameters.availableImages,
                                                                      PatternPropertyType::defaultValue());
                            const auto mid = patternProperty.evaluate(
                                *feature, zoom, layoutParameters.availableImages, PatternPropertyType::defaultValue());
                            const auto max = patternProperty.evaluate(*feature,
                                                                      zoom + 1,
                                                                      layoutParameters.availableImages,
                                                                      PatternPropertyType::defaultValue());

                            layoutParameters.imageDependencies.emplace(min.to.id(), ImageType::Pattern);
                            layoutParameters.imageDependencies.emplace(mid.to.id(), ImageType::Pattern);
                            layoutParameters.imageDependencies.emplace(max.to.id(), ImageType::Pattern);
                            patternDependencyMap.emplace(layerId,
                                                         PatternDependency{min.to.id(), mid.to.id(), max.to.id()});
                        }
                    }
                }
            }

            auto sortKey = evaluateSortKey(*feature);

            PatternFeature patternFeature{i, std::move(feature), patternDependencyMap, sortKey};
            const auto sortPosition = std::lower_bound(features.cbegin(), features.cend(), patternFeature);
            features.insert(sortPosition, std::move(patternFeature));
        }
    };

    bool hasDependencies() const override {
        return hasPattern;
    }

    void createBucket(const ImagePositions& patternPositions, std::unique_ptr<FeatureIndex>& featureIndex, std::unordered_map<std::string, LayerRenderData>& renderData, const bool, const bool) override {
        auto bucket = std::make_shared<BucketType>(layout, layerPropertiesMap, zoom, overscaling);
        for (auto & patternFeature : features) {
            const auto i = patternFeature.i;
            std::unique_ptr<GeometryTileFeature> feature = std::move(patternFeature.feature);
            const PatternLayerMap& patterns = patternFeature.patterns;
            const GeometryCollection& geometries = feature->getGeometries();

            bucket->addFeature(*feature, geometries, patternPositions, patterns, i);
            featureIndex->insert(geometries, i, sourceLayerID, bucketLeaderID);
        }
        if (bucket->hasData()) {
            for (const auto& pair : layerPropertiesMap) {
                renderData.emplace(pair.first, LayerRenderData {bucket, pair.second});
            }
        }
    };

protected:
    virtual float evaluateSortKey(const GeometryTileFeature&) {
        return 0.0;
    }

    std::map<std::string, Immutable<style::LayerProperties>> layerPropertiesMap;
    std::string bucketLeaderID;

    const std::unique_ptr<GeometryTileLayer> sourceLayer;
    std::list<PatternFeature> features;
    typename LayoutPropertiesType::Unevaluated unevaluatedLayout;
    typename LayoutPropertiesType::PossiblyEvaluated layout;

    const float zoom;
    const uint32_t overscaling;
    std::string sourceLayerID;
    bool hasPattern;
};

template <class BucketType,
          class LayerPropertiesType,
          class PatternPropertyType,
          class SortKeyPropertyType,
          class LayoutPropertiesType>
class PatternLayoutSorted : public PatternLayout<BucketType, LayerPropertiesType, PatternPropertyType, LayoutPropertiesType> {
public:
    PatternLayoutSorted(const BucketParameters& parameters_,
                  const std::vector<Immutable<style::LayerProperties>>& group_,
                  std::unique_ptr<GeometryTileLayer> sourceLayer_,
                  const LayoutParameters& layoutParameters_)
    : PatternLayout<BucketType, LayerPropertiesType, PatternPropertyType, LayoutPropertiesType>(parameters_, group_, std::move(sourceLayer_), layoutParameters_) {
    }

protected:
    virtual float evaluateSortKey(const GeometryTileFeature& sourceFeature) override {
        const auto sortKeyProperty = this->layout.template get<SortKeyPropertyType>();
        return sortKeyProperty.evaluate(sourceFeature, this->zoom, SortKeyPropertyType::defaultValue());
    }
};

} // namespace mbgl

