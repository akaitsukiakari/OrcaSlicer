#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SettingsText.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

static bool contains(const std::vector<std::string>& keys, const std::string& key)
{
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

TEST_CASE("Settings text round-trips the exported keys", "[SettingsText]")
{
    DynamicPrintConfig config;
    config.set_key_value("layer_height", new ConfigOptionFloat(0.16));
    config.set_key_value("wall_loops", new ConfigOptionInt(4));
    config.set_key_value("initial_layer_speed", new ConfigOptionFloatsNullable{25., 30.});

    const std::string text = settings_to_text(config, {"layer_height", "wall_loops", "initial_layer_speed"}, {"base: test"});
    REQUIRE(text.rfind("# base: test\n", 0) == 0);

    SettingsTextParseResult parsed = settings_from_text(text);
    CHECK(parsed.skipped_keys.empty());
    CHECK(parsed.invalid_keys.empty());
    CHECK_THAT(parsed.config.opt_float("layer_height"), WithinAbs(0.16, 1e-9));
    CHECK(parsed.config.opt_int("wall_loops") == 4);
    CHECK(parsed.config.opt_serialize("initial_layer_speed") == config.opt_serialize("initial_layer_speed"));

    // Exporting what was imported gives the same text.
    CHECK(settings_to_text(parsed.config, {"layer_height", "wall_loops", "initial_layer_speed"}, {"base: test"}) == text);
}

TEST_CASE("Settings text accepts the G-code configuration block", "[SettingsText]")
{
    const std::string text = "; CONFIG_BLOCK_START\n"
                             "; layer_height = 0.2\n"
                             ";   wall_loops = 3\n"
                             "; CONFIG_BLOCK_END\n";
    SettingsTextParseResult parsed = settings_from_text(text);
    CHECK(parsed.config.keys().size() == 2);
    CHECK_THAT(parsed.config.opt_float("layer_height"), WithinAbs(0.2, 1e-9));
    CHECK(parsed.config.opt_int("wall_loops") == 3);
}

TEST_CASE("Settings text skips identity and unknown keys and reports bad values", "[SettingsText]")
{
    const std::string text = "# a comment = with an equals sign\n"
                             "inherits = Some Parent\n"
                             "not_a_real_setting = 5\n"
                             "wall_loops = lots\n"
                             "layer_height = 0.24\n";
    SettingsTextParseResult parsed = settings_from_text(text);
    CHECK(contains(parsed.skipped_keys, "inherits"));
    CHECK(contains(parsed.skipped_keys, "not_a_real_setting"));
    CHECK(contains(parsed.invalid_keys, "wall_loops"));
    CHECK_FALSE(parsed.config.has("inherits"));
    CHECK_THAT(parsed.config.opt_float("layer_height"), WithinAbs(0.24, 1e-9));
}

TEST_CASE("Settings text export leaves out identity keys", "[SettingsText]")
{
    DynamicPrintConfig config;
    config.set_key_value("inherits", new ConfigOptionString("Parent"));
    config.set_key_value("wall_loops", new ConfigOptionInt(2));
    const std::string text = settings_to_text(config, {"inherits", "wall_loops"}, {});
    CHECK(text == "wall_loops = 2\n");
}
