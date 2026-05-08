#include "WaveformComponent.h"
#include "../LookAndFeel_Radium.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace triggerfish {
namespace {

constexpr double kAutomationMaxDb = 12.0;
constexpr double kAutomationMinDb = -70.0;
constexpr double kStretchAutomationMinRatio = 0.01;
constexpr double kStretchAutomationMaxRatio = 8.0;

double automationDbToGain(double db) {
    return std::pow(10.0, db / 20.0);
}

double automationGainToDb(double gain) {
    if (gain <= 0.0) {
        return -std::numeric_limits<double>::infinity();
    }
    return 20.0 * std::log10(gain);
}

double applyDopplerCurve(double t, radium::DopplerCurveType curveType, double curveAmount) {
    const double clampedT = std::clamp(t, 0.0, 1.0);
    const double amount = std::clamp(curveAmount, 0.0, 1.0);
    switch (curveType) {
        case radium::DopplerCurveType::SCurve: {
            const double smooth = clampedT * clampedT * (3.0 - 2.0 * clampedT);
            return std::clamp(clampedT + (smooth - clampedT) * amount, 0.0, 1.0);
        }
        case radium::DopplerCurveType::Convex: {
            const double exponent = 1.0 + amount * 4.0;
            return std::clamp(std::pow(clampedT, exponent), 0.0, 1.0);
        }
        case radium::DopplerCurveType::Concave: {
            const double exponent = 1.0 + amount * 4.0;
            return std::clamp(1.0 - std::pow(1.0 - clampedT, exponent), 0.0, 1.0);
        }
        case radium::DopplerCurveType::Linear:
        default:
            return clampedT;
    }
}

std::optional<radium::DopplerSegmentShape> findDopplerShape(
    const radium::LayerWaveformOverview& overview,
    std::size_t leftPointId
) {
    const auto it = std::find_if(
        overview.doppler_segment_shapes.begin(),
        overview.doppler_segment_shapes.end(),
        [leftPointId](const auto& shape) { return shape.left_point_id == leftPointId; });
    if (it == overview.doppler_segment_shapes.end()) {
        return std::nullopt;
    }
    return *it;
}

}  // namespace

WaveformComponent::WaveformComponent() {
    setWantsKeyboardFocus(false);
}

void WaveformComponent::setWaveform(const radium::LayerWaveformOverview& overview) {
    overview_ = overview;
    hasData_ = overview.available && !overview.peaks.empty();
    if (overview_.editable_clips.empty()) {
        editSelection_.reset();
        selectedFadeHandle_.reset();
        selectedClipIndices_.clear();
    } else if (selectedFadeHandle_.has_value() &&
               selectedFadeHandle_->first >= overview_.editable_clips.size()) {
        selectedFadeHandle_.reset();
    }
    selectedClipIndices_.erase(
        std::remove_if(selectedClipIndices_.begin(), selectedClipIndices_.end(),
                       [this](std::size_t idx) { return idx >= overview_.editable_clips.size(); }),
        selectedClipIndices_.end());
    if (selectedAutomationPoint_.has_value()) {
        const auto selectedId = *selectedAutomationPoint_;
        bool stillExists = false;
        switch (automationLaneType_) {
            case AutomationLaneType::Stretch:
                stillExists = std::any_of(
                    overview_.stretch_automation_points.begin(),
                    overview_.stretch_automation_points.end(),
                    [selectedId](const auto& point) { return point.point_id == selectedId; });
                break;
            case AutomationLaneType::PanPosition:
                stillExists = std::any_of(
                    overview_.pan_position_automation_points.begin(),
                    overview_.pan_position_automation_points.end(),
                    [selectedId](const auto& point) { return point.point_id == selectedId; });
                break;
            case AutomationLaneType::PanFrontBack:
                stillExists = std::any_of(
                    overview_.pan_front_back_automation_points.begin(),
                    overview_.pan_front_back_automation_points.end(),
                    [selectedId](const auto& point) { return point.point_id == selectedId; });
                break;
            case AutomationLaneType::PanRightPosition:
                stillExists = std::any_of(
                    overview_.pan_right_position_automation_points.begin(),
                    overview_.pan_right_position_automation_points.end(),
                    [selectedId](const auto& point) { return point.point_id == selectedId; });
                break;
            case AutomationLaneType::PanRightFrontBack:
                stillExists = std::any_of(
                    overview_.pan_right_front_back_automation_points.begin(),
                    overview_.pan_right_front_back_automation_points.end(),
                    [selectedId](const auto& point) { return point.point_id == selectedId; });
                break;
            case AutomationLaneType::Doppler:
                stillExists = std::any_of(
                    overview_.doppler_automation_points.begin(),
                    overview_.doppler_automation_points.end(),
                    [selectedId](const auto& point) { return point.point_id == selectedId; });
                break;
            case AutomationLaneType::Volume:
                stillExists = std::any_of(
                    overview_.volume_automation_points.begin(),
                    overview_.volume_automation_points.end(),
                    [selectedId](const auto& point) { return point.point_id == selectedId; });
                break;
            case AutomationLaneType::None:
            default:
                break;
        }
        if (!stillExists) {
            selectedAutomationPoint_.reset();
        }
    }
    if (selectedAutomationSegmentLeftPointId_.has_value()) {
        const auto selectedId = *selectedAutomationSegmentLeftPointId_;
        const bool stillExists = std::any_of(
            overview_.doppler_automation_points.begin(),
            overview_.doppler_automation_points.end(),
            [selectedId](const auto& point) { return point.point_id == selectedId; });
        if (!stillExists) {
            selectedAutomationSegmentLeftPointId_.reset();
        }
    }
    repaint();
}

void WaveformComponent::clearWaveform() {
    hasData_ = false;
    overview_ = {};
    viewStart_ = 0.0;
    viewEnd_ = 1.0;
    playheadPos_ = -1.0;
    editSelection_.reset();
    selectedFadeHandle_.reset();
    selectedClipIndices_.clear();
    selectedAutomationPoint_.reset();
    selectedAutomationSegmentLeftPointId_.reset();
    automationFreehandStroke_.clear();
    repaint();
}

void WaveformComponent::setPlayheadPosition(double normalizedPos) {
    if (std::abs(playheadPos_ - normalizedPos) > 0.0005) {
        playheadPos_ = normalizedPos;
        repaint();
    }
}

void WaveformComponent::clearEditSelection() {
    if (editSelection_.has_value()) {
        editSelection_.reset();
        repaint();
    }
}

void WaveformComponent::clearSelectedFadeHandle() {
    if (selectedFadeHandle_.has_value()) {
        selectedFadeHandle_.reset();
        repaint();
    }
}

void WaveformComponent::clearSelectedClips() {
    if (!selectedClipIndices_.empty()) {
        selectedClipIndices_.clear();
        repaint();
    }
}

void WaveformComponent::clearSelectedAutomationSegment() {
    if (selectedAutomationSegmentLeftPointId_.has_value()) {
        selectedAutomationSegmentLeftPointId_.reset();
        repaint();
    }
}

void WaveformComponent::setAutomationLaneVisible(bool visible) {
    if (automationLaneVisible_ != visible) {
        automationLaneVisible_ = visible;
        repaint();
    }
}

void WaveformComponent::setAutomationLaneType(AutomationLaneType type) {
    if (automationLaneType_ != type) {
        automationLaneType_ = type;
        if (!automationLaneEnabled()) {
            selectedAutomationPoint_.reset();
            selectedAutomationSegmentLeftPointId_.reset();
        }
        automationFreehandStroke_.clear();
        repaint();
    }
}

void WaveformComponent::setAutomationFreehandDrawEnabled(bool enabled) {
    if (automationFreehandDrawEnabled_ != enabled) {
        automationFreehandDrawEnabled_ = enabled;
        if (!enabled) {
            automationFreehandStroke_.clear();
            if (dragMode_ == DragMode::AutomationFreehand) {
                dragMode_ = DragMode::None;
                editGestureChanged_ = false;
            }
        }
        repaint();
    }
}

void WaveformComponent::previewAutomationPointMove(std::size_t pointId, double normalizedTimeline, double value) {
    auto sortByTimeline = [](auto* points) {
        std::sort(points->begin(), points->end(), [](const auto& a, const auto& b) {
            if (a.timeline_position == b.timeline_position) {
                return a.point_id < b.point_id;
            }
            return a.timeline_position < b.timeline_position;
        });
    };

    const double clampedTimeline = std::clamp(normalizedTimeline, 0.0, 1.0);
    bool updated = false;

    switch (automationLaneType_) {
        case AutomationLaneType::Stretch: {
            const double clampedValue = std::clamp(value, kStretchAutomationMinRatio, kStretchAutomationMaxRatio);
            auto it = std::find_if(overview_.stretch_automation_points.begin(),
                                   overview_.stretch_automation_points.end(),
                                   [pointId](const auto& point) { return point.point_id == pointId; });
            if (it != overview_.stretch_automation_points.end()) {
                it->timeline_position = clampedTimeline;
                it->ratio = clampedValue;
                sortByTimeline(&overview_.stretch_automation_points);
                updated = true;
            }
            break;
        }
        case AutomationLaneType::PanPosition: {
            const double clampedValue = std::clamp(value, -1.0, 1.0);
            auto it = std::find_if(overview_.pan_position_automation_points.begin(),
                                   overview_.pan_position_automation_points.end(),
                                   [pointId](const auto& point) { return point.point_id == pointId; });
            if (it != overview_.pan_position_automation_points.end()) {
                it->timeline_position = clampedTimeline;
                it->value = clampedValue;
                sortByTimeline(&overview_.pan_position_automation_points);
                updated = true;
            }
            break;
        }
        case AutomationLaneType::PanFrontBack: {
            const double clampedValue = std::clamp(value, -1.0, 1.0);
            auto it = std::find_if(overview_.pan_front_back_automation_points.begin(),
                                   overview_.pan_front_back_automation_points.end(),
                                   [pointId](const auto& point) { return point.point_id == pointId; });
            if (it != overview_.pan_front_back_automation_points.end()) {
                it->timeline_position = clampedTimeline;
                it->value = clampedValue;
                sortByTimeline(&overview_.pan_front_back_automation_points);
                updated = true;
            }
            break;
        }
        case AutomationLaneType::PanRightPosition: {
            const double clampedValue = std::clamp(value, -1.0, 1.0);
            auto it = std::find_if(overview_.pan_right_position_automation_points.begin(),
                                   overview_.pan_right_position_automation_points.end(),
                                   [pointId](const auto& point) { return point.point_id == pointId; });
            if (it != overview_.pan_right_position_automation_points.end()) {
                it->timeline_position = clampedTimeline;
                it->value = clampedValue;
                sortByTimeline(&overview_.pan_right_position_automation_points);
                updated = true;
            }
            break;
        }
        case AutomationLaneType::PanRightFrontBack: {
            const double clampedValue = std::clamp(value, -1.0, 1.0);
            auto it = std::find_if(overview_.pan_right_front_back_automation_points.begin(),
                                   overview_.pan_right_front_back_automation_points.end(),
                                   [pointId](const auto& point) { return point.point_id == pointId; });
            if (it != overview_.pan_right_front_back_automation_points.end()) {
                it->timeline_position = clampedTimeline;
                it->value = clampedValue;
                sortByTimeline(&overview_.pan_right_front_back_automation_points);
                updated = true;
            }
            break;
        }
        case AutomationLaneType::Doppler: {
            const double clampedValue = std::clamp(value, -1.0, 1.0);
            auto it = std::find_if(overview_.doppler_automation_points.begin(),
                                   overview_.doppler_automation_points.end(),
                                   [pointId](const auto& point) { return point.point_id == pointId; });
            if (it != overview_.doppler_automation_points.end()) {
                it->timeline_position = clampedTimeline;
                it->value = clampedValue;
                sortByTimeline(&overview_.doppler_automation_points);
                updated = true;
            }
            break;
        }
        case AutomationLaneType::Volume: {
            const double clampedValue = std::clamp(value, 0.0, automationDbToGain(kAutomationMaxDb));
            auto it = std::find_if(overview_.volume_automation_points.begin(),
                                   overview_.volume_automation_points.end(),
                                   [pointId](const auto& point) { return point.point_id == pointId; });
            if (it != overview_.volume_automation_points.end()) {
                it->timeline_position = clampedTimeline;
                it->gain = clampedValue;
                sortByTimeline(&overview_.volume_automation_points);
                updated = true;
            }
            break;
        }
        case AutomationLaneType::None:
        default:
            break;
    }

    if (updated) {
        repaint();
    }
}

double WaveformComponent::xToNormalized(float x) const {
    double frac = static_cast<double>(x) / static_cast<double>(std::max(1, getWidth()));
    return viewStart_ + frac * (viewEnd_ - viewStart_);
}

float WaveformComponent::normalizedToX(double norm) const {
    double frac = (norm - viewStart_) / std::max(0.0001, viewEnd_ - viewStart_);
    return static_cast<float>(frac * getWidth());
}

double WaveformComponent::visualToSourceNormalized(double norm) const {
    const double clamped = std::clamp(norm, 0.0, 1.0);
    return overview_.reversed ? (1.0 - clamped) : clamped;
}

double WaveformComponent::sourceToVisualNormalized(double norm) const {
    const double clamped = std::clamp(norm, 0.0, 1.0);
    return overview_.reversed ? (1.0 - clamped) : clamped;
}

juce::Rectangle<float> WaveformComponent::automationLaneBounds() const {
    if (!automationLaneVisible_ || !automationLaneEnabled()) {
        return {};
    }
    return getLocalBounds().toFloat();
}

bool WaveformComponent::automationLaneEnabled() const {
    switch (automationLaneType_) {
        case AutomationLaneType::Volume:
            return overview_.volume_automation_enabled;
        case AutomationLaneType::Stretch:
            return overview_.stretch_automation_enabled;
        case AutomationLaneType::PanPosition:
            return overview_.pan_position_automation_enabled;
        case AutomationLaneType::PanFrontBack:
            return overview_.pan_front_back_automation_enabled;
        case AutomationLaneType::PanRightPosition:
            return overview_.pan_right_position_automation_enabled;
        case AutomationLaneType::PanRightFrontBack:
            return overview_.pan_right_front_back_automation_enabled;
        case AutomationLaneType::Doppler:
            return overview_.doppler_automation_enabled;
        case AutomationLaneType::None:
        default:
            return false;
    }
}

void WaveformComponent::appendAutomationFreehandSample(double sourceNorm, double value) {
    const double clampedNorm = std::clamp(sourceNorm, 0.0, 1.0);
    double clampedValue = value;
    if (automationLaneType_ == AutomationLaneType::Doppler ||
        automationLaneType_ == AutomationLaneType::PanPosition ||
        automationLaneType_ == AutomationLaneType::PanFrontBack ||
        automationLaneType_ == AutomationLaneType::PanRightPosition ||
        automationLaneType_ == AutomationLaneType::PanRightFrontBack) {
        clampedValue = std::clamp(value, -1.0, 1.0);
    }

    if (!automationFreehandStroke_.empty()) {
        const auto& last = automationFreehandStroke_.back();
        if (std::abs(last.first - clampedNorm) < 0.002 &&
            std::abs(last.second - clampedValue) < 0.02) {
            return;
        }
    }

    automationFreehandStroke_.emplace_back(clampedNorm, clampedValue);
}

float WaveformComponent::automationValueToY(double value) const {
    const auto lane = automationLaneBounds();
    if (lane.isEmpty()) {
        return 0.0f;
    }

    const float top = lane.getY();
    const float bottom = lane.getBottom() - 1.0f;

    if (automationLaneType_ == AutomationLaneType::Stretch) {
        juce::NormalisableRange<double> range(kStretchAutomationMinRatio, kStretchAutomationMaxRatio);
        range.setSkewForCentre(1.0);
        const auto clampedValue = std::clamp(value, kStretchAutomationMinRatio, kStretchAutomationMaxRatio);
        const double proportion = std::clamp(range.convertTo0to1(clampedValue), 0.0, 1.0);
        return bottom - static_cast<float>(proportion) * (bottom - top);
    } else if (automationLaneType_ == AutomationLaneType::Doppler) {
        const auto clampedValue = std::clamp(value, -1.0, 1.0);
        const double proportion = (1.0 - clampedValue) * 0.5;
        return bottom - static_cast<float>(proportion) * (bottom - top);
    } else if (automationLaneType_ == AutomationLaneType::PanPosition ||
               automationLaneType_ == AutomationLaneType::PanFrontBack ||
               automationLaneType_ == AutomationLaneType::PanRightPosition ||
               automationLaneType_ == AutomationLaneType::PanRightFrontBack) {
        const auto clampedValue = std::clamp(value, -1.0, 1.0);
        const double proportion = (clampedValue + 1.0) * 0.5;
        return bottom - static_cast<float>(proportion) * (bottom - top);
    }

    const float zeroDbY = top + lane.getHeight() * 0.35f;
    const float anchoredZeroY = std::clamp(zeroDbY, top + 10.0f, bottom - 2.0f);

    if (value <= 0.0) {
        return bottom;
    }

    const double db = std::clamp(automationGainToDb(value), kAutomationMinDb, kAutomationMaxDb);
    if (db >= 0.0) {
        const double t = 1.0 - (db / kAutomationMaxDb);
        return top + static_cast<float>(t) * (anchoredZeroY - top);
    }

    const float attenuatedBottom = bottom - 1.0f;
    const double t = std::clamp((-db) / std::abs(kAutomationMinDb), 0.0, 1.0);
    return anchoredZeroY + static_cast<float>(t) * (attenuatedBottom - anchoredZeroY);
}

double WaveformComponent::automationYToValue(float y) const {
    const auto lane = automationLaneBounds();
    if (lane.isEmpty() || lane.getHeight() <= 0.0f) {
        return automationLaneType_ == AutomationLaneType::Doppler ? 0.0 : 1.0;
    }

    const float top = lane.getY();
    const float bottom = lane.getBottom() - 1.0f;

    if (automationLaneType_ == AutomationLaneType::Stretch) {
        juce::NormalisableRange<double> range(kStretchAutomationMinRatio, kStretchAutomationMaxRatio);
        range.setSkewForCentre(1.0);
        const float clampedY = std::clamp(y, top, bottom);
        const double proportion = std::clamp(static_cast<double>(bottom - clampedY) /
                                                 std::max(1.0f, bottom - top),
                                             0.0, 1.0);
        return range.convertFrom0to1(proportion);
    } else if (automationLaneType_ == AutomationLaneType::Doppler) {
        const float clampedY = std::clamp(y, top, bottom);
        const double proportion = std::clamp(static_cast<double>(bottom - clampedY) /
                                                 std::max(1.0f, bottom - top),
                                             0.0, 1.0);
        return 1.0 - (proportion * 2.0);
    } else if (automationLaneType_ == AutomationLaneType::PanPosition ||
               automationLaneType_ == AutomationLaneType::PanFrontBack ||
               automationLaneType_ == AutomationLaneType::PanRightPosition ||
               automationLaneType_ == AutomationLaneType::PanRightFrontBack) {
        const float clampedY = std::clamp(y, top, bottom);
        const double proportion = std::clamp(static_cast<double>(bottom - clampedY) /
                                                 std::max(1.0f, bottom - top),
                                             0.0, 1.0);
        return (proportion * 2.0) - 1.0;
    }

    const float attenuatedBottom = bottom - 1.0f;
    const float zeroDbY = top + lane.getHeight() * 0.35f;
    const float anchoredZeroY = std::clamp(zeroDbY, top + 10.0f, bottom - 2.0f);
    const float clampedY = std::clamp(y, top, bottom);

    if (clampedY >= attenuatedBottom) {
        return 0.0;
    }

    double db = 0.0;
    if (clampedY <= anchoredZeroY) {
        const double span = std::max(1.0, static_cast<double>(anchoredZeroY - top));
        const double t = std::clamp(static_cast<double>(clampedY - top) / span, 0.0, 1.0);
        db = kAutomationMaxDb * (1.0 - t);
    } else {
        const double span = std::max(1.0, static_cast<double>(attenuatedBottom - anchoredZeroY));
        const double t = std::clamp(static_cast<double>(clampedY - anchoredZeroY) / span, 0.0, 1.0);
        db = kAutomationMinDb * t;
    }

    return automationDbToGain(db);
}

double WaveformComponent::currentAutomationValueAt(double normalizedTimeline) const {
    if (!automationLaneEnabled()) {
        return automationLaneType_ == AutomationLaneType::Doppler ? 0.0 : 1.0;
    }
    const bool isStretch = automationLaneType_ == AutomationLaneType::Stretch;
    const bool isPan = automationLaneType_ == AutomationLaneType::PanPosition ||
                       automationLaneType_ == AutomationLaneType::PanFrontBack ||
                       automationLaneType_ == AutomationLaneType::PanRightPosition ||
                       automationLaneType_ == AutomationLaneType::PanRightFrontBack;
    const bool isDoppler = automationLaneType_ == AutomationLaneType::Doppler;
    const auto& volumePoints = overview_.volume_automation_points;
    const auto& stretchPoints = overview_.stretch_automation_points;
    const auto& panPoints =
        automationLaneType_ == AutomationLaneType::PanPosition ? overview_.pan_position_automation_points :
        automationLaneType_ == AutomationLaneType::PanFrontBack ? overview_.pan_front_back_automation_points :
        automationLaneType_ == AutomationLaneType::PanRightPosition ? overview_.pan_right_position_automation_points :
        overview_.pan_right_front_back_automation_points;
    const auto& dopplerPoints = overview_.doppler_automation_points;
    const auto pointCount = isStretch ? stretchPoints.size() : (isPan ? panPoints.size() : (isDoppler ? dopplerPoints.size() : volumePoints.size()));
    if (pointCount == 0) {
        return (isPan || isDoppler) ? 0.0 : 1.0;
    }
    if (pointCount == 1) {
        return isStretch ? stretchPoints.front().ratio : (isPan ? panPoints.front().value : (isDoppler ? dopplerPoints.front().value : volumePoints.front().gain));
    }
    const double clamped = std::clamp(normalizedTimeline, 0.0, 1.0);
    if (clamped <= (isStretch ? stretchPoints.front().timeline_position : (isPan ? panPoints.front().timeline_position : (isDoppler ? dopplerPoints.front().timeline_position : volumePoints.front().timeline_position)))) {
        return isStretch ? stretchPoints.front().ratio : (isPan ? panPoints.front().value : (isDoppler ? dopplerPoints.front().value : volumePoints.front().gain));
    }
    if (clamped >= (isStretch ? stretchPoints.back().timeline_position : (isPan ? panPoints.back().timeline_position : (isDoppler ? dopplerPoints.back().timeline_position : volumePoints.back().timeline_position)))) {
        return isStretch ? stretchPoints.back().ratio : (isPan ? panPoints.back().value : (isDoppler ? dopplerPoints.back().value : volumePoints.back().gain));
    }
    for (std::size_t i = 0; i + 1 < pointCount; ++i) {
        const double leftPos = isStretch ? stretchPoints[i].timeline_position : (isPan ? panPoints[i].timeline_position : (isDoppler ? dopplerPoints[i].timeline_position : volumePoints[i].timeline_position));
        const double rightPos = isStretch ? stretchPoints[i + 1].timeline_position : (isPan ? panPoints[i + 1].timeline_position : (isDoppler ? dopplerPoints[i + 1].timeline_position : volumePoints[i + 1].timeline_position));
        if (clamped >= leftPos && clamped <= rightPos) {
            const double span = std::max(0.000001, rightPos - leftPos);
            double t = std::clamp((clamped - leftPos) / span, 0.0, 1.0);
            if (isDoppler) {
                if (const auto shape = findDopplerShape(overview_, dopplerPoints[i].point_id); shape.has_value()) {
                    t = applyDopplerCurve(t, shape->curve_type, shape->curve_amount);
                }
            }
            const double leftValue = isStretch ? stretchPoints[i].ratio : (isPan ? panPoints[i].value : (isDoppler ? dopplerPoints[i].value : volumePoints[i].gain));
            const double rightValue = isStretch ? stretchPoints[i + 1].ratio : (isPan ? panPoints[i + 1].value : (isDoppler ? dopplerPoints[i + 1].value : volumePoints[i + 1].gain));
            return leftValue + (rightValue - leftValue) * t;
        }
    }
    return (isPan || isDoppler) ? 0.0 : 1.0;
}

int WaveformComponent::findAutomationPoint(float x, float y) const {
    const auto lane = automationLaneBounds();
    if (lane.isEmpty() || !lane.expanded(0.0f, 4.0f).contains(x, y)) {
        return -1;
    }

    constexpr float kSnap = 7.0f;
    const bool isStretch = automationLaneType_ == AutomationLaneType::Stretch;
    const bool isPan = automationLaneType_ == AutomationLaneType::PanPosition ||
                       automationLaneType_ == AutomationLaneType::PanFrontBack ||
                       automationLaneType_ == AutomationLaneType::PanRightPosition ||
                       automationLaneType_ == AutomationLaneType::PanRightFrontBack;
    const bool isDoppler = automationLaneType_ == AutomationLaneType::Doppler;
    const auto& activePanPoints =
        automationLaneType_ == AutomationLaneType::PanPosition ? overview_.pan_position_automation_points :
        automationLaneType_ == AutomationLaneType::PanFrontBack ? overview_.pan_front_back_automation_points :
        automationLaneType_ == AutomationLaneType::PanRightPosition ? overview_.pan_right_position_automation_points :
        overview_.pan_right_front_back_automation_points;
    const auto& dopplerPoints = overview_.doppler_automation_points;
    const auto pointCount = isStretch
        ? static_cast<int>(overview_.stretch_automation_points.size())
        : (isPan
            ? static_cast<int>(activePanPoints.size())
            : (isDoppler
                ? static_cast<int>(dopplerPoints.size())
                : static_cast<int>(overview_.volume_automation_points.size())));
    int bestPointId = -1;
    float bestDistanceSquared = std::numeric_limits<float>::max();
    for (int i = pointCount - 1; i >= 0; --i) {
        const auto pointId = isStretch
            ? overview_.stretch_automation_points[static_cast<std::size_t>(i)].point_id
            : (isPan
                ? activePanPoints[static_cast<std::size_t>(i)].point_id
                : (isDoppler
                    ? dopplerPoints[static_cast<std::size_t>(i)].point_id
                    : overview_.volume_automation_points[static_cast<std::size_t>(i)].point_id));
        const float px = normalizedToX(
            isStretch
                ? overview_.stretch_automation_points[static_cast<std::size_t>(i)].timeline_position
                : (isPan
                    ? activePanPoints[static_cast<std::size_t>(i)].timeline_position
                    : (isDoppler
                        ? dopplerPoints[static_cast<std::size_t>(i)].timeline_position
                        : overview_.volume_automation_points[static_cast<std::size_t>(i)].timeline_position)));
        const float py = automationValueToY(
            isStretch
                ? overview_.stretch_automation_points[static_cast<std::size_t>(i)].ratio
                : (isPan
                    ? activePanPoints[static_cast<std::size_t>(i)].value
                    : (isDoppler
                        ? dopplerPoints[static_cast<std::size_t>(i)].value
                        : overview_.volume_automation_points[static_cast<std::size_t>(i)].gain)));
        const float dx = px - x;
        const float dy = py - y;
        if (std::abs(dx) <= kSnap && std::abs(dy) <= kSnap) {
            const float distanceSquared = dx * dx + dy * dy;
            if (distanceSquared <= bestDistanceSquared) {
                bestDistanceSquared = distanceSquared;
                bestPointId = static_cast<int>(pointId);
            }
        }
    }
    return bestPointId;
}

int WaveformComponent::findAutomationSegment(float x, float y) const {
    if (automationLaneType_ != AutomationLaneType::Doppler) {
        return -1;
    }
    const auto lane = automationLaneBounds();
    if (lane.isEmpty() || !lane.expanded(0.0f, 6.0f).contains(x, y)) {
        return -1;
    }
    const auto& points = overview_.doppler_automation_points;
    if (points.size() < 2) {
        return -1;
    }

    constexpr float kSegmentSnap = 8.0f;
    int bestSegmentId = -1;
    float bestDistanceSquared = std::numeric_limits<float>::max();

    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const auto& left = points[i];
        const auto& right = points[i + 1];
        if (right.timeline_position < viewStart_ || left.timeline_position > viewEnd_) {
            continue;
        }

        float prevX = normalizedToX(left.timeline_position);
        float prevY = automationValueToY(left.value);
        constexpr int kSamples = 24;
        for (int sampleIndex = 1; sampleIndex <= kSamples; ++sampleIndex) {
            const double baseT = static_cast<double>(sampleIndex) / static_cast<double>(kSamples);
            double shapedT = baseT;
            if (const auto shape = findDopplerShape(overview_, left.point_id); shape.has_value()) {
                shapedT = applyDopplerCurve(baseT, shape->curve_type, shape->curve_amount);
            }
            const double norm = left.timeline_position + (right.timeline_position - left.timeline_position) * baseT;
            const double value = left.value + (right.value - left.value) * shapedT;
            const float nextX = normalizedToX(norm);
            const float nextY = automationValueToY(value);

            const float segDx = nextX - prevX;
            const float segDy = nextY - prevY;
            const float segLenSq = segDx * segDx + segDy * segDy;
            float distanceSquared = std::numeric_limits<float>::max();
            if (segLenSq > 0.0f) {
                const float t = std::clamp(((x - prevX) * segDx + (y - prevY) * segDy) / segLenSq, 0.0f, 1.0f);
                const float projX = prevX + segDx * t;
                const float projY = prevY + segDy * t;
                const float dx = x - projX;
                const float dy = y - projY;
                distanceSquared = dx * dx + dy * dy;
            }

            if (distanceSquared <= kSegmentSnap * kSegmentSnap &&
                distanceSquared < bestDistanceSquared) {
                bestDistanceSquared = distanceSquared;
                bestSegmentId = static_cast<int>(left.point_id);
            }

            prevX = nextX;
            prevY = nextY;
        }
    }

    return bestSegmentId;
}

int WaveformComponent::findRegionHandle(float x) const {
    constexpr float kHandleSnap = 6.0f;
    for (int i = 0; i < static_cast<int>(overview_.authored_regions.size()); ++i) {
        float sx = normalizedToX(overview_.authored_regions[i].start);
        float ex = normalizedToX(overview_.authored_regions[i].end);
        if (std::abs(x - sx) < kHandleSnap) {
            return i * 2;
        }
        if (std::abs(x - ex) < kHandleSnap) {
            return i * 2 + 1;
        }
    }
    return -1;
}

int WaveformComponent::findEditableClipBody(double norm) const {
    for (int i = static_cast<int>(overview_.editable_clips.size()) - 1; i >= 0; --i) {
        const auto& clip = overview_.editable_clips[static_cast<std::size_t>(i)];
        if (norm >= clip.start && norm <= clip.end) {
            return i;
        }
    }
    return -1;
}

int WaveformComponent::findEditableClipEdge(float x, bool& isLeftEdge) const {
    constexpr float kHandleSnap = 6.0f;
    for (int i = static_cast<int>(overview_.editable_clips.size()) - 1; i >= 0; --i) {
        const auto& clip = overview_.editable_clips[static_cast<std::size_t>(i)];
        const float sx = normalizedToX(clip.start);
        const float ex = normalizedToX(clip.end);
        if (std::abs(x - sx) < kHandleSnap) {
            isLeftEdge = true;
            return i;
        }
        if (std::abs(x - ex) < kHandleSnap) {
            isLeftEdge = false;
            return i;
        }
    }
    return -1;
}

int WaveformComponent::findEditableFadeHandle(float x, bool& isFadeIn) const {
    constexpr float kHandleSnap = 6.0f;
    for (int i = static_cast<int>(overview_.editable_clips.size()) - 1; i >= 0; --i) {
        const auto& clip = overview_.editable_clips[static_cast<std::size_t>(i)];
        if (clip.fade_in_end > clip.start + 0.001) {
            const float fx = normalizedToX(clip.fade_in_end);
            if (std::abs(x - fx) < kHandleSnap) {
                isFadeIn = true;
                return i;
            }
        }
        if (clip.fade_out_start < clip.end - 0.001) {
            const float fx = normalizedToX(clip.fade_out_start);
            if (std::abs(x - fx) < kHandleSnap) {
                isFadeIn = false;
                return i;
            }
        }
    }
    return -1;
}

bool WaveformComponent::canStartEditSelection(double norm) const {
    if (overview_.editable_clips.size() < 2) {
        return false;
    }
    for (std::size_t i = 0; i + 1 < overview_.editable_clips.size(); ++i) {
        const auto& left = overview_.editable_clips[i];
        const auto& right = overview_.editable_clips[i + 1];
        const double minBoundary = std::min(left.end, right.start);
        const double maxBoundary = std::max(left.end, right.start);
        if (norm >= minBoundary && norm <= maxBoundary) {
            return true;
        }
    }
    return false;
}

bool WaveformComponent::isClipSelected(std::size_t clipIndex) const {
    return std::find(selectedClipIndices_.begin(), selectedClipIndices_.end(), clipIndex) !=
           selectedClipIndices_.end();
}

namespace {

// Interpolate between two adjacent values in a peak array
float lerpPeak(const std::vector<float>& data, double fractionalIndex) {
    if (data.empty()) return 0.0f;
    const auto maxIdx = static_cast<double>(data.size() - 1);
    const double ci = std::clamp(fractionalIndex, 0.0, maxIdx);
    const auto i0 = static_cast<std::size_t>(ci);
    const auto i1 = std::min(i0 + 1, data.size() - 1);
    const float f = static_cast<float>(ci - static_cast<double>(i0));
    return data[i0] + (data[i1] - data[i0]) * f;
}

// Build a filled path from actual min/max envelope data (Pro Tools style).
// peaksMax = max sample per bucket, peaksMin = min sample per bucket.
// The path traces the max edge left-to-right, then the min edge right-to-left.
juce::Path buildEnvelopePath(const std::vector<float>& peaksMax,
                              const std::vector<float>& peaksMin,
                              int width, float top, float height,
                              double viewStart, double viewEnd) {
    juce::Path path;
    if (peaksMax.empty() || width <= 0) return path;

    const auto n = static_cast<double>(peaksMax.size() - 1);
    const float cy = top + height * 0.5f;
    const float amp = height * 0.48f; // slight padding from edges
    const bool hasMin = peaksMin.size() == peaksMax.size();

    // Top edge (max values) left-to-right
    bool started = false;
    for (int x = 0; x <= width; ++x) {
        double norm = viewStart + (static_cast<double>(x) / static_cast<double>(width))
                      * (viewEnd - viewStart);
        if (norm < 0.0 || norm > 1.0) continue;
        float maxVal = lerpPeak(peaksMax, norm * n);
        // Map sample value [-1,1] to pixel y: +1 = top, -1 = bottom
        float y = cy - maxVal * amp;
        if (!started) { path.startNewSubPath(static_cast<float>(x), y); started = true; }
        else { path.lineTo(static_cast<float>(x), y); }
    }
    // Bottom edge (min values) right-to-left
    for (int x = width; x >= 0; --x) {
        double norm = viewStart + (static_cast<double>(x) / static_cast<double>(width))
                      * (viewEnd - viewStart);
        if (norm < 0.0 || norm > 1.0) continue;
        float minVal = hasMin ? lerpPeak(peaksMin, norm * n) : -lerpPeak(peaksMax, norm * n);
        float y = cy - minVal * amp;
        path.lineTo(static_cast<float>(x), y);
    }
    path.closeSubPath();
    return path;
}

} // namespace

void WaveformComponent::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();
    g.setColour(colours::waveBg);
    g.fillRect(bounds);

    if (!hasData_) {
        g.setColour(colours::textDim);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText("Select a layer to view its waveform", bounds, juce::Justification::centred);
        return;
    }

    const int w = getWidth();
    const int h = getHeight();
    const bool stereo = !overview_.peaks_right.empty();
    const float halfH = stereo ? h * 0.5f : static_cast<float>(h);
    const auto automationLane = automationLaneBounds();

    // Draw loop region fill
    if (overview_.loop_start.has_value() && overview_.loop_end.has_value()) {
        float lx = normalizedToX(*overview_.loop_start);
        float rx = normalizedToX(*overview_.loop_end);
        g.setColour(colours::loopFillFocus);
        g.fillRect(lx, 0.0f, rx - lx, static_cast<float>(h));
    }

    if (editSelection_.has_value()) {
        const float sx = normalizedToX(editSelection_->first);
        const float ex = normalizedToX(editSelection_->second);
        g.setColour(colours::textPrimary.withAlpha(0.08f));
        g.fillRect(std::min(sx, ex), 0.0f, std::abs(ex - sx), static_cast<float>(h));
        g.setColour(colours::textPrimary.withAlpha(0.28f));
        g.drawRect(juce::Rectangle<float>(std::min(sx, ex), 0.0f, std::abs(ex - sx), static_cast<float>(h)), 1.0f);
    }

    for (const auto& clip : overview_.editable_clips) {
        const float clipStart = normalizedToX(clip.start);
        const float clipEnd = normalizedToX(clip.end);
        const float left = std::min(clipStart, clipEnd);
        const float width = std::abs(clipEnd - clipStart);
        if (width <= 1.0f) {
            continue;
        }

        const bool clipSelected = isClipSelected(clip.clip_index);

        g.setColour((clipSelected ? juce::Colours::white : colours::accentFocus).withAlpha(clipSelected ? 0.12f : 0.08f));
        g.fillRect(left, 0.0f, width, static_cast<float>(h));
        g.setColour((clipSelected ? juce::Colours::white : colours::accentFocus).withAlpha(clipSelected ? 0.95f : 0.6f));
        g.drawRect(juce::Rectangle<float>(left, 0.0f, width, static_cast<float>(h)), clipSelected ? 3.0f : 2.0f);

        const float fadeInX = normalizedToX(clip.fade_in_end);
        if (std::abs(fadeInX - clipStart) > 1.0f) {
            if (selectedFadeHandle_.has_value() &&
                selectedFadeHandle_->first == clip.clip_index &&
                selectedFadeHandle_->second) {
                g.setColour(juce::Colours::white.withAlpha(0.9f));
                g.drawLine(clipStart, static_cast<float>(h), fadeInX, 0.0f, 3.0f);
                g.setColour(colours::accentFocus.withAlpha(0.8f));
            }
            g.drawLine(clipStart, static_cast<float>(h), fadeInX, 0.0f, 2.0f);
        }
        const float fadeOutX = normalizedToX(clip.fade_out_start);
        if (std::abs(fadeOutX - clipEnd) > 1.0f) {
            if (selectedFadeHandle_.has_value() &&
                selectedFadeHandle_->first == clip.clip_index &&
                !selectedFadeHandle_->second) {
                g.setColour(juce::Colours::white.withAlpha(0.9f));
                g.drawLine(fadeOutX, 0.0f, clipEnd, static_cast<float>(h), 3.0f);
                g.setColour(colours::accentFocus.withAlpha(0.8f));
            }
            g.drawLine(fadeOutX, 0.0f, clipEnd, static_cast<float>(h), 2.0f);
        }
    }

    // Draw trigger regions
    for (const auto& region : overview_.authored_regions) {
        float lx = normalizedToX(region.start);
        float rx = normalizedToX(region.end);
        g.setColour(colours::regionFill.withAlpha(0.5f));
        g.fillRect(lx, 0.0f, rx - lx, static_cast<float>(h));
        g.setColour(colours::regionHandle);
        g.fillRect(lx - 1.0f, 0.0f, 3.0f, static_cast<float>(h));
        g.fillRect(rx - 1.0f, 0.0f, 3.0f, static_cast<float>(h));
    }

    // --- Waveform: true min/max envelope ---
    const juce::Colour waveColour = colours::waveformFocus;

    // Left / mono channel
    {
        float top = 0.0f;
        auto path = buildEnvelopePath(overview_.peaks, overview_.peaks_min,
                                       w, top, halfH, viewStart_, viewEnd_);
        // Gradient fill
        g.setGradientFill(juce::ColourGradient(
            waveColour.withAlpha(0.45f), 0.0f, top,
            waveColour.withAlpha(0.15f), 0.0f, top + halfH, false));
        g.fillPath(path);
        // Edge
        g.setColour(waveColour.withAlpha(0.85f));
        g.strokePath(path, juce::PathStrokeType(0.8f));
        // Center line
        g.setColour(colours::border.withAlpha(0.35f));
        g.drawHorizontalLine(static_cast<int>(top + halfH * 0.5f), 0.0f, static_cast<float>(w));
    }

    // Right channel
    if (stereo) {
        float top = halfH;
        auto path = buildEnvelopePath(overview_.peaks_right, overview_.peaks_right_min,
                                       w, top, halfH, viewStart_, viewEnd_);
        g.setGradientFill(juce::ColourGradient(
            waveColour.withAlpha(0.38f), 0.0f, top,
            waveColour.withAlpha(0.12f), 0.0f, top + halfH, false));
        g.fillPath(path);
        g.setColour(waveColour.withAlpha(0.7f));
        g.strokePath(path, juce::PathStrokeType(0.8f));
        g.setColour(colours::border.withAlpha(0.35f));
        g.drawHorizontalLine(static_cast<int>(top + halfH * 0.5f), 0.0f, static_cast<float>(w));
        // Stereo divider
        g.setColour(colours::border);
        g.drawHorizontalLine(static_cast<int>(halfH), 0.0f, static_cast<float>(w));
    }

    // Audition start marker
    float audX = normalizedToX(overview_.audition_start);
    g.setColour(colours::accentFocus);
    g.drawVerticalLine(static_cast<int>(audX), 0.0f, static_cast<float>(h));
    {
        juce::Path tri;
        tri.addTriangle(audX - 4.0f, 0.0f, audX + 4.0f, 0.0f, audX, 6.0f);
        g.fillPath(tri);
    }

    // Playhead
    if (playheadPos_ >= 0.0 && playheadPos_ <= 1.0) {
        float phX = normalizedToX(playheadPos_);
        g.setColour(juce::Colours::white.withAlpha(0.25f));
        g.drawVerticalLine(static_cast<int>(phX) - 1, 0.0f, static_cast<float>(h));
        g.drawVerticalLine(static_cast<int>(phX) + 1, 0.0f, static_cast<float>(h));
        g.setColour(juce::Colours::white);
        g.drawVerticalLine(static_cast<int>(phX), 0.0f, static_cast<float>(h));
    }

    if (!automationLane.isEmpty()) {
        juce::Path automationPath;
        bool started = false;
        const auto appendPoint = [&](double norm, double value) {
            const float x = normalizedToX(norm);
            const float y = automationValueToY(value);
            if (!started) {
                automationPath.startNewSubPath(x, y);
                started = true;
            } else {
                automationPath.lineTo(x, y);
            }
        };

        const bool isStretch = automationLaneType_ == AutomationLaneType::Stretch;
        const bool isPan = automationLaneType_ == AutomationLaneType::PanPosition ||
                           automationLaneType_ == AutomationLaneType::PanFrontBack ||
                           automationLaneType_ == AutomationLaneType::PanRightPosition ||
                           automationLaneType_ == AutomationLaneType::PanRightFrontBack;
        const bool isDoppler = automationLaneType_ == AutomationLaneType::Doppler;
        const auto& volumePoints = overview_.volume_automation_points;
        const auto& stretchPoints = overview_.stretch_automation_points;
        const auto& panPoints =
            automationLaneType_ == AutomationLaneType::PanPosition ? overview_.pan_position_automation_points :
            automationLaneType_ == AutomationLaneType::PanFrontBack ? overview_.pan_front_back_automation_points :
            automationLaneType_ == AutomationLaneType::PanRightPosition ? overview_.pan_right_position_automation_points :
            overview_.pan_right_front_back_automation_points;
        const auto& dopplerPoints = overview_.doppler_automation_points;
        const auto pointCount = isStretch ? stretchPoints.size() : (isPan ? panPoints.size() : (isDoppler ? dopplerPoints.size() : volumePoints.size()));
        if (pointCount == 0) {
            const double defaultValue = (isPan || isDoppler) ? 0.0 : 1.0;
            appendPoint(viewStart_, defaultValue);
            appendPoint(viewEnd_, defaultValue);
        } else if (pointCount == 1) {
            const double singleValue = isStretch ? stretchPoints.front().ratio :
                                       (isPan ? panPoints.front().value : (isDoppler ? dopplerPoints.front().value : volumePoints.front().gain));
            appendPoint(viewStart_, singleValue);
            appendPoint(viewEnd_, singleValue);
        } else if (isDoppler) {
            constexpr int kCurveSamplesPerSegment = 24;
            appendPoint(viewStart_, currentAutomationValueAt(viewStart_));
            for (std::size_t i = 0; i + 1 < dopplerPoints.size(); ++i) {
                const auto& left = dopplerPoints[i];
                const auto& right = dopplerPoints[i + 1];
                if (right.timeline_position < viewStart_ || left.timeline_position > viewEnd_) {
                    continue;
                }
                for (int sampleIndex = 0; sampleIndex <= kCurveSamplesPerSegment; ++sampleIndex) {
                    const double baseT = static_cast<double>(sampleIndex) /
                                         static_cast<double>(kCurveSamplesPerSegment);
                    const double norm = left.timeline_position +
                                        (right.timeline_position - left.timeline_position) * baseT;
                    if (norm < viewStart_ || norm > viewEnd_) {
                        continue;
                    }
                    appendPoint(norm, currentAutomationValueAt(norm));
                }
            }
            appendPoint(viewEnd_, currentAutomationValueAt(viewEnd_));
        } else {
            appendPoint(viewStart_, currentAutomationValueAt(viewStart_));
            if (isStretch) {
                for (const auto& point : stretchPoints) {
                    if (point.timeline_position >= viewStart_ && point.timeline_position <= viewEnd_) {
                        appendPoint(point.timeline_position, point.ratio);
                    }
                }
            } else if (isPan) {
                for (const auto& point : panPoints) {
                    if (point.timeline_position >= viewStart_ && point.timeline_position <= viewEnd_) {
                        appendPoint(point.timeline_position, point.value);
                    }
                }
            } else {
                for (const auto& point : volumePoints) {
                    if (point.timeline_position >= viewStart_ && point.timeline_position <= viewEnd_) {
                        appendPoint(point.timeline_position, point.gain);
                    }
                }
            }
            appendPoint(viewEnd_, currentAutomationValueAt(viewEnd_));
        }

        g.setColour(colours::accentFocus.withAlpha(0.95f));
        g.strokePath(automationPath, juce::PathStrokeType(2.0f));

        if (isStretch) {
            for (const auto& point : stretchPoints) {
                if (point.timeline_position < viewStart_ || point.timeline_position > viewEnd_) {
                    continue;
                }
                const float px = normalizedToX(point.timeline_position);
                const float py = automationValueToY(point.ratio);
                const bool selected = selectedAutomationPoint_.has_value() &&
                                      *selectedAutomationPoint_ == point.point_id;
                g.setColour(selected ? juce::Colours::white : colours::accentFocus);
                g.fillEllipse(px - (selected ? 5.0f : 4.0f), py - (selected ? 5.0f : 4.0f),
                              selected ? 10.0f : 8.0f, selected ? 10.0f : 8.0f);
            }
        } else if (isPan) {
            for (const auto& point : panPoints) {
                if (point.timeline_position < viewStart_ || point.timeline_position > viewEnd_) {
                    continue;
                }
                const float px = normalizedToX(point.timeline_position);
                const float py = automationValueToY(point.value);
                const bool selected = selectedAutomationPoint_.has_value() &&
                                      *selectedAutomationPoint_ == point.point_id;
                g.setColour(selected ? juce::Colours::white : colours::accentFocus);
                g.fillEllipse(px - (selected ? 5.0f : 4.0f), py - (selected ? 5.0f : 4.0f),
                              selected ? 10.0f : 8.0f, selected ? 10.0f : 8.0f);
            }
        } else if (isDoppler) {
            if (selectedAutomationSegmentLeftPointId_.has_value()) {
                const auto selectedId = *selectedAutomationSegmentLeftPointId_;
                for (std::size_t i = 0; i + 1 < dopplerPoints.size(); ++i) {
                    if (dopplerPoints[i].point_id != selectedId) {
                        continue;
                    }
                    juce::Path selectedSegmentPath;
                    bool startedSegment = false;
                    constexpr int kCurveSamplesPerSegment = 32;
                    for (int sampleIndex = 0; sampleIndex <= kCurveSamplesPerSegment; ++sampleIndex) {
                        const double baseT = static_cast<double>(sampleIndex) /
                                             static_cast<double>(kCurveSamplesPerSegment);
                        const double norm = dopplerPoints[i].timeline_position +
                                            (dopplerPoints[i + 1].timeline_position - dopplerPoints[i].timeline_position) *
                                                baseT;
                        const double value = currentAutomationValueAt(norm);
                        const float px = normalizedToX(norm);
                        const float py = automationValueToY(value);
                        if (!startedSegment) {
                            selectedSegmentPath.startNewSubPath(px, py);
                            startedSegment = true;
                        } else {
                            selectedSegmentPath.lineTo(px, py);
                        }
                    }
                    g.setColour(juce::Colours::white.withAlpha(0.95f));
                    g.strokePath(selectedSegmentPath, juce::PathStrokeType(3.0f));
                    break;
                }
            }
            for (const auto& point : dopplerPoints) {
                if (point.timeline_position < viewStart_ || point.timeline_position > viewEnd_) {
                    continue;
                }
                const float px = normalizedToX(point.timeline_position);
                const float py = automationValueToY(point.value);
                const bool selected = selectedAutomationPoint_.has_value() &&
                                      *selectedAutomationPoint_ == point.point_id;
                g.setColour(selected ? juce::Colours::white : colours::accentFocus);
                g.fillEllipse(px - (selected ? 5.0f : 4.0f), py - (selected ? 5.0f : 4.0f),
                              selected ? 10.0f : 8.0f, selected ? 10.0f : 8.0f);
            }
        } else {
            for (const auto& point : volumePoints) {
                if (point.timeline_position < viewStart_ || point.timeline_position > viewEnd_) {
                    continue;
                }
                const float px = normalizedToX(point.timeline_position);
                const float py = automationValueToY(point.gain);
                const bool selected = selectedAutomationPoint_.has_value() &&
                                      *selectedAutomationPoint_ == point.point_id;
                g.setColour(selected ? juce::Colours::white : colours::accentFocus);
                g.fillEllipse(px - (selected ? 5.0f : 4.0f), py - (selected ? 5.0f : 4.0f),
                              selected ? 10.0f : 8.0f, selected ? 10.0f : 8.0f);
            }
        }
    }

    if (dragMode_ == DragMode::AutomationFreehand && automationFreehandStroke_.size() >= 2) {
        juce::Path freehandPath;
        bool started = false;
        for (const auto& sample : automationFreehandStroke_) {
            const float px = normalizedToX(sourceToVisualNormalized(sample.first));
            const float py = automationValueToY(sample.second);
            if (!started) {
                freehandPath.startNewSubPath(px, py);
                started = true;
            } else {
                freehandPath.lineTo(px, py);
            }
        }
        g.setColour(juce::Colours::white.withAlpha(0.95f));
        g.strokePath(freehandPath, juce::PathStrokeType(2.5f));
    }

    // Border
    g.setColour(colours::accentFocus.withAlpha(0.4f));
    g.drawRect(bounds, 2.0f);
}

void WaveformComponent::mouseDown(const juce::MouseEvent& e) {
    if (!hasData_) return;
    editGestureChanged_ = false;
    const bool crossfadeHeld =
        crossfadeSelectionMode_ ||
        juce::KeyPress::isKeyCurrentlyDown('f') ||
        juce::KeyPress::isKeyCurrentlyDown('F');

    if (e.mods.isCtrlDown() && e.mods.isRightButtonDown()) {
        dragMode_ = DragMode::None;
        dragHandleRegion_ = -1;
        if (onRegionsCleared) {
            onRegionsCleared();
        }
        return;
    }

    if (e.mods.isRightButtonDown()) {
        dragMode_ = DragMode::CreateRegion;
        dragStartNorm_ = xToNormalized(static_cast<float>(e.x));
        return;
    }

    if (e.mods.isLeftButtonDown()) {
        const auto lane = automationLaneBounds();
        if (!lane.isEmpty() && lane.contains(e.position)) {
            if (automationLaneType_ == AutomationLaneType::Doppler && automationFreehandDrawEnabled_) {
                dragMode_ = DragMode::AutomationFreehand;
                dragStartNorm_ = std::clamp(xToNormalized(static_cast<float>(e.x)), 0.0, 1.0);
                dragStarted_ = true;
                selectedAutomationPoint_.reset();
                selectedAutomationSegmentLeftPointId_.reset();
                selectedFadeHandle_.reset();
                editSelection_.reset();
                clearSelectedClips();
                automationFreehandStroke_.clear();
                appendAutomationFreehandSample(
                    visualToSourceNormalized(dragStartNorm_),
                    automationYToValue(static_cast<float>(e.y)));
                if (onAutomationGestureBegan) {
                    onAutomationGestureBegan();
                }
                repaint();
                return;
            }

            const int automationPoint = findAutomationPoint(static_cast<float>(e.x), static_cast<float>(e.y));
            if (automationPoint >= 0) {
                if (e.mods.isAltDown()) {
                    selectedAutomationPoint_.reset();
                    if (onAutomationGestureBegan) {
                        onAutomationGestureBegan();
                    }
                    if (onAutomationPointDeleted) {
                        onAutomationPointDeleted(static_cast<std::size_t>(automationPoint));
                    }
                    repaint();
                    return;
                }
                dragMode_ = DragMode::AutomationPoint;
                dragClipIndex_ = automationPoint;
                dragStarted_ = true;
                selectedAutomationPoint_ = static_cast<std::size_t>(automationPoint);
                selectedAutomationSegmentLeftPointId_.reset();
                selectedFadeHandle_.reset();
                editSelection_.reset();
                clearSelectedClips();
                if (onAutomationGestureBegan) {
                    onAutomationGestureBegan();
                }
                repaint();
                return;
            }

            const double norm = std::clamp(xToNormalized(static_cast<float>(e.x)), 0.0, 1.0);
            const float automationLineY =
                automationValueToY(currentAutomationValueAt(norm));
            if (automationLaneType_ == AutomationLaneType::Doppler) {
                const int segmentId = findAutomationSegment(static_cast<float>(e.x), static_cast<float>(e.y));
                if (segmentId >= 0 && std::abs(automationLineY - static_cast<float>(e.y)) <= 4.0f) {
                    selectedAutomationSegmentLeftPointId_ = static_cast<std::size_t>(segmentId);
                    selectedAutomationPoint_.reset();
                    selectedFadeHandle_.reset();
                    editSelection_.reset();
                    clearSelectedClips();
                    if (onAutomationSegmentSelected) {
                        onAutomationSegmentSelected(selectedAutomationSegmentLeftPointId_);
                    }
                    repaint();
                    return;
                }
                if (selectedAutomationSegmentLeftPointId_.has_value()) {
                    selectedAutomationSegmentLeftPointId_.reset();
                    if (onAutomationSegmentSelected) {
                        onAutomationSegmentSelected(std::nullopt);
                    }
                }
            }
            if (std::abs(automationLineY - static_cast<float>(e.y)) <= 10.0f &&
                onAutomationPointCreated) {
                if (onAutomationGestureBegan) {
                    onAutomationGestureBegan();
                }
                auto createdIndex = onAutomationPointCreated(visualToSourceNormalized(norm));
                if (createdIndex.has_value()) {
                    selectedAutomationPoint_ = createdIndex;
                    selectedAutomationSegmentLeftPointId_.reset();
                    selectedFadeHandle_.reset();
                    editSelection_.reset();
                    clearSelectedClips();
                    if (onAutomationGestureFinished) {
                        onAutomationGestureFinished();
                    }
                    repaint();
                    return;
                }
            }
        }
    }

    if (e.mods.isLeftButtonDown() && selectedAutomationSegmentLeftPointId_.has_value()) {
        selectedAutomationSegmentLeftPointId_.reset();
        if (onAutomationSegmentSelected) {
            onAutomationSegmentSelected(std::nullopt);
        }
    }

    const double norm = xToNormalized(static_cast<float>(e.x));
    const int clipBody = findEditableClipBody(norm);
    bool fadeIsIn = false;
    const int fadeHandle = findEditableFadeHandle(static_cast<float>(e.x), fadeIsIn);
    bool isLeftEdge = false;
    const int clipEdge = findEditableClipEdge(static_cast<float>(e.x), isLeftEdge);
    const bool clickedSelectedClip =
        (clipBody >= 0 && isClipSelected(static_cast<std::size_t>(clipBody))) ||
        (fadeHandle >= 0 && isClipSelected(static_cast<std::size_t>(fadeHandle))) ||
        (clipEdge >= 0 && isClipSelected(static_cast<std::size_t>(clipEdge)));

    if (e.mods.isShiftDown() && e.mods.isLeftButtonDown() && clipBody >= 0) {
        const auto selectedIndex = static_cast<std::size_t>(clipBody);
        if (!isClipSelected(selectedIndex)) {
            selectedClipIndices_.push_back(selectedIndex);
        }
        selectedFadeHandle_.reset();
        editSelection_.reset();
        dragMode_ = DragMode::None;
        repaint();
        return;
    }

    if (e.mods.isLeftButtonDown() && !clickedSelectedClip && !selectedClipIndices_.empty()) {
        clearSelectedClips();
    }
    if (e.mods.isLeftButtonDown() && selectedAutomationPoint_.has_value()) {
        selectedAutomationPoint_.reset();
        repaint();
    }

    if (e.mods.isCtrlDown() && e.mods.isLeftButtonDown()) {
        int handle = findRegionHandle(static_cast<float>(e.x));
        if (handle >= 0) {
            dragMode_ = DragMode::DragHandle;
            dragHandleRegion_ = handle / 2;
            dragHandleIsEnd_ = (handle % 2) == 1;
            return;
        }
    }

    if (e.mods.isLeftButtonDown() && !overview_.editable_clips.empty()) {
        if (crossfadeHeld && overview_.editable_clips.size() >= 2) {
            dragMode_ = DragMode::EditSelection;
            dragStartNorm_ = norm;
            editSelection_ = std::make_pair(norm, norm);
            selectedFadeHandle_.reset();
            repaint();
            return;
        }

        if (fadeHandle >= 0) {
            dragMode_ = DragMode::FadeClip;
            dragClipIndex_ = fadeHandle;
            dragClipFadeIsIn_ = fadeIsIn;
            dragStarted_ = true;
            editSelection_.reset();
            selectedFadeHandle_ = std::make_pair(static_cast<std::size_t>(fadeHandle), fadeIsIn);
            if (onEditGestureBegan) {
                onEditGestureBegan();
            }
            repaint();
            return;
        }

        if (clipEdge >= 0) {
            dragMode_ = DragMode::TrimClip;
            dragClipIndex_ = clipEdge;
            dragClipIsLeftEdge_ = isLeftEdge;
            dragStarted_ = true;
            editSelection_.reset();
            selectedFadeHandle_.reset();
            if (onEditGestureBegan) {
                onEditGestureBegan();
            }
            return;
        }

        if (clipBody >= 0) {
            dragMode_ = DragMode::MoveClipCandidate;
            dragClipIndex_ = clipBody;
            dragStartNorm_ = norm;
            dragClipOriginalStart_ = overview_.editable_clips[static_cast<std::size_t>(clipBody)].start;
            dragStarted_ = false;
            editSelection_.reset();
            selectedFadeHandle_.reset();
            if (onAuditionStart) {
                onAuditionStart(visualToSourceNormalized(norm));
            }
            return;
        }

    }

    dragMode_ = DragMode::AuditionStart;
    editSelection_.reset();
    selectedFadeHandle_.reset();
    if (onAuditionStart) {
        onAuditionStart(visualToSourceNormalized(norm));
    }
}

void WaveformComponent::mouseDoubleClick(const juce::MouseEvent& e) {
    if (!hasData_ || !e.mods.isLeftButtonDown()) {
        return;
    }

    bool fadeIsIn = false;
    const int fadeHandle = findEditableFadeHandle(static_cast<float>(e.x), fadeIsIn);
    if (fadeHandle >= 0) {
        selectedFadeHandle_ = std::make_pair(static_cast<std::size_t>(fadeHandle), fadeIsIn);
    } else {
        selectedFadeHandle_.reset();
    }
    repaint();
}

void WaveformComponent::mouseDrag(const juce::MouseEvent& e) {
    if (!hasData_) return;
    double norm = std::clamp(xToNormalized(static_cast<float>(e.x)), 0.0, 1.0);

    if (dragMode_ == DragMode::DragHandle && dragHandleRegion_ >= 0) {
        auto idx = static_cast<std::size_t>(dragHandleRegion_);
        if (idx < overview_.authored_regions.size()) {
            auto region = overview_.authored_regions[idx];
            if (dragHandleIsEnd_) {
                region.end = std::max(region.start + 0.001, norm);
            } else {
                region.start = std::min(region.end - 0.001, norm);
            }
            if (onRegionUpdated) {
                const double sourceA = visualToSourceNormalized(region.start);
                const double sourceB = visualToSourceNormalized(region.end);
                onRegionUpdated(idx, std::min(sourceA, sourceB), std::max(sourceA, sourceB));
            }
        }
    } else if (dragMode_ == DragMode::MoveClipCandidate) {
        if (std::abs(norm - dragStartNorm_) > 0.003) {
            dragMode_ = DragMode::MoveClip;
            dragStarted_ = true;
            if (onEditGestureBegan) {
                onEditGestureBegan();
            }
        }
    } else if (dragMode_ == DragMode::MoveClip && dragClipIndex_ >= 0) {
        const double sourceCurrent = visualToSourceNormalized(norm);
        const double sourceStart = visualToSourceNormalized(dragStartNorm_);
        const double delta = sourceCurrent - sourceStart;
        const auto movedClipIndex = static_cast<std::size_t>(dragClipIndex_);

        if (selectedClipIndices_.size() > 1 && isClipSelected(movedClipIndex) && onEditClipsMoved) {
            onEditClipsMoved(selectedClipIndices_, delta);
            dragStartNorm_ = norm;
            editGestureChanged_ = true;
        } else if (onEditClipMoved) {
            onEditClipMoved(movedClipIndex, delta);
            dragStartNorm_ = norm;
            editGestureChanged_ = true;
        }
    } else if (dragMode_ == DragMode::TrimClip && dragClipIndex_ >= 0) {
        if (onEditClipTrimmed) {
            onEditClipTrimmed(
                static_cast<std::size_t>(dragClipIndex_),
                dragClipIsLeftEdge_,
                visualToSourceNormalized(norm));
            editGestureChanged_ = true;
        }
    } else if (dragMode_ == DragMode::FadeClip && dragClipIndex_ >= 0) {
        if (onEditClipFadeChanged) {
            onEditClipFadeChanged(
                static_cast<std::size_t>(dragClipIndex_),
                dragClipFadeIsIn_,
                visualToSourceNormalized(norm));
            editGestureChanged_ = true;
        }
    } else if (dragMode_ == DragMode::EditSelection) {
        editSelection_ = std::make_pair(std::min(dragStartNorm_, norm), std::max(dragStartNorm_, norm));
        repaint();
    } else if (dragMode_ == DragMode::AutomationFreehand) {
        appendAutomationFreehandSample(
            visualToSourceNormalized(norm),
            automationYToValue(static_cast<float>(e.y)));
        editGestureChanged_ = true;
        repaint();
    } else if (dragMode_ == DragMode::AutomationPoint && dragClipIndex_ >= 0) {
        const double sourceNorm = visualToSourceNormalized(norm);
        const double value = automationYToValue(static_cast<float>(e.y));
        if (onAutomationPointMoved) {
            onAutomationPointMoved(static_cast<std::size_t>(dragClipIndex_), sourceNorm, value);
            editGestureChanged_ = true;
        }
    }
}

void WaveformComponent::mouseUp(const juce::MouseEvent& e) {
    if (!hasData_) return;

    const auto finishedMode = dragMode_;
    const int finishedClipIndex = dragClipIndex_;
    const bool editGestureChanged = editGestureChanged_;

    if (dragMode_ == DragMode::CreateRegion) {
        double endNorm = std::clamp(xToNormalized(static_cast<float>(e.x)), 0.0, 1.0);
        double start = std::min(dragStartNorm_, endNorm);
        double end = std::max(dragStartNorm_, endNorm);
        if (end - start > 0.005 && onRegionCreated) {
            const double sourceA = visualToSourceNormalized(start);
            const double sourceB = visualToSourceNormalized(end);
            onRegionCreated(std::min(sourceA, sourceB), std::max(sourceA, sourceB));
        }
    } else if (dragMode_ == DragMode::EditSelection) {
        double endNorm = std::clamp(xToNormalized(static_cast<float>(e.x)), 0.0, 1.0);
        double start = std::min(dragStartNorm_, endNorm);
        double end = std::max(dragStartNorm_, endNorm);
        const bool crossfadeHeld =
            crossfadeSelectionMode_ ||
            juce::KeyPress::isKeyCurrentlyDown('f') ||
            juce::KeyPress::isKeyCurrentlyDown('F');
        if (crossfadeHeld && end - start > 0.005 && onCrossfadeRequested) {
            const double sourceA = visualToSourceNormalized(start);
            const double sourceB = visualToSourceNormalized(end);
            onCrossfadeRequested(std::min(sourceA, sourceB), std::max(sourceA, sourceB));
            editSelection_.reset();
        }
    } else if (dragMode_ == DragMode::AutomationFreehand) {
        appendAutomationFreehandSample(
            visualToSourceNormalized(std::clamp(xToNormalized(static_cast<float>(e.x)), 0.0, 1.0)),
            automationYToValue(static_cast<float>(e.y)));
        if (!automationFreehandStroke_.empty() && onAutomationFreehandDraw) {
            onAutomationFreehandDraw(automationFreehandStroke_);
        }
        automationFreehandStroke_.clear();
    }

    dragMode_ = DragMode::None;
    dragHandleRegion_ = -1;
    dragClipIndex_ = -1;
    dragStarted_ = false;
    editGestureChanged_ = false;

    if (editGestureChanged && finishedMode == DragMode::AutomationPoint && onAutomationGestureFinished) {
        onAutomationGestureFinished();
    } else if (editGestureChanged &&
        (finishedMode == DragMode::MoveClip || finishedMode == DragMode::TrimClip || finishedMode == DragMode::FadeClip) &&
        ((finishedMode == DragMode::MoveClip && finishedClipIndex >= 0 &&
          selectedClipIndices_.size() > 1 &&
          isClipSelected(static_cast<std::size_t>(finishedClipIndex)) &&
          onGroupedEditGestureFinished) || onEditGestureFinished)) {
        if (finishedMode == DragMode::MoveClip && finishedClipIndex >= 0 &&
            selectedClipIndices_.size() > 1 &&
            isClipSelected(static_cast<std::size_t>(finishedClipIndex)) &&
            onGroupedEditGestureFinished) {
            onGroupedEditGestureFinished(selectedClipIndices_);
        } else if (onEditGestureFinished) {
            std::optional<std::size_t> priorityClipIndex;
            if (finishedMode == DragMode::MoveClip && finishedClipIndex >= 0) {
                priorityClipIndex = static_cast<std::size_t>(finishedClipIndex);
            }
            onEditGestureFinished(priorityClipIndex);
        }
    }
}

void WaveformComponent::mouseWheelMove(const juce::MouseEvent& e,
                                        const juce::MouseWheelDetails& wheel) {
    if (!hasData_) return;
    if (!e.mods.isCtrlDown() && !e.mods.isShiftDown()) return;

    double range = viewEnd_ - viewStart_;

    if (e.mods.isShiftDown() && !e.mods.isCtrlDown()) {
        // Shift+wheel: scroll left/right (only when zoomed in)
        if (range >= 1.0) return;
        double scrollAmount = range * 0.15 * (wheel.deltaY > 0 ? -1.0 : 1.0);
        viewStart_ += scrollAmount;
        viewEnd_ += scrollAmount;
        if (viewStart_ < 0.0) { viewEnd_ -= viewStart_; viewStart_ = 0.0; }
        if (viewEnd_ > 1.0) { viewStart_ -= (viewEnd_ - 1.0); viewEnd_ = 1.0; }
    } else {
        // Ctrl+wheel: zoom in/out centered on mouse
        double mouseNorm = xToNormalized(static_cast<float>(e.x));
        double zoomFactor = wheel.deltaY > 0 ? 0.8 : 1.25;
        double newRange = std::clamp(range * zoomFactor, 0.005, 1.0);
        double mouseRatio = (mouseNorm - viewStart_) / range;
        viewStart_ = mouseNorm - mouseRatio * newRange;
        viewEnd_ = viewStart_ + newRange;
        if (viewStart_ < 0.0) { viewEnd_ -= viewStart_; viewStart_ = 0.0; }
        if (viewEnd_ > 1.0) { viewStart_ -= (viewEnd_ - 1.0); viewEnd_ = 1.0; }
    }

    viewStart_ = std::max(0.0, viewStart_);
    viewEnd_ = std::min(1.0, viewEnd_);

    if (onViewRangeChanged) {
        const double sourceA = visualToSourceNormalized(viewStart_);
        const double sourceB = visualToSourceNormalized(viewEnd_);
        onViewRangeChanged(std::min(sourceA, sourceB), std::max(sourceA, sourceB));
    }
    repaint();
}

}  // namespace triggerfish
