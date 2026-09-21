#include <gtest/gtest.h>

#include <array>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/commands/NetworkCommands.hpp"
#include "nemo/core/commands/RotoCommands.hpp"
#include "nemo/core/document/Roto.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/eval/GpuExecutor.hpp"
#include "nemo/gpu/Error.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/gpu/Submit.hpp"

using namespace nemo;

namespace {

RotoElement rectangle(RotoData& data, float x, float y, float right, float bottom, RotoElementId parent = 0,
                      RotoBlend blend = RotoBlend::Combine) {
    RotoElement shape;
    shape.id = data.nextElementId++;
    shape.name = "Rectangle" + std::to_string(shape.id);
    shape.parent = parent;
    shape.blend = blend;
    for (const Vector2Value position :
         {Vector2Value{{x, y}}, Vector2Value{{right, y}}, Vector2Value{{right, bottom}}, Vector2Value{{x, bottom}}}) {
        RotoPoint point;
        point.id = data.nextPointId++;
        point.position = position;
        shape.points.push_back(point);
    }
    return shape;
}

// Each implementation faces analytic samples, not another implementation's image.
class RotoRaster : public testing::TestWithParam<int> {
protected:
    ProjectSession session;
    NetworkId network{};
    NodeId node{};
    std::unique_ptr<gpu::Instance> instance;
    std::unique_ptr<gpu::Device> device;
    std::unique_ptr<gpu::Allocator> allocator;
    std::optional<eval::EffectLibrary> effects;
    ResultCache<CpuImage> cpuCache;
    ResultCache<eval::GpuNodeImage> gpuCache;

    void SetUp() override {
        network = session.document().rootNetworkId();
        auto id = std::make_shared<NodeId>();
        submit(transactionCommand("Roto canvas", {setNetworkFormatCommand(network, ImageFormat{16, 16, 1.0F}),
                                                  addNodeCommand(network, "roto", "Matte", id)}));
        node = *id;
        if (GetParam() != 0) {
            try {
                instance = gpu::Instance::create({.validation = true});
                device = gpu::Device::create(*instance);
                allocator = gpu::Allocator::create(*instance, *device, {.max_device_bytes = 256u << 20});
            } catch (const gpu::GpuException& error) {
                if (error.errorCode() == gpu::GpuError::NoDevice)
                    GTEST_SKIP() << error.what();
                throw;
            }
            if (GetParam() == 1)
                effects.emplace(eval::glslEffectLibrary());
            else {
#ifdef NEMO_SLANG_SPV_DIR
                effects.emplace(eval::loadSlangEffectLibrary(NEMO_SLANG_SPV_DIR, NEMO_SLANG_SRC_DIR));
#else
                GTEST_SKIP() << "Slang shaders unavailable";
#endif
            }
        }
    }

    void TearDown() override {
        if (instance) {
            for (const auto& message : instance->take_debug_messages())
                EXPECT_LT(message.severity, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) << message.text;
        }
    }

    void submit(Command command) {
        const auto result = session.submit(std::move(command), {session.revision(), {}});
        ASSERT_TRUE(result.committed) << (result.error ? result.error->message : "not committed");
    }

    CpuImage render(Region region = {0, 0, 16, 16}, int scale = 1, std::vector<std::string> channels = {}) {
        EvaluationRequest request;
        request.network = network;
        request.output = node;
        request.region = region;
        request.samplingScale = scale;
        request.channels = std::move(channels);
        if (!effects)
            return evaluateCpu(session.document(), request, &cpuCache).image;
        auto result =
            eval::evaluateGpu(session.document(), request, *effects, *device, *allocator, 10'000'000'000ULL, &gpuCache);
        return result.readBack(node, *device, *allocator);
    }
};

TEST_P(RotoRaster, GeneratorContoursAndRegionalSampling) {
    RotoData shapes;
    shapes.elements.push_back(rectangle(shapes, 2, 2, 10, 10));
    submit(setRotoDataCommand(network, node, shapes));
    const auto full = render();
    EXPECT_EQ(full.layout().channels, (std::vector<std::string>{"A"}));
    EXPECT_FLOAT_EQ(full.pixel(4, 4)[3], 1.0F);
    EXPECT_FLOAT_EQ(full.pixel(12, 4)[3], 0.0F);
    const auto region = render({4, 4, 8, 8}, 2);
    EXPECT_FLOAT_EQ(region.pixel(0, 0)[3], 1.0F);
    EXPECT_FLOAT_EQ(region.pixel(3, 0)[3], 0.0F);
}

TEST_P(RotoRaster, NestedIntersectionAndSubtractionRespectOrder) {
    RotoData shapes;
    RotoElement group;
    group.id = shapes.nextElementId++;
    group.name = "Group";
    group.kind = RotoKind::Group;
    group.translation = Vector2Value{{2, 0}};
    shapes.elements.push_back(group);
    shapes.elements.push_back(rectangle(shapes, 0, 2, 10, 12, group.id));
    shapes.elements.push_back(rectangle(shapes, 4, 0, 14, 14, group.id, RotoBlend::Intersect));
    shapes.elements.push_back(rectangle(shapes, 6, 6, 8, 8, group.id, RotoBlend::Subtract));
    submit(setRotoDataCommand(network, node, shapes));
    const auto matte = render();
    EXPECT_FLOAT_EQ(matte.pixel(4, 4)[3], 0.0F);
    EXPECT_FLOAT_EQ(matte.pixel(7, 4)[3], 1.0F);
    EXPECT_FLOAT_EQ(matte.pixel(8, 6)[3], 0.0F);
    EXPECT_FLOAT_EQ(matte.pixel(13, 6)[3], 0.0F);
}

TEST_P(RotoRaster, FeatherAndLifetimeAffectOnlyContributingShapes) {
    RotoData shapes;
    auto shape = rectangle(shapes, 2, 2, 10, 10);
    shape.feather = 4;
    shapes.elements.push_back(shape);
    submit(setRotoDataCommand(network, node, shapes));
    const auto feathered = render();
    EXPECT_NEAR(feathered.pixel(11, 6)[3], 0.625F, 0.025F);
    EXPECT_FLOAT_EQ(feathered.pixel(6, 6)[3], 1.0F);
    shape.firstFrame = 1;
    RotoData inactive;
    inactive.nextElementId = shapes.nextElementId;
    inactive.nextPointId = shapes.nextPointId;
    inactive.elements.push_back(shape);
    submit(setRotoDataCommand(network, node, inactive));
    EXPECT_FLOAT_EQ(render().pixel(6, 6)[3], 0.0F);
}

TEST_P(RotoRaster, MotionBlurSamplesCompletedHierarchyAndOneSampleDisablesIt) {
    RotoData shapes;
    RotoElement group;
    group.id = shapes.nextElementId++;
    group.name = "Moving group";
    group.kind = RotoKind::Group;
    shapes.elements.push_back(group);
    shapes.elements.push_back(rectangle(shapes, 0, 0, 4, 16, group.id));
    shapes.elements.push_back(rectangle(shapes, 1, 0, 3, 16, group.id, RotoBlend::Subtract));
    submit(setRotoDataCommand(network, node, shapes));
    const ParameterAddress address{network, node, "translation", 0, group.id, 0};
    submit(setKeyframesCommand({{address, Keyframe{.time = -1, .value = Vector2Value{{0, 0}}}},
                                {address, Keyframe{.time = 1, .value = Vector2Value{{8, 0}}}}}));
    submit(setParamCommand(network, node, "shutter", 1.0));
    EXPECT_FLOAT_EQ(render().pixel(4, 8)[3], 1.0F);
    submit(setParamCommand(network, node, "samples", std::int64_t{2}));
    EXPECT_NEAR(render().pixel(4, 8)[3], 0.0F, 0.001F);
    submit(setParamCommand(network, node, "shutter", 0.0));
    EXPECT_FLOAT_EQ(render().pixel(4, 8)[3], 1.0F);
}

TEST_P(RotoRaster, NodeOpacityAndShutterUseValuesBeyondNavigationTravel) {
    RotoData shapes;
    const auto shape = rectangle(shapes, 0, 0, 4, 16);
    shapes.elements.push_back(shape);
    submit(setRotoDataCommand(network, node, shapes));
    const ParameterAddress address{network, node, "translation", 0, shape.id, 0};
    submit(setKeyframesCommand({{address, Keyframe{.time = -1, .value = Vector2Value{{0, 0}}}},
                                {address, Keyframe{.time = 1, .value = Vector2Value{{8, 0}}}}}));
    submit(setParamCommand(network, node, "samples", std::int64_t{2}));
    submit(setParamCommand(network, node, "shutter", 2.0));
    // The midpoint samples translate the rectangle by 2 and 6 pixels.
    // Pixel 2 is covered only by the first sample; a shutter capped at 1
    // would sample translations 3 and 5 and leave it uncovered.
    EXPECT_FLOAT_EQ(render().pixel(2, 8)[3], 0.5F);
    submit(setParamCommand(network, node, "opacity", 2.0));
    EXPECT_FLOAT_EQ(render().pixel(2, 8)[3], 1.0F);
    submit(setParamCommand(network, node, "opacity", -2.0));
    const auto negative = render();
    EXPECT_FLOAT_EQ(negative.pixel(2, 8)[3], -1.0F);
    EXPECT_FLOAT_EQ(negative.pixel(12, 8)[3], 0.0F);
}

TEST_P(RotoRaster, NamedOutputPreservesBackgroundAndInputMask) {
    auto background = std::make_shared<NodeId>();
    auto mask = std::make_shared<NodeId>();
    submit(transactionCommand("Inputs", {addNodeCommand(network, "constcolor", "Background", background),
                                         addNodeCommand(network, "constcolor", "Mask", mask)}));
    submit(transactionCommand("Connect",
                              {setParamCommand(network, *background, "color", ColorValue{{0.25F, 0.5F, 0.75F, 1.0F}}),
                               setParamCommand(network, *mask, "color", ColorValue{{0, 0, 0, 0.5F}}),
                               connectCommand(network, {*background, 0}, {node, 0}),
                               connectCommand(network, {*mask, 0}, {node, 1}),
                               setParamCommand(network, node, "outputChannel", std::string{"matte.coverage"}),
                               setParamCommand(network, node, "maskChannel", std::string{"A"})}));
    RotoData shapes;
    shapes.elements.push_back(rectangle(shapes, 2, 2, 10, 10));
    submit(setRotoDataCommand(network, node, shapes));
    const auto image = render();
    EXPECT_EQ(image.pixel(4, 4), (std::array<float, 4>{0.25F, 0.5F, 0.75F, 1.0F}));
    const int channel = channelIndex(image.layout().channels, "matte.coverage");
    ASSERT_GE(channel, 0);
    EXPECT_FLOAT_EQ(image.channel(4, 4, channel), 0.5F);
    EXPECT_FLOAT_EQ(image.channel(12, 4, channel), 0.0F);
}

TEST_P(RotoRaster, NeighboringShapeKeyInvalidatesBlurWithoutChangingCurrentFrame) {
    RotoData shapes;
    const auto shape = rectangle(shapes, 0, 0, 4, 16);
    shapes.elements.push_back(shape);
    submit(setRotoDataCommand(network, node, shapes));
    const ParameterAddress address{network, node, "translation", 0, shape.id, 0};
    submit(setKeyframesCommand({{address, Keyframe{.time = -1, .value = Vector2Value{{0, 0}}}},
                                {address, Keyframe{.time = 0, .value = Vector2Value{{4, 0}}}},
                                {address, Keyframe{.time = 1, .value = Vector2Value{{8, 0}}}}}));
    submit(transactionCommand("Blur", {setParamCommand(network, node, "samples", std::int64_t{2}),
                                       setParamCommand(network, node, "shutter", 1.0)}));
    EXPECT_FLOAT_EQ(render().pixel(2, 8)[3], 0.0F);
    submit(setKeyframesCommand({{address, Keyframe{.time = -0.25, .value = Vector2Value{{0, 0}}}}}));
    EXPECT_FLOAT_EQ(render().pixel(2, 8)[3], 0.5F);
    ASSERT_TRUE(session.undo({session.revision(), {}}).committed);
    EXPECT_FLOAT_EQ(render().pixel(2, 8)[3], 0.0F);
}

TEST_P(RotoRaster, BezierTangentsAndBSplineTensionChangeTheActualContour) {
    RotoData shapes;
    auto curve = rectangle(shapes, 2, 2, 14, 14);
    curve.kind = RotoKind::BSpline;
    shapes.elements.push_back(curve);
    submit(setRotoDataCommand(network, node, shapes));
    EXPECT_FLOAT_EQ(render().pixel(8, 8)[3], 1.0F);
    EXPECT_FLOAT_EQ(render().pixel(3, 3)[3], 0.0F);
    for (std::size_t i = 0; i < curve.points.size(); ++i)
        curve.points[i].tension = 1;
    shapes.elements[0] = curve;
    submit(setRotoDataCommand(network, node, shapes));
    EXPECT_FLOAT_EQ(render().pixel(3, 3)[3], 1.0F);

    // Four cubic arcs approximate the unit circle with error below 0.03%.
    curve.kind = RotoKind::Bezier;
    constexpr float tangent = 2.209139F;
    const std::array<Vector2Value, 4> positions{{{{8, 4}}, {{12, 8}}, {{8, 12}}, {{4, 8}}}};
    const std::array<Vector2Value, 4> handles{{{{tangent, 0}}, {{0, tangent}}, {{-tangent, 0}}, {{0, -tangent}}}};
    for (std::size_t i = 0; i < curve.points.size(); ++i) {
        curve.points[i].position = positions[i];
        curve.points[i].outTangent = handles[i];
        curve.points[i].inTangent = Vector2Value{{-handles[i].value[0], -handles[i].value[1]}};
    }
    shapes.elements[0] = curve;
    submit(setRotoDataCommand(network, node, shapes));
    EXPECT_FLOAT_EQ(render().pixel(9, 9)[3], 1.0F);
    EXPECT_FLOAT_EQ(render().pixel(11, 11)[3], 0.0F);
}

class RotoNativeLifetime : public RotoRaster {};

TEST_P(RotoNativeLifetime, DroppedSampledGeometryRetiresOnlyAfterGpuCompletion) {
    RotoData shapes;
    shapes.elements.push_back(rectangle(shapes, 2, 2, 10, 10));
    submit(setRotoDataCommand(network, node, shapes));
    submit(setParamCommand(network, node, "samples", std::int64_t{4}));
    auto& queue = device->submissions(device->graphics_family());
    struct Gate {
        VkDevice device{};
        VkSemaphore semaphore{};
        ~Gate() {
            if (semaphore)
                vkDestroySemaphore(device, semaphore, nullptr);
        }
    };
    auto gate = std::make_shared<Gate>();
    gate->device = device->handle();
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE,
                                   0};
    VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type, 0};
    ASSERT_EQ(vkCreateSemaphore(gate->device, &create, nullptr, &gate->semaphore), VK_SUCCESS);
    gpu::SubmissionQueue::TimelineSemaphores dependencies;
    dependencies.wait = {gate->semaphore};
    dependencies.waitValues = {1};
    const auto blocker = queue.submit([](VkCommandBuffer) {}, {gate}, dependencies);
    ASSERT_TRUE(blocker);
    EvaluationRequest request;
    request.network = network;
    request.output = node;
    request.region = {0, 0, 16, 16};
    std::optional<eval::GpuEvaluation> pending;
    std::exception_ptr failure;
    try {
        pending = eval::submitGpu(session.document(), request, *effects, *device, *allocator);
    } catch (...) {
        failure = std::current_exception();
    }
    const auto completion = pending ? pending->completion : std::nullopt;
    const auto charged = allocator->charged_bytes();
    if (completion) {
        EXPECT_FALSE(queue.poll(*completion));
    }
    pending.reset();
    EXPECT_EQ(allocator->charged_bytes(), charged);
    // Release the external gate before any fatal assertion or exception.
    VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, gate->semaphore, 1};
    EXPECT_EQ(vkSignalSemaphore(gate->device, &signal), VK_SUCCESS);
    EXPECT_TRUE(queue.wait(*blocker, 5'000'000'000ULL));
    if (completion) {
        EXPECT_TRUE(queue.wait(*completion, 5'000'000'000ULL));
    }
    if (failure)
        std::rethrow_exception(failure);
    ASSERT_TRUE(completion);
    EXPECT_GT(charged, 0u);
    EXPECT_EQ(allocator->charged_bytes(), 0u);
    EXPECT_FLOAT_EQ(render().pixel(4, 4)[3], 1.0F);
}

INSTANTIATE_TEST_SUITE_P(Backends, RotoNativeLifetime, testing::Values(1, 2),
                         [](const testing::TestParamInfo<int>& info) { return info.param == 1 ? "Glsl" : "Slang"; });

INSTANTIATE_TEST_SUITE_P(Implementations, RotoRaster, testing::Values(0, 1, 2),
                         [](const testing::TestParamInfo<int>& info) {
                             return info.param == 0 ? "Cpu" : info.param == 1 ? "Glsl" : "Slang";
                         });

}  // namespace

// ---------------------------------------------------------------------------
// Renderer-slice regression coverage (issue #93, renderer contribution owner):
// the analytic contour, feather, hierarchy-composition, clipping, channel and
// refusal behaviours the shape-list acceptance examples above do not pin.
// ---------------------------------------------------------------------------

namespace {

// A four-point cubic Bezier approximating a circle: the points sit on the axes
// and the kappa handles (4*(sqrt(2)-1)/3 of the radius) make each quarter an arc.
// A sample inside the circle but OUTSIDE the straight chord between two points
// separates a real curve from a polygon, so the same points tested with zero
// tangents pin the degenerate (straight-segment) case too.
RotoElement bezierCircle(RotoData& data, float cx, float cy, float radius, bool withTangents) {
    const float kappa = withTangents ? 0.5522847498307936F * radius : 0.0F;
    RotoElement shape;
    shape.id = data.nextElementId++;
    shape.name = "Circle" + std::to_string(shape.id);
    const std::array<Vector2Value, 4> positions{Vector2Value{{cx, cy - radius}}, Vector2Value{{cx + radius, cy}},
                                                Vector2Value{{cx, cy + radius}}, Vector2Value{{cx - radius, cy}}};
    const std::array<Vector2Value, 4> inTangents{Vector2Value{{-kappa, 0}}, Vector2Value{{0, -kappa}},
                                                 Vector2Value{{kappa, 0}}, Vector2Value{{0, kappa}}};
    const std::array<Vector2Value, 4> outTangents{Vector2Value{{kappa, 0}}, Vector2Value{{0, kappa}},
                                                  Vector2Value{{-kappa, 0}}, Vector2Value{{0, -kappa}}};
    for (std::size_t index = 0; index < positions.size(); ++index) {
        RotoPoint point;
        point.id = data.nextPointId++;
        point.position = positions[index];
        point.inTangent = inTangents[index];
        point.outTangent = outTangents[index];
        shape.points.push_back(point);
    }
    return shape;
}

// A closed periodic B-spline through `positions`, one tension per point.
RotoElement bsplinePolygon(RotoData& data, const std::vector<Vector2Value>& positions, float tension) {
    RotoElement shape;
    shape.id = data.nextElementId++;
    shape.name = "Spline" + std::to_string(shape.id);
    shape.kind = RotoKind::BSpline;
    for (const Vector2Value position : positions) {
        RotoPoint point;
        point.id = data.nextPointId++;
        point.position = position;
        point.tension = tension;
        shape.points.push_back(point);
    }
    return shape;
}

TEST_P(RotoRaster, CurvedAndControlPolygonFormsSeparateFromTheirChords) {
    submit(setNetworkFormatCommand(network, ImageFormat{32, 32, 1.0F}));
    RotoData curved;
    curved.elements.push_back(bezierCircle(curved, 16, 16, 12, true));
    submit(setRotoDataCommand(network, node, curved));
    // (22.5, 9.5) is inside the 12-pixel circle (radius 9.19) and outside the
    // straight chord from (16,4) to (28,16) (the chord line is y = x - 12), so a
    // real curve covers it and a polygon does not.
    EXPECT_FLOAT_EQ(render({0, 0, 32, 32}).pixel(22, 9)[3], 1.0F);

    submit(setNetworkFormatCommand(network, ImageFormat{16, 16, 1.0F}));
    RotoData straight;
    straight.elements.push_back(bezierCircle(straight, 8, 8, 6, false));
    submit(setRotoDataCommand(network, node, straight));
    EXPECT_FLOAT_EQ(render().pixel(11, 4)[3], 0.0F);
}

TEST_P(RotoRaster, BSplineTensionBlendsBetweenSmoothCurveAndControlPolygon) {
    const std::vector<Vector2Value> triangle{Vector2Value{{0, 0}}, Vector2Value{{12, 0}}, Vector2Value{{0, 12}}};
    RotoData spline;
    spline.elements.push_back(bsplinePolygon(spline, triangle, 0.0F));
    submit(setRotoDataCommand(network, node, spline));
    // At tension 0 the smooth periodic B-spline cuts the (0,0) corner of its
    // control triangle, so the corner sample is outside the matte.
    EXPECT_FLOAT_EQ(render().pixel(0, 0)[3], 0.0F);

    RotoData cusped;
    cusped.elements.push_back(bsplinePolygon(cusped, triangle, 1.0F));
    submit(setRotoDataCommand(network, node, cusped));
    // At tension 1 the curve IS the control polygon, whose interior covers it.
    EXPECT_FLOAT_EQ(render().pixel(0, 0)[3], 1.0F);
}

TEST_P(RotoRaster, FeatherProfileFalloffAndSignShapeTheRamp) {
    RotoData shapes;
    auto shape = rectangle(shapes, 2, 2, 10, 10);
    shape.feather = 4;
    shapes.elements.push_back(shape);

    // Linear, falloff 1 (the default): 1.5 pixels outside the right edge of a
    // 4-pixel feather is 1 - 1.5/4.
    submit(setRotoDataCommand(network, node, shapes));
    EXPECT_NEAR(render().pixel(11, 6)[3], 0.625F, 0.001F);

    // Smooth (smoothstep of the same ramp parameter).
    shape.featherProfile = RotoFeatherProfile::Smooth;
    shapes.elements[0] = shape;
    submit(setRotoDataCommand(network, node, shapes));
    EXPECT_NEAR(render().pixel(11, 6)[3], 0.68359375F, 0.001F);

    // featherFalloff is an exponent on the ramp: pow(0.625, 1/0.5) = 0.625^2.
    shape.featherProfile = RotoFeatherProfile::Linear;
    shape.featherFalloff = 0.5;
    shapes.elements[0] = shape;
    submit(setRotoDataCommand(network, node, shapes));
    EXPECT_NEAR(render().pixel(11, 6)[3], 0.390625F, 0.001F);

    // A NEGATIVE feather ramps inward: half a pixel inside the right edge is
    // 0.5/4 up the ramp, and 3.5 pixels inside is 3.5/4.
    shape.featherFalloff = 1.0;
    shape.feather = -4;
    shapes.elements[0] = shape;
    submit(setRotoDataCommand(network, node, shapes));
    EXPECT_NEAR(render().pixel(9, 6)[3], 0.125F, 0.001F);
    EXPECT_NEAR(render().pixel(6, 6)[3], 0.875F, 0.001F);
}

TEST_P(RotoRaster, GroupOpacityAndInversionApplyToTheCombinedChildren) {
    RotoData shapes;
    RotoElement group;
    group.id = shapes.nextElementId++;
    group.name = "Group";
    group.kind = RotoKind::Group;
    group.opacity = 0.5;
    shapes.elements.push_back(group);
    shapes.elements.push_back(rectangle(shapes, 2, 2, 10, 10, group.id));
    submit(setRotoDataCommand(network, node, shapes));
    EXPECT_FLOAT_EQ(render().pixel(4, 4)[3], 0.5F);
    EXPECT_FLOAT_EQ(render().pixel(12, 4)[3], 0.0F);

    // Inversion applies to the group's combined value, then its opacity: the
    // inside becomes empty and the outside is covered at half strength.
    shapes.elements[0].inverted = true;
    submit(setRotoDataCommand(network, node, shapes));
    EXPECT_FLOAT_EQ(render().pixel(4, 4)[3], 0.0F);
    EXPECT_FLOAT_EQ(render().pixel(12, 4)[3], 0.5F);
}

TEST_P(RotoRaster, ClipRestrictsTheProducedDataWindow) {
    // The matte lies entirely outside the 16x16 canvas, and the request reaches
    // outside it too: whether the sample at (20.5, 20.5) is DATA depends only on
    // the clip rule, so it separates format/intersect from bbox/union/none.
    RotoData shapes;
    shapes.elements.push_back(rectangle(shapes, 20, 20, 28, 28));
    submit(setRotoDataCommand(network, node, shapes));
    const Region outside{-8, -8, 32, 32};

    EXPECT_FLOAT_EQ(render(outside).pixel(28, 28)[3], 0.0F);  // clip format (default)
    submit(setParamCommand(network, node, "clip", ChoiceValue{"bbox"}));
    EXPECT_FLOAT_EQ(render(outside).pixel(28, 28)[3], 1.0F);
    submit(setParamCommand(network, node, "clip", ChoiceValue{"union"}));
    EXPECT_FLOAT_EQ(render(outside).pixel(28, 28)[3], 1.0F);
    submit(setParamCommand(network, node, "clip", ChoiceValue{"intersect"}));
    EXPECT_FLOAT_EQ(render(outside).pixel(28, 28)[3], 0.0F);
    submit(setParamCommand(network, node, "clip", ChoiceValue{"none"}));
    EXPECT_FLOAT_EQ(render(outside).pixel(28, 28)[3], 1.0F);
}

TEST_P(RotoRaster, ReplaceClearsAndAbsentMaskNeverLimits) {
    auto background = std::make_shared<NodeId>();
    submit(addNodeCommand(network, "constcolor", "Background", background));
    submit(transactionCommand("Background",
                              {setParamCommand(network, *background, "color", ColorValue{{0.25F, 0.5F, 0.75F, 1.0F}}),
                               connectCommand(network, {*background, 0}, {node, 0}),
                               setParamCommand(network, node, "outputChannel", std::string{"A"})}));
    RotoData shapes;
    shapes.elements.push_back(rectangle(shapes, 2, 2, 10, 10));
    submit(setRotoDataCommand(network, node, shapes));

    // Drawn OVER the background's alpha where the matte is empty...
    EXPECT_FLOAT_EQ(render().pixel(12, 4)[3], 1.0F);
    // ...and written exactly once `replace` clears it.
    submit(setParamCommand(network, node, "replace", true));
    EXPECT_FLOAT_EQ(render().pixel(12, 4)[3], 0.0F);
    submit(setParamCommand(network, node, "replace", false));

    // A named mask channel with no mask input connected never limits anything
    // (the shared optional-mask contract), and `none` is the default.
    submit(setParamCommand(network, node, "maskChannel", std::string{"A"}));
    EXPECT_FLOAT_EQ(render().pixel(4, 4)[3], 1.0F);
    submit(setParamCommand(network, node, "maskChannel", std::string{"none"}));
    EXPECT_FLOAT_EQ(render().pixel(4, 4)[3], 1.0F);
}

TEST_P(RotoRaster, EmptyGeneratorAndDegenerateHierarchyStayUsable) {
    // No data published at all: an empty generator is a valid all-transparent
    // alpha-only image, not an error.
    const auto empty = render();
    EXPECT_EQ(empty.layout().channels, (std::vector<std::string>{"A"}));
    EXPECT_FLOAT_EQ(empty.pixel(4, 4)[3], 0.0F);

    // A shape without enough points to enclose area contributes nothing.
    RotoData degenerate;
    RotoElement line;
    line.id = degenerate.nextElementId++;
    line.name = "Line";
    for (const Vector2Value position : {Vector2Value{{2, 2}}, Vector2Value{{10, 10}}}) {
        RotoPoint point;
        point.id = degenerate.nextPointId++;
        point.position = position;
        line.points.push_back(point);
    }
    degenerate.elements.push_back(line);
    const auto revision = session.revision();
    const auto refused = session.submit(setRotoDataCommand(network, node, degenerate), {revision, {}});
    EXPECT_FALSE(refused.committed);
    EXPECT_EQ(session.revision(), revision);
}

TEST_P(RotoRaster, TooDeepAHierarchyIsRefusedRatherThanFlattened) {
    RotoData shapes;
    RotoElementId parent = 0;
    for (int level = 0; level < 34; ++level) {
        RotoElement group;
        group.id = shapes.nextElementId++;
        group.name = "Group" + std::to_string(level);
        group.kind = RotoKind::Group;
        group.parent = parent;
        parent = group.id;
        shapes.elements.push_back(group);
    }
    shapes.elements.push_back(rectangle(shapes, 2, 2, 10, 10, parent));
    submit(setRotoDataCommand(network, node, shapes));
    std::string failure;
    try {
        static_cast<void>(render());
    } catch (const std::exception& error) {
        failure = error.what();
    }
    // The refusal names the bound it exceeded; it never evaluates a partial
    // hierarchy and never truncates one.
    EXPECT_NE(failure.find("nests deeper"), std::string::npos) << failure;
}

TEST_P(RotoRaster, InflectedBezierDoesNotCollapseToItsChord) {
    RotoData shapes;
    auto contour = rectangle(shapes, 4, 8, 6, 15);
    contour.points[0].outTangent = {{0, 10}};
    contour.points[1].inTangent = {{0, -10}};
    shapes.elements.push_back(contour);
    submit(setRotoDataCommand(network, node, shapes));
    const auto image = render();
    EXPECT_FLOAT_EQ(image.pixel(4, 9)[3], 0.0F);
    EXPECT_FLOAT_EQ(image.pixel(5, 6)[3], 1.0F);
}

TEST_P(RotoRaster, ScaledCurveRetainsFullResolutionAccuracy) {
    submit(setNetworkFormatCommand(network, ImageFormat{32, 32, 1.0F}));
    RotoData shapes;
    auto contour = bezierCircle(shapes, 0.016F, 0.016F, 0.012F, true);
    contour.scale = {{1000, 1000}};
    shapes.elements.push_back(contour);
    submit(setRotoDataCommand(network, node, shapes));
    EXPECT_FLOAT_EQ(render({0, 0, 32, 32}).pixel(22, 9)[3], 1.0F);
}

TEST_P(RotoRaster, InvertedGeneratorKeepsCanvasCoverageWhenClippedToBounds) {
    RotoData shapes;
    auto contour = rectangle(shapes, 4, 4, 12, 12);
    contour.inverted = true;
    shapes.elements.push_back(contour);
    submit(setRotoDataCommand(network, node, shapes));
    submit(setParamCommand(network, node, "clip", ChoiceValue{"bbox"}));
    const auto image = render();
    EXPECT_FLOAT_EQ(image.pixel(8, 8)[3], 0.0F);
    EXPECT_FLOAT_EQ(image.pixel(1, 1)[3], 1.0F);
}

TEST_P(RotoRaster, EndedContoursDoNotLeakCoverageIntoLaterRows) {
    RotoData shapes;
    shapes.elements.push_back(rectangle(shapes, 2, 2, 6, 14));
    shapes.elements.push_back(rectangle(shapes, 10, 2, 14, 6));
    submit(setRotoDataCommand(network, node, shapes));
    const auto image = render();
    EXPECT_FLOAT_EQ(image.pixel(11, 3)[3], 1.0F);
    EXPECT_FLOAT_EQ(image.pixel(11, 11)[3], 0.0F);
    EXPECT_FLOAT_EQ(image.pixel(3, 11)[3], 1.0F);
}

TEST_P(RotoRaster, MissingConnectedMaskChannelIsZeroRatherThanUnmasked) {
    auto mask = std::make_shared<NodeId>();
    submit(addNodeCommand(network, "constcolor", "Mask", mask));
    submit(connectCommand(network, {*mask, 0}, {node, 1}));
    RotoData shapes;
    shapes.elements.push_back(rectangle(shapes, 2, 2, 10, 10));
    submit(setRotoDataCommand(network, node, shapes));
    submit(setParamCommand(network, node, "maskChannel", std::string{"missing.coverage"}));
    EXPECT_FLOAT_EQ(render().pixel(4, 4)[3], 0.0F);
    submit(setParamCommand(network, node, "invertMask", true));
    EXPECT_FLOAT_EQ(render().pixel(4, 4)[3], 1.0F);
    EXPECT_FLOAT_EQ(render().pixel(12, 4)[3], 0.0F);
}

TEST_P(RotoRaster, EllipsePrimitiveMatchesItsAnalyticInterior) {
    RotoData shapes;
    const auto ellipse = appendRotoEllipse(shapes, 0, "Ellipse", 8, 8, 6, 6);
    ASSERT_NE(ellipse, 0u);
    submit(setRotoDataCommand(network, node, shapes));
    const auto image = render();
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const double radius = std::hypot(x + .5 - 8, y + .5 - 8) / 6;
            if (radius < .93)
                EXPECT_FLOAT_EQ(image.pixel(x, y)[3], 1.0F) << x << ',' << y;
            else if (radius > 1.07)
                EXPECT_FLOAT_EQ(image.pixel(x, y)[3], 0.0F) << x << ',' << y;
        }
    }
}

TEST_P(RotoRaster, DisablingShapeFeatherAlsoDisablesItsPointWidths) {
    RotoData shapes;
    auto shape = rectangle(shapes, 4, 4, 12, 12);
    for (std::size_t i = 0; i < shape.points.size(); ++i)
        shape.points[i].feather = 4;
    shapes.elements.push_back(shape);
    submit(setRotoDataCommand(network, node, shapes));
    EXPECT_NEAR(render().pixel(3, 8)[3], .875F, .001F);
    shapes.elements[0].featherEnabled = false;
    submit(setRotoDataCommand(network, node, shapes));
    EXPECT_FLOAT_EQ(render().pixel(3, 8)[3], 0.0F);
    EXPECT_FLOAT_EQ(render().pixel(8, 8)[3], 1.0F);
}

TEST_P(RotoRaster, AnimatedFeatherProfileChangesTheRasterizedRamp) {
    RotoData shapes;
    auto shape = rectangle(shapes, 4, 4, 12, 12);
    shape.feather = 4;
    shapes.elements.push_back(shape);
    submit(setRotoDataCommand(network, node, shapes));
    EXPECT_NEAR(render().pixel(3, 8)[3], .875F, .001F);
    const ParameterAddress address{network, node, "featherProfile", 0, shape.id, 0};
    submit(setKeyframesCommand(
        {{address, Keyframe{.time = 0, .value = ChoiceValue{"smooth"}, .interpolation = KeyInterpolation::Hold}}}));
    EXPECT_NEAR(render().pixel(3, 8)[3], .95703125F, .001F);
}

TEST_P(RotoRaster, NarrowOutputDemandStillReadsTheNamedMaskChannel) {
    auto mask = std::make_shared<NodeId>();
    submit(addNodeCommand(network, "constcolor", "Mask", mask));
    submit(
        transactionCommand("Named mask", {setParamCommand(network, *mask, "color", ColorValue{{.25F, .5F, .75F, 1.0F}}),
                                          connectCommand(network, {*mask, 0}, {node, 1}),
                                          setParamCommand(network, node, "maskChannel", std::string{"R"})}));
    RotoData shapes;
    shapes.elements.push_back(rectangle(shapes, 2, 2, 10, 10));
    submit(setRotoDataCommand(network, node, shapes));
    EXPECT_FLOAT_EQ(render({0, 0, 16, 16}, 1, {"A"}).pixel(4, 4)[3], .25F);
}

TEST_P(RotoRaster, RotationUsesPhysicalPixelAspect) {
    submit(setNetworkFormatCommand(network, ImageFormat{16, 16, 2.0F}));
    RotoData shapes;
    auto shape = rectangle(shapes, 2, 6, 6, 10);
    shape.pivot = Vector2Value{{8, 8}};
    shape.rotation = 90;
    shapes.elements.push_back(shape);
    submit(setRotoDataCommand(network, node, shapes));
    const auto image = render();
    EXPECT_FLOAT_EQ(image.pixel(7, 1)[3], 1.0F);
    EXPECT_FLOAT_EQ(image.pixel(8, 3)[3], 1.0F);
    EXPECT_FLOAT_EQ(image.pixel(6, 1)[3], 0.0F);
    EXPECT_FLOAT_EQ(image.pixel(8, 5)[3], 0.0F);
}

TEST_P(RotoRaster, AncestorAndPointFeatherTravelThroughGroupScale) {
    RotoData shapes;
    const auto group = appendRotoGroup(shapes, 0, "Feather group");
    shapes.elements[0].scale = Vector2Value{{2, 2}};
    shapes.elements[0].feather = 3;
    auto shape = rectangle(shapes, 4, 4, 6, 6, group);
    shape.feather = 1;
    for (std::size_t i = 0; i < shape.points.size(); ++i)
        shape.points[i].feather = 2;
    shapes.elements.push_back(shape);
    submit(setRotoDataCommand(network, node, shapes));
    EXPECT_NEAR(render().pixel(7, 10)[3], 1.0F - .5F / 12.0F, .0001F);
    shapes.elements[1].featherEnabled = false;
    submit(setRotoDataCommand(network, node, shapes));
    EXPECT_NEAR(render().pixel(7, 10)[3], 1.0F - .5F / 6.0F, .0001F);
}

TEST_P(RotoRaster, EightTemporalSamplesAverageTheCompleteExposure) {
    RotoData shapes;
    const auto shape = rectangle(shapes, 7, 0, 9, 16);
    shapes.elements.push_back(shape);
    submit(setRotoDataCommand(network, node, shapes));
    const ParameterAddress address{network, node, "translation", 0, shape.id, 0};
    submit(setKeyframesCommand({{address, Keyframe{.time = -.5, .value = Vector2Value{{-8, 0}}}},
                                {address, Keyframe{.time = .5, .value = Vector2Value{{8, 0}}}}}));
    submit(transactionCommand("Exposure", {setParamCommand(network, node, "samples", std::int64_t{8}),
                                           setParamCommand(network, node, "shutter", 1.0)}));
    const auto image = render();
    EXPECT_FLOAT_EQ(image.pixel(0, 8)[3], .125F);
    EXPECT_FLOAT_EQ(image.pixel(5, 8)[3], .125F);
    EXPECT_FLOAT_EQ(image.pixel(15, 8)[3], .125F);
}

}  // namespace
