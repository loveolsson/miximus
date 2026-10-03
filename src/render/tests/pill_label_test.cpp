#include "logger/logger.hpp"
#include "render/font/font_instance.hpp"
#include "render/font/font_loader.hpp"
#include "render/font/font_registry.hpp"
#include "render/pill_label.hpp"
#include "render/surface/surface.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace {
using namespace miximus;

constexpr render::pill_style_s label_style{
    .font_size  = 24,
    .padding_x  = 12,
    .padding_y  = 6,
    .max_width  = 600,
    .max_height = 120,
};

class PillLabel : public testing::Test
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
        ASSERT_TRUE(info);
        ASSERT_FALSE(info->resource_path.empty());
        font_ = std::make_shared<render::font_loader_s>()->load_font(&*info);
        ASSERT_NE(font_, nullptr);
    }
};

TEST_F(PillLabel, MeasuresFinalGlyphAndKeepsAllInkInsidePadding)
{
    font().set_size(24);
    const auto single = font().measure_line(U"W");
    const auto pair   = font().measure_line(U"WW");
    EXPECT_GT(pair.metrics.ink_bounds.size.x, single.metrics.ink_bounds.size.x);

    for (const auto text : {"W", "Camera 01", "jgy ÁÉ", "Kamera — entré"}) {
        const auto label = render::make_pill_label(font(), text, label_style);
        ASSERT_FALSE(label.text.empty());
        EXPECT_EQ(label.font_size, 24);
        const auto bounds = font().measure_line(label.text).metrics.ink_bounds;
        const auto start  = label.baseline + bounds.pos;
        EXPECT_GE(start.x, label_style.padding_x);
        EXPECT_GE(start.y, label_style.padding_y);
        EXPECT_LE(start.x + bounds.size.x, label.dimensions.x - label_style.padding_x);
        EXPECT_LE(start.y + bounds.size.y, label.dimensions.y - label_style.padding_y);
    }
}

TEST_F(PillLabel, PaintsSeventyFivePercentBlackBackgroundAndOpaqueWhiteText)
{
    const auto                              label = render::make_pill_label(font(), "Camera 01", label_style);
    std::vector<render::surface_s::pixel_t> pixels(static_cast<size_t>(label.dimensions.x) *
                                                   static_cast<size_t>(label.dimensions.y));
    render::surface_s                       surface(label.dimensions, pixels);
    // Painting restores the size captured by layout, independent of previous font use.
    font().set_size(72);
    render::render_pill_label(font(), label, surface);
    EXPECT_EQ(pixels.front().a, 0);
    const bool has_background = std::ranges::any_of(
        pixels, [](auto pixel) { return pixel.r == 0 && pixel.g == 0 && pixel.b == 0 && pixel.a == 191; });
    const bool has_white_text = std::ranges::any_of(
        pixels, [](auto pixel) { return pixel.r == 255 && pixel.g == 255 && pixel.b == 255 && pixel.a == 255; });
    const bool is_premultiplied = std::ranges::all_of(
        pixels, [](auto pixel) { return pixel.r <= pixel.a && pixel.g <= pixel.a && pixel.b <= pixel.a; });
    EXPECT_TRUE(has_background);
    EXPECT_TRUE(has_white_text);
    EXPECT_TRUE(is_premultiplied);
}

TEST_F(PillLabel, EllipsizesUnicodeAndRejectsEmptyOrTooSmallLabels)
{
    auto narrow      = label_style;
    narrow.max_width = 120;
    const auto label = render::make_pill_label(font(), "Kamera — entré with a very long name", narrow);
    ASSERT_FALSE(label.text.empty());
    EXPECT_EQ(label.text.back(), U'\u2026');
    EXPECT_LE(label.dimensions.x, narrow.max_width);
    EXPECT_TRUE(render::make_pill_label(font(), "", label_style).text.empty());
    EXPECT_TRUE(render::make_pill_label(font(), "   ", label_style).text.empty());

    narrow.max_height = 4;
    EXPECT_TRUE(render::make_pill_label(font(), "Camera", narrow).text.empty());

    narrow           = label_style;
    narrow.max_width = 4;
    EXPECT_TRUE(render::make_pill_label(font(), "Camera", narrow).text.empty());

    const auto single_line = render::make_pill_label(font(), "A\nB\tC", label_style);
    EXPECT_EQ(single_line.text, U"A B C");
}

TEST_F(PillLabel, DoubledPixelStyleProducesNativeResolutionLargerLabel)
{
    auto doubled = label_style;
    doubled.font_size *= 2;
    doubled.padding_x *= 2;
    doubled.padding_y *= 2;
    doubled.max_width *= 2;
    doubled.max_height *= 2;
    const auto small = render::make_pill_label(font(), "Camera", label_style);
    const auto large = render::make_pill_label(font(), "Camera", doubled);
    EXPECT_NEAR(large.dimensions.x, 2 * small.dimensions.x, 8);
    EXPECT_NEAR(large.dimensions.y, 2 * small.dimensions.y, 4);
}
} // namespace
