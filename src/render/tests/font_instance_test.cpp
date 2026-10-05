#include "logger/logger.hpp"
#include "render/font/font_instance.hpp"
#include "render/font/font_loader.hpp"
#include "render/font/font_registry.hpp"
#include "render/surface/surface.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace miximus;

class FontLayout : public testing::Test
{
    std::unique_ptr<render::font_instance_s> font_;

  protected:
    render::font_instance_s& font() { return *font_; }

    void SetUp() override
    {
        if (!getlog("app")) {
            logger::init_loggers(spdlog::level::warn);
        }
        render::font_registry_s registry;
        const auto              info = registry.find_font_variant(render::get_default_font_name(), "Regular");
        if (!info.has_value()) {
            FAIL() << "Bundled font variant is missing";
        }
        ASSERT_FALSE(info->resource_path.empty());
        font_ = std::make_shared<render::font_loader_s>()->load_font(&*info);
        ASSERT_NE(font_, nullptr);
        font().set_size(24);
    }
};

void expect_metrics(const render::font_instance_s::line_metrics_s& actual,
                    const render::font_instance_s::line_metrics_s& expected)
{
    EXPECT_EQ(actual.advance, expected.advance);
    EXPECT_EQ(actual.has_ink, expected.has_ink);
    EXPECT_EQ(actual.ink_bounds, expected.ink_bounds);
}

TEST_F(FontLayout, MeasurementWrappingAndPaintingAgreeIncludingFinalGlyph)
{
    std::vector<render::surface_s::pixel_t> pixels(size_t{512} * 128);
    render::surface_s                       surface({512, 128}, pixels);
    for (const auto text : {U"W", U"WW", U"Hello", U"Hello ", U"AV To", U"jgy ÁÉ", U"Kamera — entré"}) {
        const auto measured = font().measure_line(text);
        const auto wrapped  = font().flow_line(text, std::numeric_limits<int>::max());
        const auto painted  = font().render_line(text, &surface, {32, 64});
        EXPECT_EQ(measured.text_length, std::u32string_view(text).size());
        EXPECT_EQ(wrapped.consumed_length, measured.consumed_length);
        expect_metrics(wrapped.metrics, measured.metrics);
        expect_metrics(painted.metrics, measured.metrics);
        EXPECT_GT(measured.metrics.advance.x, 0);
    }
    const auto one = font().measure_line(U"W");
    EXPECT_EQ(font().measure_line(U"WW").metrics.advance.x, 2 * one.metrics.advance.x);
}

TEST_F(FontLayout, SpacesAdvanceWithoutInkAndDoNotExpandInkBounds)
{
    const auto empty = font().measure_line(U"");
    EXPECT_EQ(empty.consumed_length, 0);
    EXPECT_FALSE(empty.metrics.has_ink);
    EXPECT_EQ(empty.metrics.advance, gpu::vec2i_t(0));

    const auto spaces = font().measure_line(U"   ");
    EXPECT_FALSE(spaces.metrics.has_ink);
    EXPECT_EQ(spaces.metrics.ink_bounds, gpu::recti_s{});
    EXPECT_GT(spaces.metrics.advance.x, 0);

    const auto a      = font().measure_line(U"A");
    const auto padded = font().measure_line(U" A ");
    EXPECT_EQ(padded.metrics.ink_bounds.size, a.metrics.ink_bounds.size);
    EXPECT_GT(padded.metrics.ink_bounds.pos.x, a.metrics.ink_bounds.pos.x);
    EXPECT_EQ(padded.metrics.ink_bounds, font().measure_line(U" A").metrics.ink_bounds);
    EXPECT_GT(padded.metrics.advance.x, a.metrics.advance.x);
}

TEST_F(FontLayout, WrapAcceptsExactFitsAndConsumesBreakWhitespaceSeparately)
{
    const auto word = font().measure_line(U"Hello");
    auto       line = font().flow_line(U"Hello", word.metrics.advance.x);
    EXPECT_EQ(line.text_length, 5);
    expect_metrics(line.metrics, word.metrics);

    line = font().flow_line(U"Hello   world", word.metrics.advance.x);
    EXPECT_EQ(line.text_length, 5);
    EXPECT_EQ(line.consumed_length, 8);
    expect_metrics(line.metrics, word.metrics);

    line = font().flow_line(U"Hello world", font().measure_line(U"Hello wo").metrics.advance.x);
    EXPECT_EQ(line.text_length, 5);
    EXPECT_EQ(line.consumed_length, 6);
    expect_metrics(line.metrics, word.metrics);
}

TEST_F(FontLayout, OverlongWordsSplitAndEveryNarrowLineMakesProgress)
{
    const int  width = font().measure_line(U"WW").metrics.advance.x;
    const auto split = font().flow_line(U"WWWW", width);
    EXPECT_EQ(split.text_length, 2);
    EXPECT_EQ(split.consumed_length, 2);
    EXPECT_EQ(split.metrics.advance.x, width);
    for (int limit : {-1, 0, 1}) {
        auto remaining = std::u32string_view(U"W W\nW");
        while (!remaining.empty()) {
            const auto line = font().flow_line(remaining, limit);
            ASSERT_GT(line.consumed_length, 0);
            ASSERT_LE(line.consumed_length, remaining.size());
            expect_metrics(line.metrics, font().measure_line(remaining.substr(0, line.text_length)).metrics);
            remaining.remove_prefix(line.consumed_length);
        }
    }
}

TEST_F(FontLayout, WrapAdjacentToNewlineDoesNotCreateAnExtraEmptyLine)
{
    for (const auto text : {U"W\nB", U"W  \r\nB"}) {
        const auto line = font().flow_line(text, 0);
        EXPECT_EQ(line.text_length, 1);
        EXPECT_EQ(std::u32string_view(text).substr(line.consumed_length), U"B");
    }
    const auto line = font().flow_line(U"Hello  \nB", font().measure_line(U"Hello").metrics.advance.x);
    EXPECT_EQ(line.text_length, 5);
    EXPECT_EQ(line.consumed_length, 8);
}

TEST_F(FontLayout, AllOperationsStopAtLineSeparatorsAndHandleTabsIdentically)
{
    std::vector<render::surface_s::pixel_t> pixels(size_t{256} * 128);
    render::surface_s                       surface({256, 128}, pixels);
    for (auto text : {U"A\nB", U"A\rB", U"A\r\nB"}) {
        const auto measured = font().measure_line(text);
        EXPECT_EQ(measured.text_length, 1);
        EXPECT_EQ(measured.consumed_length, std::u32string_view(text).size() - 1);
        expect_metrics(measured.metrics, font().measure_line(U"A").metrics);
        expect_metrics(font().flow_line(text, 500).metrics, measured.metrics);
        expect_metrics(font().render_line(text, &surface, {20, 64}).metrics, measured.metrics);
    }
    EXPECT_EQ(font().measure_line(U"\n").consumed_length, 1);
    EXPECT_EQ(font().measure_line(U"\n").text_length, 0);
    expect_metrics(font().measure_line(U"A\tB").metrics, font().measure_line(U"A B").metrics);
}

TEST_F(FontLayout, MeasuredInkEnclosesRenderedPixelsWithoutIncludingTheOrigin)
{
    std::vector<render::surface_s::pixel_t> pixels(size_t{512} * 128);
    render::surface_s                       surface({512, 128}, pixels);
    const gpu::vec2i_t                      baseline{32, 64};
    for (const auto text : {U"i", U"j", U"Ágj", U"AV To", U" W "}) {
        surface.clear({0, 0, 0, 0});
        const auto measured = font().measure_line(text);
        font().render_line(text, &surface, baseline);
        const auto ink_min = baseline + measured.metrics.ink_bounds.pos;
        const auto ink_max = ink_min + measured.metrics.ink_bounds.size;
        size_t     painted = 0;
        for (int y = 0; y < 128; ++y) {
            for (int x = 0; x < 512; ++x) {
                if (pixels[(static_cast<size_t>(y) * 512) + static_cast<size_t>(x)].a != 0) {
                    ++painted;
                    ASSERT_GE(x, ink_min.x);
                    ASSERT_LT(x, ink_max.x);
                    ASSERT_GE(y, ink_min.y);
                    ASSERT_LT(y, ink_max.y);
                }
            }
        }
        EXPECT_GT(painted, 0);
    }
    EXPECT_GT(font().measure_line(U" W").metrics.ink_bounds.pos.x, 0);
}

TEST_F(FontLayout, FontSizeChangesPreserveMetrics)
{
    const auto small = font().measure_line(U"Camera");
    font().set_size(48);
    EXPECT_GT(font().measure_line(U"Camera").metrics.advance.x, small.metrics.advance.x);
    font().set_size(24);
    expect_metrics(font().measure_line(U"Camera").metrics, small.metrics);
    EXPECT_THROW(font().set_size(0), std::invalid_argument);
    EXPECT_GT(font().line_height(), 0);
}

TEST(FontResources, AllBundledVariantsSurviveRegistryRefreshAndLoaderLifetime)
{
    if (!getlog("app")) {
        logger::init_loggers(spdlog::level::warn);
    }
    render::font_registry_s registry;
    EXPECT_EQ(render::get_default_font_name(), "Liberation Sans");
    for (const auto family : {"Liberation Sans", "Liberation Mono"}) {
        for (const auto style : {"Regular", "Bold", "Italic", "Bold Italic"}) {
            const auto info = registry.find_font_variant(family, style);
            if (!info.has_value()) {
                FAIL() << "Bundled font variant is missing";
            }
            ASSERT_FALSE(info->resource_path.empty());
            EXPECT_TRUE(info->path.empty());
            registry.refresh();
            auto font = std::make_shared<render::font_loader_s>()->load_font(&*info);
            ASSERT_NE(font, nullptr);
            font->set_size(24);
            EXPECT_GT(font->measure_line(U"Camera 01 — Åäö").metrics.advance.x, 0);
        }
    }
}

TEST(FontResources, MissingAndInvalidSourcesFailCleanly)
{
    auto loader = std::make_shared<render::font_loader_s>();
    EXPECT_EQ(loader->load_font(nullptr), nullptr);
    const render::font_variant_s missing_resource{.resource_path = "fonts/missing.ttf"};
    EXPECT_EQ(loader->load_font(&missing_resource), nullptr);
    const render::font_variant_s invalid_resource{.resource_path = "fonts/Liberation-LICENSE.txt"};
    EXPECT_EQ(loader->load_font(&invalid_resource), nullptr);
    const render::font_variant_s missing_file{.path = "missing-font.ttf"};
    EXPECT_EQ(loader->load_font(&missing_file), nullptr);
}

TEST(FontBitmapLayout, BitmapMetricsAndMonochromePaintingAgree)
{
    const auto              path = std::filesystem::path(__FILE__).parent_path() / "fonts" / "metrics.bdf";
    render::font_instance_s font(std::make_shared<render::font_loader_s>(), path, 0);
    ASSERT_TRUE(font.valid());
    font.set_size(9);
    const auto layout = font.measure_line(U" Wj ");
    EXPECT_EQ(layout.metrics.advance, gpu::vec2i_t(17, 0));
    const gpu::recti_s expected_bounds{
        .pos  = {4, -7},
        .size = {8, 9 },
    };
    EXPECT_EQ(layout.metrics.ink_bounds, expected_bounds);
    EXPECT_TRUE(layout.metrics.has_ink);
    std::vector<render::surface_s::pixel_t> pixels(size_t{32} * 32);
    render::surface_s                       surface({32, 32}, pixels);
    expect_metrics(font.render_line(U" Wj ", &surface, {4, 12}).metrics, layout.metrics);
    EXPECT_TRUE(std::ranges::any_of(pixels, [](auto pixel) { return pixel.a == 255; }));
    const auto wrapped = font.flow_line(U"Wj W", 9);
    EXPECT_EQ(wrapped.text_length, 2);
    EXPECT_EQ(wrapped.consumed_length, 3);
    EXPECT_EQ(wrapped.metrics.advance.x, 9);
}

} // namespace
