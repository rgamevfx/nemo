// Installed extension package contract (issue #37).
//
// The subject is the REAL, separately built and installed ColorWarp example
// located through NEMO_TEST_EXTENSION_ROOT: a trusted native package that is no
// part of this build. Every scenario drives it through the ordinary host seams
// the application uses — nemo::extensions::InstalledPackages discovery and
// activation, the public ProjectSession commands/gestures/file layer, the public
// CPU reference and the native effect library — never through a private
// collaborator and never through a fabricated registration.
//
// What this file defends, in the words of the pre-edit contract:
//   * installed discovery, the manifest's schema/editor/panel projection, and
//     refusal diagnostics that never hide an unrelated supported package;
//   * the authored-state identity rule (only a declared state change makes an
//     installed type unavailable for a saved document) with the complete
//     authored state retained and recovered when the package returns;
//   * the published deformation, evaluated against INDEPENDENT pixels:
//     identity/neutral/reset bypasses, the smooth 12 x 4 tensor mapping at knots,
//     cell edges, the periodic seam and the fixed boundary rings, luminance and
//     alpha preservation, unclamped negative/HDR values, and the incomplete-RGB
//     bypass;
//   * the conservative fold constraint enforced atomically by commands, gestures
//     and animation through the real package's own validation, with supported
//     animation on the authored coordinates;
//   * ownership: installed adapters and the native library outlive the loader,
//     and an in-flight native submission keeps them alive until completion.
//
// The expected pixels are an independent implementation of the published mapping
// contract, worked out numerically OUTSIDE the package (opponent-chroma encode,
// 12-cell periodic hue x 4-cell radial tensor cubic smoothstep displacement in
// cell coordinates, decode preserving luminance) and frozen here as literals.
// Agreement with the package is evidence, never the source of the expectation.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "ScopedEnvironment.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include "nemo/core/session/ProjectFile.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/extensions/InstalledPackages.hpp"
#if defined(NEMO_TEST_GPU)
#include "nemo/eval/DeliveryJob.hpp"
#include "nemo/eval/GpuExecutor.hpp"
#include "nemo/extensions/GpuPackages.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/gpu/Submit.hpp"
#include "nemo/media/ImageIO.hpp"
#include <chrono>
#endif

using namespace nemo;

namespace {

namespace fs = std::filesystem;

// --- manifest facts this build projects (examples/colorwarp/manifest.json) ---
constexpr const char* kColorWarpType{"org.nemo.colorwarp"};
constexpr const char* kPanelId{"org.nemo.colorwarp.example"};
// Published comparison tolerance: 2e-5 absolute + 2e-5 relative.
constexpr double kTolerance = 2e-5;
constexpr std::array<double, 3> kLumaWeights{0.2126, 0.7152, 0.0722};

[[nodiscard]] double luma(const std::array<float, kImageChannels>& pixel) {
    return kLumaWeights[0] * pixel[0] + kLumaWeights[1] * pixel[1] + kLumaWeights[2] * pixel[2];
}

// --- the fixture mesh and its independent pixels ---------------------------
//
// Authored displacements (cell units; every other knot is zero):
//   hue0 = +0.2, saturation0 = +0.2, hue11 = -0.08, hue24 = +0.2 (ring 3, spoke
//   0). The worst cell of that mesh satisfies the published fold bound with
//   margin — 1.5 * (0.28 + 0.20) = 0.72 < 0.95 — so the mesh is admissible.
//
// Each row was obtained outside this repository by (a) constructing an RGB triple
// whose OPPONENT ENCODE (Y = .2126R + .7152G + .0722B, u = (2R-G-B)/sqrt6,
// v = (G-B)/sqrt2, c = hypot(u,v), radius = c/(1+|Y|+c), hue = atan2(v,u)/2pi)
// recovers the intended (Y, radius, hue), (b) mapping u = 12*hue, v = 4*radius
// through the authored mesh with tensor cubic smoothstep weights, and (c) decoding
// back to straight RGBA. "disp" is the resulting displacement in cell units.
//
//   name            Y      radius  hue         disp          what it isolates
//   knot0r1        +0.35   1/4     0           (+0.2, +0.2)  exactly on knot (h=0,r=1)
//   knot0r1_signed -0.40   1/4     0           (+0.2, +0.2)  negative luminance, alpha 0.6
//   halfcell       +0.35   1/4     1/24        (+0.1, +0.1)  smoothstep half weight (t=0.5)
//   cellcorner     +0.35   1/4     1/12        (0, 0)        exactly on a shared mesh edge
//   seam_low       +0.35   1/4     1 - 1e-6    (+0.2, +0.2)  cell 11, wraps into spoke 0
//   seam_high      +0.35   1/4     1e-6        (+0.2, +0.2)  cell 0, just past the seam
//   centre_taper   +0.35   1/8     0           (+0.1, +0.1)  v=0.5 toward the fixed r=0 centre
//   ring3          +0.35   3/4     0           (+0.2, 0)     v=3, HDR result outside [0,1]
//   ring3_taper    +0.35   7/8     0           (+0.1, 0)     v=3.5 toward the fixed r=4 boundary
//   neutral        +0.35   0       0           (0, 0)        c=0 neutral, alpha 0.25
struct WarpSample {
    const char* name;
    std::array<float, kImageChannels> input;  // straight RGBA handed to the plate
    std::array<float, kImageChannels> expected;
};

constexpr std::array<WarpSample, 10> kWarpSamples{{
    {"knot0r1", {0.783963859F, 0.232828662F, 0.232828662F, 1.0F}, {0.877399862F, 0.21544309F, 0.129915446F, 1.0F}},
    {"knot0r1_signed",
     {0.0500365868F, -0.521511018F, -0.521511018F, 0.6000000238418579F},
     {0.146933183F, -0.53954047F, -0.628235817F, 0.6000000238418579F}},
    {"halfcell", {0.716222167F, 0.266222179F, 0.101510733F, 1.0F}, {0.747705579F, 0.263138026F, 0.0393556841F, 1.0F}},
    {"cellcorner",
     {0.623523057F, 0.305325001F, -0.0128730582F, 1.0F},
     {0.623523057F, 0.305325001F, -0.0128730582F, 1.0F}},
    {"seam_low", {0.783965111F, 0.232827947F, 0.23283194F, 1.0F}, {0.877401829F, 0.215442076F, 0.129919544F, 1.0F}},
    {"seam_high", {0.783962548F, 0.232829377F, 0.232825369F, 1.0F}, {0.877397835F, 0.215444103F, 0.129911333F, 1.0F}},
    {"centre_taper",
     {0.535984516F, 0.299783707F, 0.299783707F, 1.0F},
     {0.573761761F, 0.291200578F, 0.273567766F, 1.0F}},
    {"ring3", {4.25567484F, -0.7045421F, -0.7045421F, 1.0F}, {4.04179907F, -0.591898382F, -1.19059193F, 1.0F}},
    {"ring3_taper", {9.46324062F, -2.11059809F, -2.11059809F, 1.0F}, {9.22588348F, -1.98237693F, -2.68181133F, 1.0F}},
    {"neutral", {0.349999994F, 0.349999994F, 0.349999994F, 0.25F}, {0.349999994F, 0.349999994F, 0.349999994F, 0.25F}},
}};

[[nodiscard]] const WarpSample& sample(std::string_view name) {
    const auto found = std::find_if(kWarpSamples.begin(), kWarpSamples.end(),
                                    [name](const WarpSample& candidate) { return name == candidate.name; });
    if (found == kWarpSamples.end())
        throw std::runtime_error("unknown sample " + std::string{name});
    return *found;
}

// --- small helpers ---------------------------------------------------------

[[nodiscard]] std::string describe(const EditResult& result) {
    return result.error ? result.error->message : std::string{"no error reported"};
}

[[nodiscard]] bool contains(const std::vector<std::string>& values, std::string_view needle) {
    return std::any_of(values.begin(), values.end(),
                       [needle](const std::string& value) { return value.find(needle) != std::string::npos; });
}

// Authored state of an unavailable installed type survives either as a typed
// schema-v3 record (self-describing) or as verbatim opaque data; both spellings
// must carry the value the document was saved with.
[[nodiscard]] bool retainsParameter(const NodeInstance& node, const std::string& key, const ParameterValue& expected) {
    if (const auto found = node.params.find(key); found != node.params.end())
        return found->second == expected;
    if (!node.opaqueParams.is_object())
        return false;
    const auto opaque = node.opaqueParams.find(key);
    if (opaque == node.opaqueParams.end())
        return false;
    const nlohmann::json& value = opaque->is_object() && opaque->contains("value") ? opaque->at("value") : *opaque;
    if (std::holds_alternative<bool>(expected))
        return value.is_boolean() && value.get<bool>() == std::get<bool>(expected);
    if (std::holds_alternative<double>(expected))
        return value.is_number() && value.get<double>() == std::get<double>(expected);
    return false;
}

void expectPixelNear(const CpuImage& image, const std::array<float, kImageChannels>& expected, std::string_view what) {
    ASSERT_EQ(image.width(), 1) << what;
    ASSERT_EQ(image.height(), 1) << what;
    const std::array<float, kImageChannels> actual = image.pixel(0, 0);
    for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
        const double allowed = kTolerance + kTolerance * std::abs(static_cast<double>(expected[channel]));
        EXPECT_NEAR(actual[channel], expected[channel], allowed)
            << what << " channel " << channel << " (actual " << actual[channel] << ")";
    }
}

// The identity bypass is exact: the authored values cross the effect verbatim, so
// this deliberately asserts float equality instead of a tolerance.
void expectPixelExactly(const CpuImage& image, const std::array<float, kImageChannels>& expected,
                        std::string_view what) {
    ASSERT_EQ(image.width(), 1) << what;
    ASSERT_EQ(image.height(), 1) << what;
    const std::array<float, kImageChannels> actual = image.pixel(0, 0);
    for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
        EXPECT_FLOAT_EQ(actual[channel], expected[channel]) << what << " channel " << channel;
    }
}

// --- the installed fixture --------------------------------------------------

class ExtensionTest : public ::testing::Test {
protected:
    enum class Package { Ready, Skip, Broken };

    void SetUp() override {
        std::error_code error;
        dir_ = fs::temp_directory_path(error) /
               ("nemo-extension-" + std::to_string(static_cast<long>(::getpid())) + "-" + std::to_string(counter_++));
        fs::remove_all(dir_, error);
        ASSERT_TRUE(fs::create_directories(dir_, error) || !error);
    }

    void TearDown() override {
        std::error_code error;
        fs::remove_all(dir_, error);
    }

    // The installed example is an OPTIONAL fixture: an unset root skips, while a
    // root that was configured and does not produce the supported package FAILS,
    // because a wrong install path or a refused package must never read as "the
    // fixture is not available".
    [[nodiscard]] Package locatePackage() {
        const char* raw = std::getenv("NEMO_TEST_EXTENSION_ROOT");
        if (raw == nullptr || *raw == '\0')
            return Package::Skip;
        std::error_code error;
        fs::path root{raw};
        if (fs::exists(root / "manifest.json", error))
            root = root.parent_path();  // the package folder itself was configured
        if (!fs::is_directory(root, error)) {
            failure_ = "NEMO_TEST_EXTENSION_ROOT '" + root.string() + "' is not a directory";
            return Package::Broken;
        }
        fs::path package;
        for (const fs::directory_entry& entry : fs::directory_iterator(root, error)) {
            if (entry.is_directory(error) && fs::exists(entry.path() / "manifest.json", error)) {
                package = entry.path();
                break;
            }
        }
        if (package.empty()) {
            failure_ = "no package folder with manifest.json under " + root.string();
            return Package::Broken;
        }
        installedRoot_ = root;
        packageDir_ = package;
        return Package::Ready;
    }

    // The loader over the located root, or nullopt after reporting — with the
    // loader's own diagnostics — that a configured root did not activate the
    // supported package.
    [[nodiscard]] std::optional<nemo::extensions::InstalledPackages> openPackages() {
        nemo::extensions::InstalledPackages packages({installedRoot_});
        if (packages.contributions()->find(kColorWarpType) == nullptr) {
            failure_ = "root " + installedRoot_.string() + " did not activate " + kColorWarpType;
            for (const std::string& diagnostic : packages.diagnostics())
                failure_ += "\n  " + diagnostic;
            ADD_FAILURE() << failure_;
            return std::nullopt;
        }
        return packages;
    }

    // A private copy of the installed package: a mutation scenario never touches
    // what the harness installed.
    [[nodiscard]] fs::path copyPackage(const fs::path& root, std::string_view folder) const {
        const fs::path destination = root / folder;
        std::error_code error;
        fs::create_directories(root, error);
        fs::copy(packageDir_, destination, fs::copy_options::recursive, error);
        EXPECT_FALSE(error) << "cannot copy " << packageDir_.string() << ": " << error.message();
        return destination;
    }

    [[nodiscard]] fs::path installedRoot() const { return installedRoot_; }
    [[nodiscard]] fs::path packageDir() const { return packageDir_; }
    [[nodiscard]] fs::path workspace() const { return dir_; }
    [[nodiscard]] const std::string& failure() const { return failure_; }

private:
    static int counter_;
    fs::path dir_;
    fs::path installedRoot_;
    fs::path packageDir_;
    std::string failure_;
};

int ExtensionTest::counter_ = 0;

// Locate the real package; an unset root skips, a configured root without the
// package fails with the reason.
#define NEMO_REQUIRE_INSTALLED_PACKAGE(fixture)                                                                        \
    switch ((fixture).locatePackage()) {                                                                               \
    case ExtensionTest::Package::Skip:                                                                                 \
        GTEST_SKIP() << "NEMO_TEST_EXTENSION_ROOT is not set; no installed ColorWarp package to exercise";             \
    case ExtensionTest::Package::Broken:                                                                               \
        ADD_FAILURE() << (fixture).failure();                                                                          \
        return;                                                                                                        \
    case ExtensionTest::Package::Ready:                                                                                \
        break;                                                                                                         \
    }

// --- a real installed registration driven through the public session ---------
// plate(constcolor) -> ColorWarp -> Output, authored with the
// ordinary session commands.
struct WarpChain {
    std::shared_ptr<const NodeContributions> registry;
    std::unique_ptr<ProjectSession> session;
    NetworkId network{kInvalidNetwork};
    NodeId warp{kInvalidNode};
    NodeId plate{kInvalidNode};
    NodeId output{kInvalidNode};
    std::string failure;
};

[[nodiscard]] EditResult submit(WarpChain& chain, Command command) {
    return chain.session->submit(std::move(command), EditOptions{chain.session->revision(), {}});
}

[[nodiscard]] std::unique_ptr<WarpChain> makeWarpChain(std::shared_ptr<const NodeContributions> registry,
                                                       std::array<float, kImageChannels> plateColor) {
    auto chain = std::make_unique<WarpChain>();
    chain->registry = std::move(registry);
    chain->session = std::make_unique<ProjectSession>(Document(chain->registry->catalog()), 256, chain->registry);
    chain->network = chain->session->document().rootNetworkId();
    chain->output = chain->session->document().network(chain->network).defaultOutput();

    auto created = std::make_shared<NodeId>();
    EditResult result = submit(*chain, addNodeCommand(chain->network, "constcolor", "plate", created));
    chain->plate = created ? *created : kInvalidNode;
    if (!result.committed) {
        chain->failure = "cannot create the plate: " + describe(result);
        return chain;
    }
    result =
        submit(*chain, setParamCommand(chain->network, chain->plate, "color", ParameterValue{ColorValue{plateColor}}));
    if (!result.committed) {
        chain->failure = "cannot author the plate color: " + describe(result);
        return chain;
    }
    created = std::make_shared<NodeId>();
    result = submit(*chain, addNodeCommand(chain->network, kColorWarpType, "ColorWarp", created));
    chain->warp = created ? *created : kInvalidNode;
    if (!result.committed) {
        chain->failure = std::string{"cannot create "} + kColorWarpType + ": " + describe(result);
        return chain;
    }
    if (!submit(*chain, connectCommand(chain->network, {chain->plate, 0}, {chain->warp, 0})).committed) {
        chain->failure = "cannot wire the plate into the installed node";
        return chain;
    }
    if (!submit(*chain, connectCommand(chain->network, {chain->warp, 0}, {chain->output, 0})).committed) {
        chain->failure = "cannot wire the installed node into the output";
        return chain;
    }
    return chain;
}

// One batch of authored mesh values, addressed by their stable parameter keys.
[[nodiscard]] Command meshCommand(const WarpChain& chain,
                                  std::initializer_list<std::pair<const char*, double>> values) {
    std::vector<ParameterEdit> edits;
    edits.reserve(values.size());
    for (const auto& [key, value] : values)
        edits.push_back({{chain.network, chain.warp, key}, ParameterValue{value}});
    return setParametersCommand(std::move(edits));
}

// The fixture mesh: two coordinates on the first spoke, a neighbour on the last
// spoke, and one free knot on the outer editable ring.
[[nodiscard]] Command analyticMeshCommand(const WarpChain& chain) {
    return meshCommand(chain, {{"hue0", 0.2}, {"saturation0", 0.2}, {"hue11", -0.08}, {"hue24", 0.2}});
}

[[nodiscard]] EvaluationRequest requestFor(const WarpChain& chain, std::int64_t time = 0) {
    EvaluationRequest request;
    request.network = chain.network;
    request.output = chain.output;
    request.region = {0, 0, 1, 1};
    request.localTime = time;
    return request;
}

[[nodiscard]] CpuImage renderCpu(const WarpChain& chain, std::int64_t time = 0) {
    return evaluateCpu(chain.session->document(), requestFor(chain, time), nullptr, nullptr, chain.registry).image;
}

// The node's authored parameter records (empty when nothing is authored).
[[nodiscard]] const ParameterValues& authoredParams(const WarpChain& chain) {
    static const ParameterValues kEmpty;
    const NodeInstance* node = chain.session->document().network(chain.network).graph().node(chain.warp);
    return node == nullptr ? kEmpty : node->params;
}

// --- compatibility fixtures for the refusal scenarios -----------------------

[[nodiscard]] nlohmann::json readManifest(const fs::path& package) {
    std::ifstream stream(package / "manifest.json");
    nlohmann::json manifest;
    stream >> manifest;
    return manifest;
}

void writeManifest(const fs::path& package, const nlohmann::json& manifest) {
    std::ofstream stream(package / "manifest.json", std::ios::trunc);
    stream << manifest.dump(2);
}

// A refused fixture differs from the supported copy only in the mutated
// declaration, so the reason for the refusal cannot be anything else. The
// declared node type travels with the identity to keep a mutant from colliding
// with the supported package.
void mutateManifest(const fs::path& package, const std::string& identity,
                    const std::function<void(nlohmann::json&)>& edit) {
    nlohmann::json manifest = readManifest(package);
    const std::string previous = manifest.at("id").get<std::string>();
    for (auto& editor : manifest["editors"]) {
        const std::string oldId = editor.at("id").get<std::string>();
        const std::string newId = identity + oldId.substr(previous.size());
        editor["id"] = newId;
        for (auto& parameter : manifest["node"]["parameters"]) {
            if (parameter.value("editor", std::string{}) == oldId)
                parameter["editor"] = newId;
        }
    }
    for (auto& panel : manifest["panels"]) {
        const std::string oldId = panel.at("id").get<std::string>();
        panel["id"] = identity + oldId.substr(previous.size());
    }
    manifest["id"] = identity;
    manifest["node"]["type"] = identity;
    edit(manifest);
    writeManifest(package, manifest);
}

}  // namespace

// ---------------------------------------------------------------------------
// Roots: explicit, ordered and never the working directory; a root that holds no
// package contributes nothing instead of failing.
// ---------------------------------------------------------------------------
TEST_F(ExtensionTest, InstalledRootsAreExplicitAndAPackagelessRootContributesNothing) {
    NEMO_REQUIRE_INSTALLED_PACKAGE(*this);

    const fs::path emptyRoot = workspace() / "empty-root";
    ASSERT_TRUE(fs::create_directories(emptyRoot));

    {
        // An empty entry is dropped, explicit roots keep their order, and the
        // production discovery function feeds the loader the application uses.
#if defined(_WIN32)
        constexpr const char* separators = ";;";
#else
        constexpr const char* separators = "::";
#endif
        const test::ScopedEnvironment path{"NEMO_EXTENSION_PATH",
                                           installedRoot().string() + separators + emptyRoot.string()};
        const std::vector<fs::path> roots = nemo::extensions::installedPackageRoots();
        ASSERT_EQ(roots.size(), 2U);
        EXPECT_EQ(roots[0], installedRoot());
        EXPECT_EQ(roots[1], emptyRoot);

        nemo::extensions::InstalledPackages packages(nemo::extensions::installedPackageRoots());
        ASSERT_NE(packages.contributions()->find(kColorWarpType), nullptr);
        EXPECT_FALSE(contains(packages.diagnostics(), kColorWarpType));
    }
    {
        // With no explicit path the shared user data location is the root — never
        // the working directory or the open project.
        const test::ScopedEnvironment path{"NEMO_EXTENSION_PATH", std::nullopt};
        const fs::path data = workspace() / "user-data";
#if defined(_WIN32)
        const test::ScopedEnvironment home{"LOCALAPPDATA", data.string()};
        const auto expected = data / "Nemo" / "extensions";
#else
        const test::ScopedEnvironment home{"XDG_DATA_HOME", data.string()};
        const auto expected = data / "nemo" / "extensions";
#endif
        const std::vector<fs::path> roots = nemo::extensions::installedPackageRoots();
        ASSERT_EQ(roots.size(), 1U);
        EXPECT_EQ(roots.front(), expected);
    }

    // A root that does not exist and one that holds no package are both skipped,
    // and the built-in inventory is untouched.
    nemo::extensions::InstalledPackages none({workspace() / "absent", emptyRoot});
    EXPECT_TRUE(none.diagnostics().empty());
    EXPECT_TRUE(none.panels().empty());
    EXPECT_EQ(none.contributions()->find(kColorWarpType), nullptr);
    EXPECT_NE(none.contributions()->find("constcolor"), nullptr);
}

// ---------------------------------------------------------------------------
// Three independent reasons the mapping must be an exact bypass, plus the
// neutral and strength-zero cases where the authored mesh is deliberately
// non-trivial.
// ---------------------------------------------------------------------------
TEST_F(ExtensionTest, WarpIdentityNeutralAndAuthoredResetAreExactBypasses) {
    NEMO_REQUIRE_INSTALLED_PACKAGE(*this);
    const auto packages = openPackages();
    if (!packages)
        return;

    const WarpSample& signedPlate = sample("knot0r1_signed");
    auto chain = makeWarpChain(packages->contributions(), signedPlate.input);
    ASSERT_TRUE(chain->failure.empty()) << chain->failure;

    // 1) The authored defaults are an exact bypass: negative values, HDR values
    //    and alpha cross the effect unchanged.
    expectPixelExactly(renderCpu(*chain), signedPlate.input, "default mesh");

    // 2) The fixture mesh really does deform that plate, so (1) is the identity
    //    rule and not an effect that does nothing.
    ASSERT_TRUE(submit(*chain, analyticMeshCommand(*chain)).committed);
    expectPixelNear(renderCpu(*chain), signedPlate.expected, "authored mesh");

    // 3) strength == 0 is the identity regardless of the authored displacements.
    ASSERT_TRUE(submit(*chain, meshCommand(*chain, {{"strength", 0.0}})).committed);
    expectPixelExactly(renderCpu(*chain), signedPlate.input, "strength zero");
    ASSERT_TRUE(submit(*chain, meshCommand(*chain, {{"strength", 1.0}})).committed);
    expectPixelNear(renderCpu(*chain), signedPlate.expected, "strength restored");

    // 4) Pin is a protection flag on the authored point, never deformation: it is
    //    stored and undoable, and the pixels do not move.
    ASSERT_TRUE(submit(*chain, setParamCommand(chain->network, chain->warp, "pin0", ParameterValue{true})).committed);
    const auto pinned = authoredParams(*chain).find("pin0");
    ASSERT_NE(pinned, authoredParams(*chain).end());
    EXPECT_EQ(pinned->second, ParameterValue{true});
    expectPixelNear(renderCpu(*chain), signedPlate.expected, "pin does not deform");

    // 5) Reset-all clears the displacements AND the pin in one command, restoring
    //    the exact bypass; one undo brings the whole authored mesh back.
    std::vector<ParameterEdit> reset;
    for (const char* key : {"hue0", "saturation0", "hue11", "hue24", "pin0"})
        reset.push_back({{chain->network, chain->warp, key}, std::nullopt});
    ASSERT_TRUE(submit(*chain, setParametersCommand(std::move(reset))).committed);
    expectPixelExactly(renderCpu(*chain), signedPlate.input, "reset mesh");
    ASSERT_TRUE(chain->session->undo(EditOptions{chain->session->revision(), {}}).committed);
    expectPixelNear(renderCpu(*chain), signedPlate.expected, "undo restores the authored mesh");

    // 6) Neutral input (no chroma) is fixed by the mapping even under the same
    //    authored mesh: radius 0 sits on the fixed centre of the mesh.
    const WarpSample& neutral = sample("neutral");
    auto neutralChain = makeWarpChain(packages->contributions(), neutral.input);
    ASSERT_TRUE(neutralChain->failure.empty()) << neutralChain->failure;
    ASSERT_TRUE(submit(*neutralChain, analyticMeshCommand(*neutralChain)).committed);
    const CpuImage neutralRendered = renderCpu(*neutralChain);
    expectPixelNear(neutralRendered, neutral.expected, "neutral stays neutral");
    const std::array<float, kImageChannels> achromatic = neutralRendered.pixel(0, 0);
    EXPECT_NEAR(achromatic[0], achromatic[1], kTolerance);
    EXPECT_NEAR(achromatic[1], achromatic[2], kTolerance);
    EXPECT_FLOAT_EQ(achromatic[3], neutral.input[3]);
}

// ---------------------------------------------------------------------------
// The mapping itself against independent pixels: knots, cell edges, the periodic
// seam and the tapering toward the fixed boundary rings.
// ---------------------------------------------------------------------------
TEST_F(ExtensionTest, WarpMappingMatchesIndependentPixelsAtKnotsCellEdgesAndSeam) {
    NEMO_REQUIRE_INSTALLED_PACKAGE(*this);
    const auto packages = openPackages();
    if (!packages)
        return;

    auto chain = makeWarpChain(packages->contributions(), kWarpSamples.front().input);
    ASSERT_TRUE(chain->failure.empty()) << chain->failure;
    ASSERT_TRUE(submit(*chain, analyticMeshCommand(*chain)).committed);

    for (const WarpSample& row : kWarpSamples) {
        ASSERT_TRUE(submit(*chain, setParamCommand(chain->network, chain->plate, "color",
                                                   ParameterValue{ColorValue{row.input}}))
                        .committed)
            << row.name;
        expectPixelNear(renderCpu(*chain), row.expected, row.name);
    }

    // Smooth across the periodic seam: the two samples that straddle u = 0/12
    // agree well inside the tolerance band. A mapping that did not wrap spoke 0
    // would differ by the whole authored displacement (~0.09 in the first
    // channel) — and would already have missed its own independent row above.
    const auto renderPlate = [&](const WarpSample& row) {
        EXPECT_TRUE(submit(*chain, setParamCommand(chain->network, chain->plate, "color",
                                                   ParameterValue{ColorValue{row.input}}))
                        .committed);
        return renderCpu(*chain).pixel(0, 0);
    };
    const std::array<float, kImageChannels> low = renderPlate(sample("seam_low"));
    const std::array<float, kImageChannels> high = renderPlate(sample("seam_high"));
    for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
        EXPECT_NEAR(low[channel], high[channel], 1e-4) << "seam continuity, channel " << channel;
    }

    // The rows above already carry the radial taper toward the fixed boundary
    // rings as independent pixels: centre_taper is half the knot displacement at
    // v = 0.5, and ring3_taper is half the ring-3 displacement at v = 3.5, so the
    // drawn field is the same smoothstep field the renderer evaluates rather than
    // a decorative mesh.
}

// ---------------------------------------------------------------------------
// The colour-domain contract of the same mapping: luminance and alpha are
// preserved, signed/HDR values are never clamped, and an image that identifies no
// complete primary RGB set is bypassed instead of invented.
// ---------------------------------------------------------------------------
TEST_F(ExtensionTest, WarpPreservesLuminanceUnclampedExtremesAlphaAndDataOnlyChannels) {
    NEMO_REQUIRE_INSTALLED_PACKAGE(*this);
    const auto packages = openPackages();
    if (!packages)
        return;

    auto chain = makeWarpChain(packages->contributions(), kWarpSamples.front().input);
    ASSERT_TRUE(chain->failure.empty()) << chain->failure;
    ASSERT_TRUE(submit(*chain, analyticMeshCommand(*chain)).committed);

    bool sawNegative = false;
    bool sawAboveOne = false;
    for (const WarpSample& row : kWarpSamples) {
        ASSERT_TRUE(submit(*chain, setParamCommand(chain->network, chain->plate, "color",
                                                   ParameterValue{ColorValue{row.input}}))
                        .committed)
            << row.name;
        const std::array<float, kImageChannels> actual = renderCpu(*chain).pixel(0, 0);
        // Luminance is the fixed point of the warp, and alpha is carried through
        // untouched, exactly.
        const double allowed = kTolerance + kTolerance * std::abs(luma(row.input));
        EXPECT_NEAR(luma(actual), luma(row.input), allowed) << row.name << " luminance";
        EXPECT_FLOAT_EQ(actual[3], row.input[3]) << row.name << " alpha";
        sawNegative = sawNegative || actual[0] < 0.0F || actual[1] < 0.0F || actual[2] < 0.0F;
        sawAboveOne = sawAboveOne || actual[0] > 1.0F || actual[1] > 1.0F || actual[2] > 1.0F;
    }
    // No implicit [0,1] clamp: the extreme rows really do leave the display range.
    EXPECT_TRUE(sawNegative);
    EXPECT_TRUE(sawAboveOne);

    // Declare an alpha-only source. Shuffle is not a channel-removal fixture:
    // its contract deliberately carries every base channel through.
    const auto installed = packages->contributions()->entries();
    std::vector<NodeContribution> entries(installed.begin(), installed.end());
    for (auto& entry : entries) {
        if (entry.descriptor.type == "constcolor") {
            entry.describe = [](const NodeDescriptionContext& context) {
                auto description = context.inherited;
                description.channels = {"A"};
                return description;
            };
        }
    }
    const auto alphaRegistry = std::make_shared<const NodeContributions>(std::move(entries));
    auto dataChain = makeWarpChain(alphaRegistry, {0.9F, 0.2F, 0.4F, 0.25F});
    ASSERT_TRUE(dataChain->failure.empty()) << dataChain->failure;
    ASSERT_TRUE(submit(*dataChain, analyticMeshCommand(*dataChain)).committed);
    auto request = requestFor(*dataChain);
    request.channels = {"A"};
    const CpuImage rendered =
        evaluateCpu(dataChain->session->document(), request, nullptr, nullptr, dataChain->registry).image;
    ASSERT_EQ(rendered.layout().channels, (std::vector<std::string>{"A"}))
        << "a data-only input must not grow a primary RGB set";
    EXPECT_FLOAT_EQ(rendered.data()[0], 0.25F);
}

// ---------------------------------------------------------------------------
// The conservative fold constraint through the real package's own validation:
// admissible alone, refused jointly, atomic across a batch, an animated gesture
// and a preview, with one history entry per committed drag and cancel restoring
// the authored values.
// ---------------------------------------------------------------------------
TEST_F(ExtensionTest, FoldProducingMeshEditsAreRefusedAtomicallyIncludingGestures) {
    NEMO_REQUIRE_INSTALLED_PACKAGE(*this);
    const auto packages = openPackages();
    if (!packages)
        return;

    const auto initial = sample("knot0r1").input;
    auto chain = makeWarpChain(packages->contributions(), initial);
    ASSERT_TRUE(chain->failure.empty()) << chain->failure;
    const NetworkId network = chain->network;
    const NodeId warp = chain->warp;

    // +0.25 on one interior knot is admissible: its worst cell gives
    // 1.5 * (0.25 + 0.25) = 0.75 < 0.95.
    ASSERT_TRUE(submit(*chain, meshCommand(*chain, {{"hue0", 0.25}})).committed);
    ASSERT_TRUE(chain->session->undo(EditOptions{chain->session->revision(), {}}).committed);
    // The opposite displacement on the neighbouring spoke is admissible alone too.
    ASSERT_TRUE(submit(*chain, meshCommand(*chain, {{"hue1", -0.25}})).committed);
    ASSERT_TRUE(chain->session->undo(EditOptions{chain->session->revision(), {}}).committed);

    const std::uint64_t before = chain->session->revision();
    const bool undoableBefore = chain->session->canUndo();
    // Jointly they demand a 0.5 gradient across one cell in one component:
    // 1.5 * (0.5 + 0.25) = 1.125 >= 0.95, so the pair is refused as one unit.
    const EditResult folded = submit(*chain, meshCommand(*chain, {{"hue0", 0.25}, {"hue1", -0.25}}));
    EXPECT_FALSE(folded.committed);
    ASSERT_TRUE(folded.error.has_value());
    EXPECT_EQ(chain->session->revision(), before);
    EXPECT_EQ(chain->session->canUndo(), undoableBefore);
    expectPixelExactly(renderCpu(*chain), initial, "refused batch leaves the image unchanged");

    // The same fold cannot enter through animated effective parameters either.
    const ParameterGestureResult keyed = chain->session->beginKeyedParameterGesture(
        10.0, {{{network, warp, "hue0"}, 0.25}, {{network, warp, "hue1"}, -0.25}},
        EditOptions{chain->session->revision(), {}});
    EXPECT_TRUE(keyed.result.error.has_value());
    EXPECT_TRUE(chain->session->queryAnimationChannels().empty());

    // A refused preview never publishes: the last accepted preview survives it.
    ParameterGestureResult gesture = chain->session->beginParameterGesture({{{network, warp, "hue0"}, 0.25}},
                                                                           EditOptions{chain->session->revision(), {}});
    ASSERT_TRUE(gesture.snapshot);
    const ParameterGestureResult refused =
        chain->session->updateParameterGesture(gesture.token, {{{network, warp, "hue0"}, 0.9}});
    ASSERT_TRUE(refused.result.error.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(gesture.snapshot->network(network).graph().node(warp)->params.at("hue0")), 0.25);
    // Cancelling restores the authored state and publishes no history entry.
    const std::uint64_t gestureRevision = chain->session->revision();
    ASSERT_FALSE(chain->session->cancelParameterGesture(gesture.token).error);
    EXPECT_EQ(chain->session->revision(), gestureRevision);
    EXPECT_EQ(chain->session->canUndo(), undoableBefore);
    expectPixelExactly(renderCpu(*chain), initial, "cancel restores the image");

    // A committed drag is exactly one undoable entry, and undo/redo move the
    // authored mesh with it.
    gesture = chain->session->beginParameterGesture({{{network, warp, "hue0"}, 0.2}},
                                                    EditOptions{chain->session->revision(), {}});
    ASSERT_TRUE(gesture.snapshot);
    ASSERT_TRUE(
        chain->session->commitParameterGesture(gesture.token, EditOptions{chain->session->revision(), {}}).committed);
    EXPECT_TRUE(chain->session->canUndo());
    const auto committedPixels = renderCpu(*chain).pixel(0, 0);
    EXPECT_NE(committedPixels, initial);
    ASSERT_TRUE(chain->session->undo(EditOptions{chain->session->revision(), {}}).committed);
    expectPixelExactly(renderCpu(*chain), initial, "undo restores identity");
    ASSERT_TRUE(chain->session->redo(EditOptions{chain->session->revision(), {}}).committed);
    expectPixelExactly(renderCpu(*chain), committedPixels, "redo restores deformation");
}

// ---------------------------------------------------------------------------
// Supported animation: the authored coordinates key through the shared gesture,
// the channels are discoverable, evaluation at the keyed time uses the animated
// effective parameters, and the whole drag is one undo step.
// ---------------------------------------------------------------------------
TEST_F(ExtensionTest, AuthoredMeshCoordinatesAnimateThroughSharedHistoryAndEvaluateAtTime) {
    NEMO_REQUIRE_INSTALLED_PACKAGE(*this);
    const auto packages = openPackages();
    if (!packages)
        return;

    const WarpSample& row = sample("knot0r1");
    auto chain = makeWarpChain(packages->contributions(), row.input);
    ASSERT_TRUE(chain->failure.empty()) << chain->failure;
    const NetworkId network = chain->network;
    const NodeId warp = chain->warp;

    const ParameterGestureResult keyed = chain->session->beginKeyedParameterGesture(
        10.0, {{{network, warp, "hue0"}, 0.2}, {{network, warp, "saturation0"}, 0.2}},
        EditOptions{chain->session->revision(), {}});
    ASSERT_TRUE(keyed.snapshot) << describe(keyed.result);
    ASSERT_TRUE(
        chain->session->commitParameterGesture(keyed.token, EditOptions{chain->session->revision(), {}}).committed);

    std::vector<std::string> keys;
    for (const AnimationChannelQueryResult& channel : chain->session->queryAnimationChannels()) {
        EXPECT_GE(channel.keyCount, 1U);
        keys.push_back(channel.address.key);
    }
    EXPECT_NE(std::find(keys.begin(), keys.end(), "hue0"), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), "saturation0"), keys.end());

    // At the keyed time the animated effective parameters reproduce exactly the
    // static mesh's independent pixels.
    expectPixelNear(renderCpu(*chain, 10), row.expected, "animated mesh at t=10");

    // One history entry for the whole drag: undo removes both channels, redo
    // restores the animated behaviour.
    ASSERT_TRUE(chain->session->undo(EditOptions{chain->session->revision(), {}}).committed);
    EXPECT_TRUE(chain->session->queryAnimationChannels().empty());
    ASSERT_TRUE(chain->session->redo(EditOptions{chain->session->revision(), {}}).committed);
    EXPECT_EQ(chain->session->queryAnimationChannels().size(), 2U);
    expectPixelNear(renderCpu(*chain, 10), row.expected, "redo restores the animated mesh");

    // Pin is authored state on the same point and never reaches the renderer.
    ASSERT_TRUE(submit(*chain, setParamCommand(network, warp, "pin0", ParameterValue{true})).committed);
    expectPixelNear(renderCpu(*chain, 10), row.expected, "pin is not part of the deformation");
}

// ---------------------------------------------------------------------------
// Persistence and package absence: the authored mesh survives the public file
// layer, a build without the package retains the complete authored state, and
// restoring a compatible package recovers the behaviour.
// ---------------------------------------------------------------------------
TEST_F(ExtensionTest, SavedProjectKeepsAuthoredMeshAndRecoversWhenThePackageIsAbsent) {
    NEMO_REQUIRE_INSTALLED_PACKAGE(*this);
    const auto packages = openPackages();
    if (!packages)
        return;

    const std::shared_ptr<const NodeContributions> registry = packages->contributions();
    const WarpSample& row = sample("knot0r1");
    auto chain = makeWarpChain(registry, row.input);
    ASSERT_TRUE(chain->failure.empty()) << chain->failure;
    const NetworkId network = chain->network;
    const NodeId warp = chain->warp;

    ASSERT_TRUE(submit(*chain, setParametersCommand({{{network, warp, "hue11"}, -0.08},
                                                     {{network, warp, "hue24"}, 0.2},
                                                     {{network, warp, "pin0"}, ParameterValue{true}}}))
                    .committed);
    const ParameterGestureResult keyed = chain->session->beginKeyedParameterGesture(
        10.0, {{{network, warp, "hue0"}, 0.2}, {{network, warp, "saturation0"}, 0.2}},
        EditOptions{chain->session->revision(), {}});
    ASSERT_TRUE(keyed.snapshot) << describe(keyed.result);
    ASSERT_TRUE(
        chain->session->commitParameterGesture(keyed.token, EditOptions{chain->session->revision(), {}}).committed);
    expectPixelNear(renderCpu(*chain, 10), row.expected, "authored state before saving");

    // The real project file: write, commit, reopen against the same catalog.
    const fs::path target = workspace() / "colorwarp.nemo";
    const ProjectWriteRequest job = chain->session->prepareSave(target);
    const ProjectWriteResult written = ProjectFile::writeAtomic(job);
    ASSERT_TRUE(written.ok) << written.error.message;
    ASSERT_TRUE(chain->session->commitSave(job, written).committed);

    ProjectReadResult read = ProjectFile::read(target, registry->catalog());
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_TRUE(read.warnings.empty());
    ProjectSession reopened(Document(registry->catalog()), 256, registry);
    ASSERT_TRUE(reopened.open(std::move(read)).replaced);
    const NodeInstance* reopenedNode = reopened.document().network(network).graph().node(warp);
    ASSERT_NE(reopenedNode, nullptr);
    EXPECT_EQ(reopenedNode->type, kColorWarpType);
    ASSERT_TRUE(reopenedNode->params.contains("pin0"));
    EXPECT_EQ(reopenedNode->params.at("pin0"), ParameterValue{true});
    ASSERT_TRUE(reopenedNode->params.contains("hue11"));
    EXPECT_EQ(reopenedNode->params.at("hue11"), ParameterValue{-0.08});
    EXPECT_EQ(reopened.queryAnimationChannels().size(), 2U);
    ASSERT_EQ(reopened.document().network(network).defaultOutput(), chain->output);
    const CpuImage reopenedPixels =
        evaluateCpu(reopened.document(), requestFor(*chain, 10), nullptr, nullptr, registry).image;
    expectPixelNear(reopenedPixels, row.expected, "reopened project at t=10");

    // Without the package the type is unavailable, the whole authored state is
    // retained, and the file round-trips unchanged.
    const nlohmann::json saved = saveDocument(chain->session->document());
    const LoadResult absent = loadDocument(saved, builtinNodeCatalogPtr());
    EXPECT_TRUE(contains(absent.warnings, kColorWarpType)) << "the unavailable type must be reported";
    const NodeInstance* retained = absent.document.network(network).graph().nodeByName("ColorWarp");
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(retained->type, kColorWarpType);
    EXPECT_TRUE(retainsParameter(*retained, "hue24", ParameterValue{0.2}));
    EXPECT_TRUE(retainsParameter(*retained, "pin0", ParameterValue{true}));
    EXPECT_EQ(absent.document.animationChannels().size(), 2U);
    EXPECT_EQ(saveDocument(absent.document), saved) << "package absence must not lose authored state";

    // Restoring a compatible package recovers the behaviour, not just the values.
    const LoadResult restored = loadDocument(saved, registry->catalog());
    EXPECT_TRUE(restored.warnings.empty());
    EXPECT_EQ(restored.document.animationChannels().size(), 2U);
    const CpuImage restoredPixels =
        evaluateCpu(restored.document, requestFor(*chain, 10), nullptr, nullptr, registry).image;
    expectPixelNear(restoredPixels, row.expected, "restored package");
}

TEST_F(ExtensionTest, DataRgbAndPremultipliedSamplesKeepTheirDeclaredMeaning) {
    NEMO_REQUIRE_INSTALLED_PACKAGE(*this);
    const auto packages = openPackages();
    ASSERT_TRUE(packages.has_value());
    const auto& reference = sample("knot0r1_signed");
    for (int mode = 0; mode < 3; ++mode) {
        SCOPED_TRACE(mode);
        // The real constant-image producer supplies pixels; this fixture only
        // declares their interpretation, as an external source provider would.
        const auto installed = packages->contributions()->entries();
        std::vector<NodeContribution> entries(installed.begin(), installed.end());
        for (auto& entry : entries) {
            if (entry.descriptor.type != "constcolor")
                continue;
            entry.describe = [mode](const NodeDescriptionContext& context) {
                auto description = context.inherited;
                description.color = mode == 0 ? ColorInterpretation::Data : ColorInterpretation::SceneLinear;
                description.association = mode == 0 ? ImageAssociation::Straight : ImageAssociation::Premultiplied;
                return description;
            };
        }
        const auto registry = std::make_shared<const NodeContributions>(std::move(entries));
        auto input = reference.input;
        auto expected = reference.expected;
        if (mode == 0) {
            expected = input;
        } else if (mode == 1) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                input[channel] *= input[3];
                expected[channel] *= expected[3];
            }
        } else {
            input[3] = 0.0F;
            expected = input;
        }
        auto chain = makeWarpChain(registry, input);
        ASSERT_TRUE(chain->failure.empty()) << chain->failure;
        ASSERT_TRUE(submit(*chain, analyticMeshCommand(*chain)).committed);
        if (mode == 1)
            expectPixelNear(renderCpu(*chain), expected, "explicit premultiplication");
        else
            expectPixelExactly(renderCpu(*chain), expected, "data or zero-alpha preservation");
    }
}

TEST_F(ExtensionTest, ActivationRefusesUnsafeFilesAndFailedDependenciesWithoutHidingSiblings) {
    NEMO_REQUIRE_INSTALLED_PACKAGE(*this);
    const fs::path root = workspace() / "admission";
    (void)copyPackage(root, "supported");
    const auto parent = copyPackage(root, "z-parent");
    mutateManifest(parent, "org.nemo.parent", [](nlohmann::json&) {});
    const auto child = copyPackage(root, "a-child");
    mutateManifest(child, "org.nemo.child",
                   [](nlohmann::json& manifest) { manifest["dependencies"] = {"org.nemo.parent"}; });
    for (const auto& broken : {parent, child}) {
        const auto library = readManifest(broken).at("library").get<std::string>();
        std::ofstream stream(broken / library, std::ios::binary | std::ios::trunc);
        stream << "not a native library";
    }
    const auto collision = copyPackage(root, "editor-collision");
    mutateManifest(collision, "org.nemo.collision", [](nlohmann::json& manifest) {
        manifest["editors"][0]["id"] = "nemo.crop.box";
        for (auto& parameter : manifest["node"]["parameters"]) {
            if (parameter.contains("editor"))
                parameter["editor"] = "nemo.crop.box";
        }
    });
#if !defined(_WIN32)
    const auto escape = copyPackage(root, "escaped-library");
    mutateManifest(escape, "org.nemo.escape", [](nlohmann::json&) {});
    const auto library = readManifest(escape).at("library").get<std::string>();
    fs::remove(escape / library);
    fs::create_symlink(packageDir() / library, escape / library);
#endif
    nemo::extensions::InstalledPackages packages({root});
    const auto& diagnostics = packages.diagnostics();
    EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(), [](const std::string& message) {
        return message.find("org.nemo.child") != std::string::npos &&
               message.find("org.nemo.parent") != std::string::npos;
    })) << "the child must be refused for its failed dependency before attempting its own native library";
    EXPECT_TRUE(contains(diagnostics, "nemo.crop.box"));
    EXPECT_EQ(packages.contributions()->find("org.nemo.parent"), nullptr);
    EXPECT_EQ(packages.contributions()->find("org.nemo.child"), nullptr);
    EXPECT_EQ(packages.contributions()->find("org.nemo.collision"), nullptr);
#if !defined(_WIN32)
    EXPECT_TRUE(contains(diagnostics, "org.nemo.escape"));
    EXPECT_EQ(packages.contributions()->find("org.nemo.escape"), nullptr);
#endif
    auto chain = makeWarpChain(packages.contributions(), sample("knot0r1").input);
    ASSERT_TRUE(chain->failure.empty()) << chain->failure;
    ASSERT_TRUE(submit(*chain, analyticMeshCommand(*chain)).committed);
    expectPixelNear(renderCpu(*chain), sample("knot0r1").expected, "unrelated supported package");
}

// ---------------------------------------------------------------------------
// Compatibility refusals: an incompatible API range, a missing dependency (and
// its transitive dependants), an unreadable manifest and a missing declared file
// are diagnostics, and a supported package stays usable beside them. A duplicate
// identity refuses every offender instead of letting discovery order decide.
// ---------------------------------------------------------------------------
TEST_F(ExtensionTest, RefusalDiagnosticsNeverHideTheSupportedPackage) {
    NEMO_REQUIRE_INSTALLED_PACKAGE(*this);

    const fs::path mixed = workspace() / "mixed";
    const fs::path supported = copyPackage(mixed, "supported");
    const fs::path badApi = copyPackage(mixed, "badapi");
    mutateManifest(badApi, "org.nemo.colorwarp.badapi",
                   [](nlohmann::json& manifest) { manifest["api"]["maximum"] = 0; });
    const fs::path depApi = copyPackage(mixed, "depapi");
    mutateManifest(depApi, "org.nemo.colorwarp.depapi",
                   [](nlohmann::json& manifest) { manifest["api"]["maximum"] = 0; });
    const fs::path needsDep = copyPackage(mixed, "needsdep");
    mutateManifest(needsDep, "org.nemo.colorwarp.needsdep",
                   [](nlohmann::json& manifest) { manifest["dependencies"] = {"org.nemo.colorwarp.depapi"}; });
    const fs::path noLibrary = copyPackage(mixed, "nolib");
    mutateManifest(noLibrary, "org.nemo.colorwarp.nolib",
                   [](nlohmann::json& manifest) { manifest["library"] = "libnemo_colorwarp_missing.so"; });
    // A namespaced but unsupported GPU binding contract refuses the WHOLE package
    // before activation instead of downgrading it to a CPU-only node.
    const fs::path badBindings = copyPackage(mixed, "badbinding");
    mutateManifest(badBindings, "org.nemo.colorwarp.badbinding",
                   [](nlohmann::json& manifest) { manifest["gpu"]["bindings"] = "nemo.native.bindings.v1"; });
    const fs::path unreadable = mixed / "unreadable";
    ASSERT_TRUE(fs::create_directories(unreadable));
    {
        std::ofstream stream(unreadable / "manifest.json", std::ios::trunc);
        stream << "{ this is not a manifest";
    }
    ASSERT_TRUE(fs::exists(supported / "manifest.json"));

    nemo::extensions::InstalledPackages packages({mixed});
    const std::vector<std::string>& diagnostics = packages.diagnostics();
    EXPECT_EQ(diagnostics.size(), 6U);
    for (const char* identity :
         {"org.nemo.colorwarp.badapi", "org.nemo.colorwarp.depapi", "org.nemo.colorwarp.needsdep",
          "org.nemo.colorwarp.nolib", "org.nemo.colorwarp.badbinding", "unreadable"}) {
        EXPECT_TRUE(contains(diagnostics, identity)) << "no refusal names " << identity;
    }

    // The supported sibling is fully usable and the refused ones contribute
    // nothing at all — in particular the binding mismatch leaves NO node, so an
    // unsupported GPU contract can never degrade into a CPU-only contribution.
    const std::shared_ptr<const NodeContributions> registry = packages.contributions();
    ASSERT_NE(registry->find(kColorWarpType), nullptr);
    EXPECT_EQ(registry->find("org.nemo.colorwarp.badapi"), nullptr);
    EXPECT_EQ(registry->find("org.nemo.colorwarp.needsdep"), nullptr);
    EXPECT_EQ(registry->find("org.nemo.colorwarp.badbinding"), nullptr) << "the whole package must be refused";
    EXPECT_EQ(registry->catalog()->find("org.nemo.colorwarp.badbinding"), nullptr);
    EXPECT_EQ(registry->find("org.nemo.colorwarp.nolib"), nullptr);
    ASSERT_EQ(packages.panels().size(), 1U);
    EXPECT_EQ(packages.panels().front().id, kPanelId) << "a refused package contributes no panel";
    auto chain = makeWarpChain(registry, sample("knot0r1").input);
    ASSERT_TRUE(chain->failure.empty()) << chain->failure;
    ASSERT_TRUE(submit(*chain, analyticMeshCommand(*chain)).committed);
    expectPixelNear(renderCpu(*chain), sample("knot0r1").expected, "supported package beside refusals");

    // Two copies of one identity: every offender is refused, and the process is
    // not failed for it.
    const fs::path duplicated = workspace() / "duplicated";
    const fs::path first = copyPackage(duplicated, "first");
    const fs::path second = copyPackage(duplicated, "second");
    ASSERT_TRUE(fs::exists(first / "manifest.json"));
    ASSERT_TRUE(fs::exists(second / "manifest.json"));
    nemo::extensions::InstalledPackages duplicates({duplicated});
    EXPECT_EQ(duplicates.diagnostics().size(), 2U);
    EXPECT_EQ(duplicates.contributions()->find(kColorWarpType), nullptr);
    EXPECT_TRUE(duplicates.panels().empty());
    EXPECT_NE(duplicates.contributions()->find("constcolor"), nullptr);
}

// ---------------------------------------------------------------------------
// Registration identity vs authored state: a changed package version is a new
// implementation identity, while the same declared state version keeps an
// existing document's state compatible.
// ---------------------------------------------------------------------------
TEST_F(ExtensionTest, RegistrationIdentityFollowsTheDeclaredPackageVersionWhileStateStaysCompatible) {
    NEMO_REQUIRE_INSTALLED_PACKAGE(*this);

    const fs::path root = workspace() / "versioned";
    const fs::path upgraded = copyPackage(root, "colorwarp");
    {
        nlohmann::json manifest = readManifest(upgraded);
        manifest["version"] = 2;
        manifest["processingVersion"] = 2;
        writeManifest(upgraded, manifest);
    }

    nemo::extensions::InstalledPackages original({installedRoot()});
    nemo::extensions::InstalledPackages changed({root});
    ASSERT_TRUE(changed.diagnostics().empty());
    const NodeContribution* before = original.contributions()->find(kColorWarpType);
    const NodeContribution* after = changed.contributions()->find(kColorWarpType);
    ASSERT_NE(before, nullptr);
    ASSERT_NE(after, nullptr);

    // A changed implementation is a different registration identity, so no cached
    // pixel of the previous version can be served for it...
    EXPECT_NE(before->descriptor.implementationVersion, after->descriptor.implementationVersion);
    EXPECT_NE(original.contributions()->fingerprint(), changed.contributions()->fingerprint());
    // ...while the authored-state contract is a separate fact: the same declared
    // state version keeps existing documents available.
    EXPECT_EQ(before->descriptor.stateIdentity, after->descriptor.stateIdentity);
    EXPECT_EQ(before->descriptor.parameters.size(), after->descriptor.parameters.size());

    auto authored = makeWarpChain(original.contributions(), sample("knot0r1").input);
    ASSERT_TRUE(authored->failure.empty()) << authored->failure;
    ASSERT_TRUE(submit(*authored, analyticMeshCommand(*authored)).committed);
    const nlohmann::json saved = saveDocument(authored->session->document());

    // The saved document opens against the upgraded package without a state
    // warning, and the upgraded implementation renders the same contract.
    const LoadResult loaded = loadDocument(saved, changed.contributions()->catalog());
    EXPECT_TRUE(loaded.warnings.empty());
    EvaluationRequest request = requestFor(*authored);
    request.output = loaded.document.network(authored->network).defaultOutput();
    const CpuImage pixels = evaluateCpu(loaded.document, request, nullptr, nullptr, changed.contributions()).image;
    expectPixelNear(pixels, sample("knot0r1").expected, "upgraded package, same authored state");
}

#if defined(NEMO_TEST_GPU)
namespace {

// --- native bootstrap (the shared pattern of the effect suites) -------------
enum class BootstrapOutcome { Created, NoDevice, Failed };
struct Bootstrap {
    std::unique_ptr<gpu::Instance> instance;
    std::unique_ptr<gpu::Device> device;
    std::unique_ptr<gpu::Allocator> allocator;
    BootstrapOutcome outcome = BootstrapOutcome::Created;
    std::string message;
};

[[nodiscard]] Bootstrap createBootstrap() {
    Bootstrap boot;
    try {
        boot.instance = gpu::Instance::create({.validation = true});
        boot.device = gpu::Device::create(*boot.instance);
        boot.allocator = gpu::Allocator::create(*boot.instance, *boot.device, {.max_device_bytes = 128U << 20});
    } catch (const gpu::GpuException& error) {
        boot.outcome =
            error.errorCode() == gpu::GpuError::NoDevice ? BootstrapOutcome::NoDevice : BootstrapOutcome::Failed;
        boot.message = error.what();
    }
    return boot;
}

#define NEMO_SKIP_OR_FAIL(boot)                                                                                        \
    do {                                                                                                               \
        if ((boot).outcome == BootstrapOutcome::NoDevice)                                                              \
            GTEST_SKIP() << (boot).message;                                                                            \
        if ((boot).outcome == BootstrapOutcome::Failed) {                                                              \
            ADD_FAILURE() << "device creation failed: " << (boot).message;                                             \
            return;                                                                                                    \
        }                                                                                                              \
    } while (false)

void expectValidationClean(gpu::Instance& instance) {
    if (!instance.validation_enabled())
        return;
    std::string collected;
    bool hasWarnings = false;
    for (const auto& message : instance.take_debug_messages()) {
        collected += message.text + "\n";
        hasWarnings = hasWarnings || message.severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    }
    EXPECT_FALSE(hasWarnings) << "validation-layer messages:\n" << collected;
}

// The host's compiled built-in kernels, when this build configured them.
[[nodiscard]] fs::path hostKernelDirectory() {
#if defined(NEMO_SLANG_SPV_DIR)
    return fs::path{NEMO_SLANG_SPV_DIR};
#else
    return {};
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// CPU and GPU against the same independent pixels: the native front ends are
// compared to the literals, never to each other alone.
// ---------------------------------------------------------------------------
TEST_F(ExtensionTest, ColorWarpCpuAndGpuMatchTheIndependentMapping) {
    NEMO_REQUIRE_INSTALLED_PACKAGE(*this);
    const auto packages = openPackages();
    if (!packages)
        return;

    const std::vector<eval::GpuNodeContribution> declarations = nemo::extensions::gpuContributions(*packages);
    const fs::path spvDir = hostKernelDirectory();
    const eval::EffectLibrary slang(declarations, eval::EffectBackend::Slang, spvDir);
    const eval::EffectLibrary glsl(declarations, eval::EffectBackend::Glsl);
    const auto usable = [](const eval::EffectLibrary& library) {
        const eval::RegisteredGpuEffect* warp = library.find(kColorWarpType);
        const eval::RegisteredGpuEffect* plate = library.find("constcolor");
        return warp != nullptr && plate != nullptr && warp->unavailableReason.empty() &&
               plate->unavailableReason.empty();
    };
    // The Slang half needs the host's compiled built-in kernels for the plate; the
    // GLSL half needs only a GLSL front end, because the package carries its shader.
    const bool slangReady = !spvDir.empty() && usable(slang);
    const bool glslReady = usable(glsl);
    if (!slangReady && !glslReady)
        GTEST_SKIP() << "no usable native effect front end for this build";

    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);

    // The document is built from the SAME registration the library published, so
    // the schema and the native programs cannot disagree.
    auto chain = makeWarpChain(slangReady ? slang.contributions() : glsl.contributions(), kWarpSamples.front().input);
    ASSERT_TRUE(chain->failure.empty()) << chain->failure;
    ASSERT_TRUE(submit(*chain, analyticMeshCommand(*chain)).committed);

    for (const WarpSample& row : kWarpSamples) {
        // The CPU reference for the same document and demand.
        {
            const bool authored = submit(*chain, setParamCommand(chain->network, chain->plate, "color",
                                                                 ParameterValue{ColorValue{row.input}}))
                                      .committed;
            EXPECT_TRUE(authored) << row.name;
            expectPixelNear(renderCpu(*chain), row.expected, std::string{"cpu "} + row.name);
        }
        const auto renderNative = [&](const eval::EffectLibrary& library, const char* frontEnd) {
            const bool authored = submit(*chain, setParamCommand(chain->network, chain->plate, "color",
                                                                 ParameterValue{ColorValue{row.input}}))
                                      .committed;
            EXPECT_TRUE(authored) << row.name;
            eval::GpuEvaluation evaluated = eval::evaluateGpu(chain->session->document(), requestFor(*chain), library,
                                                              *boot.device, *boot.allocator);
            expectPixelNear(evaluated.readBack(chain->output, *boot.device, *boot.allocator), row.expected,
                            std::string{frontEnd} + " " + row.name);
        };
        if (slangReady)
            renderNative(slang, "slang");
        if (glslReady)
            renderNative(glsl, "glsl");
    }
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Ownership: installed adapters and the native library outlive the loader that
// produced them, and an in-flight submission keeps the library snapshot alive
// until its completion retires it.
// ---------------------------------------------------------------------------
TEST_F(ExtensionTest, InstalledCallbacksAndLibraryOutliveTheLoaderAndNativeWork) {
    NEMO_REQUIRE_INSTALLED_PACKAGE(*this);
    // The host's compiled kernels are a build fact, not a package fact: without
    // them the plate cannot run natively at all, which is a normal skip.
    const fs::path spvDir = hostKernelDirectory();
    if (spvDir.empty())
        GTEST_SKIP() << "the host's compiled kernels are not configured for this build";
    {
        const auto packages = openPackages();
        ASSERT_TRUE(packages.has_value());
        const auto declarations = nemo::extensions::gpuContributions(*packages);
        // The plate needs a host kernel, and the chain must be sound, before any
        // submission is gated: a failure here returns without a blocked queue.
        const eval::EffectLibrary probe(declarations, eval::EffectBackend::Slang, spvDir);
        const eval::RegisteredGpuEffect* builtin = probe.find("constcolor");
        if (builtin == nullptr || !builtin->unavailableReason.empty())
            GTEST_SKIP() << "the built-in plate has no native kernel in this build";
        const auto wired = makeWarpChain(probe.contributions(), sample("knot0r1").input);
        ASSERT_TRUE(wired->failure.empty()) << wired->failure;
        ASSERT_TRUE(submit(*wired, analyticMeshCommand(*wired)).committed);
    }

    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    gpu::SubmissionQueue& queue = boot.device->submissions(boot.device->graphics_family());

    // Gate the queue so the submission is provably in flight while the loader and
    // the library are destroyed.
    for (const bool abandon : {false, true}) {
        SCOPED_TRACE(abandon ? "cancelled consumer" : "completed consumer");
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        info.pNext = &type;
        VkSemaphore gate{};
        gpu::checkVulkan(vkCreateSemaphore(boot.device->handle(), &info, nullptr, &gate), "vkCreateSemaphore");
        const auto release = [&] {
            VkSemaphoreSignalInfo signal{};
            signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
            signal.semaphore = gate;
            signal.value = 1;
            gpu::checkVulkan(vkSignalSemaphore(boot.device->handle(), &signal), "vkSignalSemaphore");
            queue.drain();
            vkDestroySemaphore(boot.device->handle(), gate, nullptr);
        };
        const WarpSample& row = sample("knot0r1");
        try {
            gpu::SubmissionQueue::TimelineSemaphores waits;
            waits.wait = {gate};
            waits.waitValues = {1};
            if (!queue.submit([](VkCommandBuffer) {}, {}, waits))
                throw std::runtime_error("could not establish the native completion gate");

            std::weak_ptr<const void> observed;
            std::optional<eval::GpuEvaluation> pending;
            NodeId output{kInvalidNode};
            {  // The loader, the session and the library all die here.
                nemo::extensions::InstalledPackages owner({installedRoot()});
                const auto declarations = nemo::extensions::gpuContributions(owner);
                const eval::EffectLibrary library(declarations, eval::EffectBackend::Slang, spvDir);
                observed = library.retain();
                auto chain = makeWarpChain(library.contributions(), row.input);
                EXPECT_TRUE(chain->failure.empty()) << chain->failure;
                EXPECT_TRUE(submit(*chain, analyticMeshCommand(*chain)).committed);
                output = chain->output;
                pending = eval::submitGpu(chain->session->document(), requestFor(*chain), library, *boot.device,
                                          *boot.allocator);
            }
            EXPECT_FALSE(observed.expired()) << "an in-flight submission must retain its registration";
            EXPECT_FALSE(queue.poll(pending->completion.value()));
            if (abandon)
                pending.reset();
            EXPECT_FALSE(observed.expired()) << "abandoning a result must not unload in-flight code";
            release();
            gate = VK_NULL_HANDLE;
            EXPECT_TRUE(observed.expired()) << "completion retires the retained registration";
            if (pending)
                expectPixelNear(pending->readBack(output, *boot.device, *boot.allocator), row.expected,
                                "native submission survives the package owner");
        } catch (...) {
            if (gate != VK_NULL_HANDLE)
                release();
            throw;
        }
    }
    expectValidationClean(*boot.instance);

    // The CPU adapter taken from the same package keeps working after the loader
    // is gone: evaluating calls straight into the installed library.
    std::shared_ptr<const NodeContributions> retainedRegistry;
    std::unique_ptr<WarpChain> chain;
    {
        nemo::extensions::InstalledPackages owner({installedRoot()});
        retainedRegistry = owner.contributions();
        chain = makeWarpChain(retainedRegistry, sample("halfcell").input);
        ASSERT_TRUE(chain->failure.empty()) << chain->failure;
        ASSERT_TRUE(submit(*chain, analyticMeshCommand(*chain)).committed);
    }
    EXPECT_NE(retainedRegistry->find(kColorWarpType), nullptr);
    expectPixelNear(renderCpu(*chain), sample("halfcell").expected, "cpu adapter outlives the loader");
}

// ---------------------------------------------------------------------------
// Delivery over the queue's OWN installed inventory (issue #37): the raster a
// preflight describes and the frame a job delivers resolve through the same
// registration, so an installed chain feeding a Write is described, rendered and
// checked against the published mapping's independent pixels. A queue that does
// not carry that registration refuses the same chain by name instead of
// describing a graph it cannot render.
// ---------------------------------------------------------------------------
namespace {

constexpr int kDeliveryWidth{3};
constexpr int kDeliveryHeight{2};

// The installed chain a delivery test submits: plate -> ColorWarp -> Write,
// authored through the ordinary session commands (the path the inspector uses)
// on a canvas small enough to check every delivered pixel.
struct InstalledDeliveryChain {
    std::unique_ptr<ProjectSession> session;
    NetworkId network{kInvalidNetwork};
    NodeId write{kInvalidNode};
    std::string failure;
};

[[nodiscard]] EditResult submitTo(ProjectSession& session, Command command) {
    return session.submit(std::move(command), EditOptions{session.revision(), {}});
}

[[nodiscard]] std::unique_ptr<InstalledDeliveryChain>
makeInstalledDeliveryChain(std::shared_ptr<const NodeContributions> registry,
                           const std::array<float, kImageChannels>& plateColor, const std::string& outputFile) {
    auto chain = std::make_unique<InstalledDeliveryChain>();
    Document document(registry->catalog());
    document.network(document.rootNetworkId())
        .setFormat(ImageFormat{.width = kDeliveryWidth, .height = kDeliveryHeight});
    chain->session = std::make_unique<ProjectSession>(std::move(document), 256, std::move(registry));
    chain->network = chain->session->document().rootNetworkId();

    auto created = std::make_shared<NodeId>();
    EditResult result = submitTo(*chain->session, addNodeCommand(chain->network, "constcolor", "plate", created));
    const NodeId plate = created ? *created : kInvalidNode;
    if (!result.committed) {
        chain->failure = "cannot create the plate: " + describe(result);
        return chain;
    }
    if (!submitTo(*chain->session,
                  setParamCommand(chain->network, plate, "color", ParameterValue{ColorValue{plateColor}}))
             .committed) {
        chain->failure = "cannot author the plate color";
        return chain;
    }

    created = std::make_shared<NodeId>();
    result = submitTo(*chain->session, addNodeCommand(chain->network, kColorWarpType, "ColorWarp", created));
    const NodeId warp = created ? *created : kInvalidNode;
    if (!result.committed) {
        chain->failure = std::string{"cannot create "} + kColorWarpType + ": " + describe(result);
        return chain;
    }
    // The same admissible mesh the independent sample literals were computed for.
    std::vector<ParameterEdit> mesh;
    mesh.push_back({{chain->network, warp, "hue0"}, ParameterValue{0.2}});
    mesh.push_back({{chain->network, warp, "saturation0"}, ParameterValue{0.2}});
    mesh.push_back({{chain->network, warp, "hue11"}, ParameterValue{-0.08}});
    mesh.push_back({{chain->network, warp, "hue24"}, ParameterValue{0.2}});
    result = submitTo(*chain->session, setParametersCommand(std::move(mesh)));
    if (!result.committed) {
        chain->failure = "cannot author the mesh: " + describe(result);
        return chain;
    }
    if (!submitTo(*chain->session, connectCommand(chain->network, {plate, 0}, {warp, 0})).committed) {
        chain->failure = "cannot wire the plate into the installed node";
        return chain;
    }

    created = std::make_shared<NodeId>();
    result = submitTo(*chain->session, addNodeCommand(chain->network, "write", "deliver", created));
    chain->write = created ? *created : kInvalidNode;
    if (!result.committed) {
        chain->failure = "cannot create the Write node: " + describe(result);
        return chain;
    }
    if (!submitTo(*chain->session, setParamCommand(chain->network, chain->write, "file", ParameterValue{outputFile}))
             .committed) {
        chain->failure = "cannot author the delivery file pattern";
        return chain;
    }
    // The delivered storage must be stated exactly: a half-precision file would
    // quantize the independent pixels this test compares the file against.
    if (!submitTo(*chain->session,
                  setParamCommand(chain->network, chain->write, "precision", ParameterValue{ChoiceValue{"float"}}))
             .committed) {
        chain->failure = "cannot author the delivery precision";
        return chain;
    }
    if (!submitTo(*chain->session, connectCommand(chain->network, {warp, 0}, {chain->write, 0})).committed) {
        chain->failure = "cannot wire the installed node into the Write node";
        return chain;
    }
    return chain;
}

}  // namespace

TEST_F(ExtensionTest, InstalledChainIsDescribedAndDeliveredByTheQueuesOwnInventory) {
    NEMO_REQUIRE_INSTALLED_PACKAGE(*this);
    const fs::path spvDir = hostKernelDirectory();
    if (spvDir.empty())
        GTEST_SKIP() << "the host's compiled kernels are not configured for this build";
    const auto packages = openPackages();
    if (!packages)
        return;

    const std::vector<eval::GpuNodeContribution> declarations = nemo::extensions::gpuContributions(*packages);
    {
        // Both the host plate and the installed effect must have a usable native
        // implementation; otherwise this configuration cannot deliver the chain
        // at all, which is a normal skip.
        const eval::EffectLibrary probe(declarations, eval::EffectBackend::Slang, spvDir);
        const eval::RegisteredGpuEffect* plate = probe.find("constcolor");
        const eval::RegisteredGpuEffect* warp = probe.find(kColorWarpType);
        if (plate == nullptr || !plate->unavailableReason.empty() || warp == nullptr ||
            !warp->unavailableReason.empty())
            GTEST_SKIP() << "no usable native front end for the installed chain in this build";
    }

    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);

    const WarpSample& row = sample("knot0r1");
    const std::string still = (workspace() / "installed.exr").string();
    auto chain = makeInstalledDeliveryChain(packages->contributions(), row.input, still);
    ASSERT_TRUE(chain->failure.empty()) << chain->failure;

    // The preflight of a graph carrying an installed node reports the raster the
    // job would deliver: the describing inventory IS the queue's own list, so the
    // installed node's registration is found and its inherited description
    // resolved instead of the plan refusing a type it could not see.
    eval::DeliveryQueue queue(*boot.instance, *boot.device, *boot.allocator, spvDir, 8, declarations);
    const eval::DeliveryPlan planned = queue.plan(chain->session->document(), chain->network, chain->write, 0);
    ASSERT_TRUE(planned.ok()) << planned.problem;
    EXPECT_EQ(planned.width, kDeliveryWidth);
    EXPECT_EQ(planned.height, kDeliveryHeight);
    EXPECT_EQ(planned.channels, (std::vector<std::string>{"R", "G", "B", "A"}));
    EXPECT_FALSE(planned.movie);

    // The delivered frame carries the published mapping's pixels, asserted
    // against the independent literals rather than against the effect itself.
    const std::uint64_t id = queue.submit(chain->session->document(), chain->network, chain->write, 0);
    queue.waitForIdle();
    const eval::DeliveryJobInfo info = queue.status(id);
    EXPECT_EQ(info.state, eval::DeliveryState::Completed) << info.error;
    EXPECT_EQ(info.writtenFrames, 1U);
    EXPECT_EQ(info.failedFrames, 0U);
    EXPECT_EQ(info.width, kDeliveryWidth);
    EXPECT_EQ(info.height, kDeliveryHeight);
    ASSERT_EQ(info.files.size(), 1U);
    EXPECT_TRUE(info.files.front().written);
    EXPECT_EQ(info.files.front().path, still);

    const media::ImageReadResult delivered = media::readImage(still);
    ASSERT_EQ(delivered.image.width(), kDeliveryWidth);
    ASSERT_EQ(delivered.image.height(), kDeliveryHeight);
    EXPECT_EQ(delivered.header.channelNames, (std::vector<std::string>{"R", "G", "B", "A"}));
    for (int y = 0; y < delivered.image.height(); ++y) {
        for (int x = 0; x < delivered.image.width(); ++x) {
            const std::array<float, kImageChannels> actual = delivered.image.pixel(x, y);
            for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
                const double allowed = kTolerance + kTolerance * std::abs(static_cast<double>(row.expected[channel]));
                EXPECT_NEAR(actual[channel], row.expected[channel], allowed)
                    << "delivered pixel (" << x << ", " << y << ") channel " << channel;
            }
        }
    }

    // A queue whose inventory does not carry the installed registration refuses
    // the SAME chain by name: nothing is described, no frame is claimed and no
    // file appears at the authored path.
    const std::string unregistered = (workspace() / "unregistered.exr").string();
    ASSERT_TRUE(
        submitTo(*chain->session, setParamCommand(chain->network, chain->write, "file", ParameterValue{unregistered}))
            .committed);
    eval::DeliveryQueue builtinOnly(*boot.instance, *boot.device, *boot.allocator, spvDir);
    const eval::DeliveryPlan refusal = builtinOnly.plan(chain->session->document(), chain->network, chain->write, 0);
    EXPECT_FALSE(refusal.ok());
    EXPECT_NE(refusal.problem.find(kColorWarpType), std::string::npos) << refusal.problem;
    const std::uint64_t refused = builtinOnly.submit(chain->session->document(), chain->network, chain->write, 0);
    builtinOnly.waitForIdle();
    const eval::DeliveryJobInfo rejected = builtinOnly.status(refused);
    EXPECT_EQ(rejected.state, eval::DeliveryState::Failed);
    EXPECT_EQ(rejected.writtenFrames, 0U);
    EXPECT_NE(rejected.error.find(kColorWarpType), std::string::npos) << rejected.error;
    EXPECT_FALSE(fs::exists(unregistered));
}
#endif  // NEMO_TEST_GPU
