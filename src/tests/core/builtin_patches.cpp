// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include "common/common_funcs.h"
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/nso.h"

namespace {

constexpr u64 OcarinaTitleId = 0x0400486024DEC000;
constexpr u64 OtherTitleId = 0x0100000000010000;
constexpr std::string_view MaxResolution = "Max Resolution (built-in)";
constexpr std::string_view DynamicFps = "Dynamic FPS by Dark (built-in)";
constexpr std::string_view OcarinaBuildIdHex = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348";
constexpr FileSys::PatchManager::BuildID OcarinaBuildId{
    0xE0, 0xF3, 0xCC, 0xFA, 0xF5, 0x00, 0x65, 0xAE, 0x4E, 0xA0,
    0x1E, 0xB5, 0xAE, 0xB1, 0x3B, 0x1A, 0x45, 0xDA, 0x33, 0x48,
};
constexpr size_t PatchOffset = sizeof(Loader::NSOHeader) + 0x805BD0;
constexpr u32 TbzInstruction = 0x360041E8;
constexpr u32 BranchInstruction = 0x1400020F;
constexpr u32 NopInstruction = 0xD503201F;
constexpr size_t DynamicCallOffset = sizeof(Loader::NSOHeader) + 0x6E6CC4;
constexpr size_t DynamicRoutineOffset = sizeof(Loader::NSOHeader) + 0x3C46200;
constexpr size_t DynamicStateOffset = sizeof(Loader::NSOHeader) + 0x4E9FE00;
constexpr size_t DynamicRoutineSize = 120;
constexpr u32 DynamicCallInstruction = 0x94D57D4F;
constexpr u32 DynamicOriginalInstruction = 0x1E201828;

constexpr std::array<std::pair<Common::FS::EdenPath, const char*>, 4> TestDirectories{{
    {Common::FS::EdenPath::NANDDir, "nand"},
    {Common::FS::EdenPath::SDMCDir, "sdmc"},
    {Common::FS::EdenPath::LoadDir, "load"},
    {Common::FS::EdenPath::DumpDir, "dump"},
}};

void WriteIps(const std::filesystem::path& path, u32 word, size_t offset = PatchOffset) {
    const bool ips32 = offset > 0xFFFFFF;
    std::vector<u8> ips =
        ips32 ? std::vector<u8>{'I', 'P', 'S', '3', '2'} : std::vector<u8>{'P', 'A', 'T', 'C', 'H'};
    if (ips32) {
        ips.push_back(static_cast<u8>(offset >> 24));
    }
    ips.push_back(static_cast<u8>(offset >> 16));
    ips.push_back(static_cast<u8>(offset >> 8));
    ips.push_back(static_cast<u8>(offset));
    ips.push_back(0);
    ips.push_back(sizeof(word));
    for (size_t i = 0; i < sizeof(word); ++i) {
        ips.push_back(static_cast<u8>(word >> (i * 8)));
    }
    if (ips32) {
        ips.push_back('E');
    }
    ips.insert(ips.end(), {'E', 'O', 'F'});
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(ips.data()), static_cast<std::streamsize>(ips.size()));
}

class PatchManagerTestContext {
public:
    explicit PatchManagerTestContext(std::optional<u32> user_mod_word = std::nullopt,
                                     size_t user_mod_offset = PatchOffset) {
        for (size_t i = 0; i < TestDirectories.size(); ++i) {
            const auto& [path, name] = TestDirectories[i];
            previous[i] = Common::FS::GetEdenPath(path);
            std::filesystem::create_directories(root / name);
            Common::FS::SetEdenPath(path, root / name);
        }
        const auto title_dir = root / "load" / fmt::format("{:016X}", OcarinaTitleId);
        std::filesystem::create_directories(title_dir);
        if (user_mod_word) {
            const auto exefs = title_dir / "UserMod" / "exefs";
            std::filesystem::create_directories(exefs);
            WriteIps(exefs / fmt::format("{}.ips", OcarinaBuildIdHex), *user_mod_word,
                     user_mod_offset);
        }
        system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
        system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
        system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    }

    ~PatchManagerTestContext() {
        for (size_t i = 0; i < TestDirectories.size(); ++i) {
            Common::FS::SetEdenPath(TestDirectories[i].first, previous[i]);
        }
        Settings::values.disabled_addons.erase(OcarinaTitleId);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    PatchManagerTestContext(const PatchManagerTestContext&) = delete;
    PatchManagerTestContext& operator=(const PatchManagerTestContext&) = delete;

    FileSys::PatchManager Manager(u64 title_id = OcarinaTitleId) {
        return FileSys::PatchManager{title_id, system.GetFileSystemController(),
                                     system.GetContentProvider()};
    }

private:
    std::array<std::filesystem::path, TestDirectories.size()> previous;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("builtin-patch-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Core::System system;
};

std::vector<u8> MakeMain(const FileSys::PatchManager::BuildID& build_id, u32 word) {
    std::vector<u8> nso(PatchOffset + 0x100);
    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');
    header.build_id = build_id;
    std::memcpy(nso.data(), &header, sizeof(header));
    std::memcpy(nso.data() + PatchOffset, &word, sizeof(word));
    return nso;
}

u32 WordAt(const std::vector<u8>& nso, size_t offset = PatchOffset) {
    REQUIRE(nso.size() >= offset + sizeof(u32));
    u32 word;
    std::memcpy(&word, nso.data() + offset, sizeof(word));
    return word;
}

u32 PatchMain(PatchManagerTestContext& context, const FileSys::PatchManager::BuildID& build_id,
              u32 word, const std::string& name = "main", u64 title_id = OcarinaTitleId) {
    return WordAt(context.Manager(title_id).PatchNSO(MakeMain(build_id, word), name));
}

} // Anonymous namespace

TEST_CASE("Built-in Max Resolution patch is gated on build id and original word",
          "[patch_manager]") {
    PatchManagerTestContext context;

    SECTION("Matching build id and original word applies") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }

    SECTION("Wrong original word skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, NopInstruction) == NopInstruction);
    }

    SECTION("Other build id skips") {
        auto other = OcarinaBuildId;
        other[0] ^= 1;
        REQUIRE(PatchMain(context, other, TbzInstruction) == TbzInstruction);
    }

    SECTION("Other module skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction, "sdk") == TbzInstruction);
    }

    SECTION("Other title skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction, "main", OtherTitleId) ==
                TbzInstruction);
    }

    SECTION("Disabled in Add-Ons skips") {
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == TbzInstruction);
    }
}

TEST_CASE("Built-in Max Resolution patch defers to a user mod at the same offset",
          "[patch_manager]") {
    SECTION("A different word from the mod is kept") {
        PatchManagerTestContext context{NopInstruction};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == NopInstruction);
    }

    SECTION("The same word from the mod is applied once") {
        PatchManagerTestContext context{BranchInstruction};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }

    SECTION("Disabling the built-in patch keeps the mod") {
        PatchManagerTestContext context{BranchInstruction};
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }
}

namespace {

constexpr std::string_view RealTime = "Real-time speed (built-in)";
constexpr std::array<std::pair<size_t, u32>, 4> RealTimeOriginals{{
    {0x3C461E0, 0x00000000},
    {0x3C461E4, 0x00000000},
    {0x3C461E8, 0x00000000},
    {0x6E6CC4, 0x1E201828},
}};

std::vector<u8> MakeRealTimeMain() {
    std::vector<u8> nso(sizeof(Loader::NSOHeader) + 0x3C47000);
    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');
    header.build_id = OcarinaBuildId;
    std::memcpy(nso.data(), &header, sizeof(header));
    for (const auto& [offset, word] : RealTimeOriginals) {
        std::memcpy(nso.data() + sizeof(Loader::NSOHeader) + offset, &word, sizeof(word));
    }
    return nso;
}

u32 ModuleWord(const std::vector<u8>& nso, size_t offset) {
    u32 word;
    std::memcpy(&word, nso.data() + sizeof(Loader::NSOHeader) + offset, sizeof(word));
    return word;
}

} // Anonymous namespace

TEST_CASE("Built-in real-time patch applies all of its words or none", "[patch_manager]") {
    PatchManagerTestContext context;
    auto nso = MakeRealTimeMain();

    SECTION("All words apply, on by default") {
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(ModuleWord(patched, 0x3C461E0) == 0x3F800000);
        REQUIRE(ModuleWord(patched, 0x3C461E4) == 0x1CFFFFE8);
        REQUIRE(ModuleWord(patched, 0x3C461E8) == 0x172A82B8);
        REQUIRE(ModuleWord(patched, 0x6E6CC4) == 0x14D57D48);
    }

    SECTION("One changed word leaves the hook out") {
        const u32 other = 0xD503201F;
        std::memcpy(nso.data() + sizeof(Loader::NSOHeader) + 0x3C461E4, &other, sizeof(other));
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(ModuleWord(patched, 0x3C461E0) == 0x00000000);
        REQUIRE(ModuleWord(patched, 0x6E6CC4) == 0x1E201828);
    }

    SECTION("Disabled in Add-Ons skips") {
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{RealTime}};
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(ModuleWord(patched, 0x6E6CC4) == 0x1E201828);
    }

    SECTION("It is one Add-Ons entry") {
        const auto patches = context.Manager().GetPatches();
        REQUIRE(std::ranges::count(patches, RealTime, &FileSys::Patch::name) == 1);
    }
}

TEST_CASE("Built-in Max Resolution patch is listed and loaded for Ocarina's main",
          "[patch_manager]") {
    PatchManagerTestContext context;
    const auto manager = context.Manager();

    REQUIRE(manager.HasNSOPatch(OcarinaBuildId, "main"));
    REQUIRE_FALSE(manager.HasNSOPatch(OcarinaBuildId, "sdk"));
    REQUIRE_FALSE(context.Manager(OtherTitleId).HasNSOPatch(OcarinaBuildId, "main"));

    const auto builtin_enabled = [&] {
        const auto patches = manager.GetPatches();
        const auto it = std::ranges::find(patches, MaxResolution, &FileSys::Patch::name);
        REQUIRE(it != patches.end());
        REQUIRE(it->type == FileSys::PatchType::Mod);
        return it->enabled;
    };
    REQUIRE(builtin_enabled());

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    REQUIRE_FALSE(builtin_enabled());
}

namespace {

std::vector<u8> MakeDynamicMain() {
    auto nso = MakeMain(OcarinaBuildId, TbzInstruction);
    nso.resize(DynamicStateOffset + 16);
    Loader::NSOHeader header;
    std::memcpy(&header, nso.data(), sizeof(header));
    header.segments[0].size = 63201744;
    header.segments[1].location = 63205376;
    header.segments[1].size = 12603520;
    header.segments[2].location = 75812864;
    header.segments[2].size = 5217152;
    header.segments[2].bss_size = 1414272;
    std::memcpy(nso.data(), &header, sizeof(header));
    std::memcpy(nso.data() + DynamicCallOffset, &DynamicOriginalInstruction,
                sizeof(DynamicOriginalInstruction));
    return nso;
}

} // Anonymous namespace

TEST_CASE("Built-in Dynamic FPS is off by default and selected by disabling Real-time speed",
          "[patch_manager]") {
    PatchManagerTestContext context;
    const auto manager = context.Manager();
    const auto enabled = [&] {
        const auto patches = manager.GetPatches();
        const auto it = std::ranges::find(patches, DynamicFps, &FileSys::Patch::name);
        REQUIRE(it != patches.end());
        REQUIRE(it->type == FileSys::PatchType::Mod);
        return it->enabled;
    };
    // Superseded by Real-time speed, which hooks the same instruction (see the test below).
    REQUIRE_FALSE(enabled());
    auto nso = MakeDynamicMain();
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{RealTime}};
    REQUIRE(enabled());
    auto patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(WordAt(patched, DynamicRoutineOffset) == 0x1E201828);
    REQUIRE(WordAt(patched, DynamicRoutineOffset + 116) == 0x3F83D70A);
    REQUIRE(
        Common::HexToString(std::span{patched}.subspan(DynamicRoutineOffset, DynamicRoutineSize)) ==
        "2818201ECA9200B04A0138912BE03BD54C0140F94B0100F9CC0200B46B010CCB0DE03BD5"
        "8E0280D2AE09CE9A7F010EEBCB818B9A4C0540F94C0000B5EC030BAA6E010CCB8C0D8E8B"
        "4C0500F98201239EA301239E4218231E4208211EC300001C0309231E4020231E4D000054"
        "4840201EC0035FD60AD7833F");
    REQUIRE(WordAt(patched) == BranchInstruction);
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{DynamicFps}, std::string{RealTime}};
    REQUIRE_FALSE(enabled());
    REQUIRE(WordAt(manager.PatchNSO(nso, "main"), DynamicCallOffset) == DynamicOriginalInstruction);
    REQUIRE(WordAt(manager.PatchNSO(nso, "main"), DynamicRoutineOffset) == 0);

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}, std::string{RealTime}};
    REQUIRE(enabled());
    patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(WordAt(patched) == TbzInstruction);

    const auto unrelated = context.Manager(OtherTitleId).GetPatches();
    REQUIRE(std::ranges::find(unrelated, DynamicFps, &FileSys::Patch::name) == unrelated.end());
}

TEST_CASE("Built-in Dynamic FPS rejects mismatched or truncated modules transactionally",
          "[patch_manager]") {
    PatchManagerTestContext context;
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}, std::string{RealTime}};
    auto nso = MakeDynamicMain();
    std::string module = "main";
    u64 title = OcarinaTitleId;
    SECTION("Wrong build") {
        nso[offsetof(Loader::NSOHeader, build_id)] ^= 1;
    }
    SECTION("Wrong module") {
        module = "sdk";
    }
    SECTION("Wrong title") {
        title = OtherTitleId;
    }
    SECTION("Incomplete header") {
        nso.resize(sizeof(Loader::NSOHeader) - 1);
    }
    SECTION("Incomplete call") {
        nso.resize(DynamicCallOffset + 3);
    }
    SECTION("Incomplete routine") {
        nso.resize(DynamicRoutineOffset + DynamicRoutineSize - 1);
    }
    SECTION("Incomplete timer state") {
        nso.resize(DynamicStateOffset + 15);
    }
    SECTION("Timer state outside BSS") {
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[2].bss_size = 15;
        std::memcpy(nso.data(), &header, sizeof(header));
    }
    SECTION("Wrong original hook") {
        nso[DynamicCallOffset] ^= 1;
    }
    SECTION("Nonzero code cave") {
        nso[DynamicRoutineOffset + DynamicRoutineSize - 1] = 1;
    }
    SECTION("Nonzero timer state") {
        nso[DynamicStateOffset + 15] = 1;
    }
    SECTION("Code cave outside executable padding") {
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[0].size = 0x3C46000;
        std::memcpy(nso.data(), &header, sizeof(header));
    }
    SECTION("Code cave overlaps read-only segment") {
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[1].location = 0x3C46200;
        std::memcpy(nso.data(), &header, sizeof(header));
    }
    REQUIRE(context.Manager(title).PatchNSO(nso, module) == nso);
}

TEST_CASE("Built-in Dynamic FPS leaves both records alone when a user mod changes either",
          "[patch_manager]") {
    size_t offset = DynamicCallOffset;
    u32 mod_word = 0x11223344;
    SECTION("User call takes precedence") {}
    SECTION("Identical call still takes precedence for the whole patch") {
        mod_word = DynamicCallInstruction;
    }
    SECTION("User routine takes precedence") {
        offset = DynamicRoutineOffset;
    }
    SECTION("Last routine byte takes precedence") {
        offset = DynamicRoutineOffset + DynamicRoutineSize - sizeof(u32);
    }
    SECTION("User timer state takes precedence") {
        offset = DynamicStateOffset;
    }
    PatchManagerTestContext context{mod_word, offset};
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}, std::string{RealTime}};
    const auto nso = MakeDynamicMain();
    auto expected = nso;
    std::memcpy(expected.data() + offset, &mod_word, sizeof(mod_word));
    REQUIRE(context.Manager().PatchNSO(nso, "main") == expected);
}

TEST_CASE("Built-in Real-time speed and Dynamic FPS never both apply", "[patch_manager]") {
    PatchManagerTestContext context;
    const auto nso = MakeDynamicMain();
    SECTION("Defaults: Real-time speed applies, Dynamic FPS is skipped") {
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(WordAt(patched, DynamicCallOffset) == 0x14D57D48);
        REQUIRE(ModuleWord(patched, 0x3C461E4) == 0x1CFFFFE8);
        REQUIRE(WordAt(patched, DynamicRoutineOffset) == 0);
    }
    SECTION("Real-time speed disabled: Dynamic FPS applies") {
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{RealTime}};
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
        REQUIRE(ModuleWord(patched, 0x3C461E0) == 0);
    }
}

namespace {

// The desktop Add-Ons dialog (configure_per_game_addons.cpp) without Qt: one checkbox per listed
// add-on, ticked as LoadConfiguration ticks it; ticking one unticks the built-ins that write the
// same code; applying disables every unticked entry, then keeps superseded built-in entries.
class AddonsDialog {
public:
    explicit AddonsDialog(const FileSys::PatchManager& manager) {
        const auto& disabled = Settings::values.disabled_addons[OcarinaTitleId];
        for (const auto& patch : manager.GetPatches()) {
            const bool listed_disabled = std::ranges::find(disabled, patch.name) != disabled.end();
            const bool is_mod = patch.type == FileSys::PatchType::Mod;
            items.emplace_back(patch.name, !listed_disabled && (!is_mod || patch.enabled));
        }
    }

    bool Ticked(std::string_view name) {
        return Item(name);
    }

    void Tick(std::string_view name, bool ticked) {
        Item(name) = ticked;
        if (!ticked) {
            return;
        }
        for (const auto& other : FileSys::ConflictingBuiltinPatches(OcarinaTitleId, name)) {
            Item(other) = false;
        }
    }

    void Apply() {
        std::vector<std::string> disabled;
        for (const auto& [name, ticked] : items) {
            if (!ticked) {
                disabled.push_back(name);
            }
        }
        auto& saved = Settings::values.disabled_addons[OcarinaTitleId];
        FileSys::KeepSupersededBuiltinEntries(OcarinaTitleId, disabled, saved);
        saved = disabled;
    }

private:
    bool& Item(std::string_view name) {
        const auto it = std::ranges::find(items, name, &std::pair<std::string, bool>::first);
        REQUIRE(it != items.end());
        return it->second;
    }

    std::vector<std::pair<std::string, bool>> items;
};

enum class SpeedPatch { None, RealTime, DynamicFps };

SpeedPatch AppliedSpeedPatch(const FileSys::PatchManager& manager) {
    const u32 word = WordAt(manager.PatchNSO(MakeDynamicMain(), "main"), DynamicCallOffset);
    if (word == 0x14D57D48) {
        return SpeedPatch::RealTime;
    }
    REQUIRE((word == DynamicCallInstruction || word == DynamicOriginalInstruction));
    return word == DynamicCallInstruction ? SpeedPatch::DynamicFps : SpeedPatch::None;
}

bool SavedDisabled(std::string_view name) {
    const auto& saved = Settings::values.disabled_addons[OcarinaTitleId];
    return std::ranges::find(saved, name) != saved.end();
}

} // Anonymous namespace

TEST_CASE("Real-time speed and Dynamic FPS write the same code and exclude each other",
          "[patch_manager][addons]") {
    REQUIRE(FileSys::ConflictingBuiltinPatches(OcarinaTitleId, RealTime) ==
            std::vector<std::string>{std::string{DynamicFps}});
    REQUIRE(FileSys::ConflictingBuiltinPatches(OcarinaTitleId, DynamicFps) ==
            std::vector<std::string>{std::string{RealTime}});
    REQUIRE(FileSys::ConflictingBuiltinPatches(OcarinaTitleId, MaxResolution).empty());
    REQUIRE(FileSys::ConflictingBuiltinPatches(OtherTitleId, RealTime).empty());
    REQUIRE(FileSys::ConflictingBuiltinPatches(OcarinaTitleId, "UserMod").empty());
}

TEST_CASE("The Add-Ons list shows the speed patch that applies and saves only real choices",
          "[patch_manager][addons]") {
    PatchManagerTestContext context;
    const auto manager = context.Manager();

    // Defaults: Real-time speed ticked, Dynamic FPS unticked, and saving changes nothing.
    {
        AddonsDialog dialog{manager};
        REQUIRE(dialog.Ticked(RealTime));
        REQUIRE_FALSE(dialog.Ticked(DynamicFps));
        REQUIRE(dialog.Ticked(MaxResolution));
        dialog.Apply();
        REQUIRE(Settings::values.disabled_addons[OcarinaTitleId].empty());
        REQUIRE(AppliedSpeedPatch(manager) == SpeedPatch::RealTime);
    }
    // Ticking Dynamic FPS unticks Real-time speed.
    {
        AddonsDialog dialog{manager};
        dialog.Tick(DynamicFps, true);
        REQUIRE_FALSE(dialog.Ticked(RealTime));
        dialog.Apply();
        REQUIRE(SavedDisabled(RealTime));
        REQUIRE_FALSE(SavedDisabled(DynamicFps));
        REQUIRE(AppliedSpeedPatch(manager) == SpeedPatch::DynamicFps);
    }
    // ...and ticking Real-time speed again unticks Dynamic FPS without saving it as disabled.
    {
        AddonsDialog dialog{manager};
        REQUIRE_FALSE(dialog.Ticked(RealTime));
        REQUIRE(dialog.Ticked(DynamicFps));
        dialog.Tick(RealTime, true);
        REQUIRE_FALSE(dialog.Ticked(DynamicFps));
        dialog.Apply();
        REQUIRE(Settings::values.disabled_addons[OcarinaTitleId].empty());
        REQUIRE(AppliedSpeedPatch(manager) == SpeedPatch::RealTime);
    }
    // Both unticked: neither applies, and the list shows both unticked again.
    {
        AddonsDialog dialog{manager};
        dialog.Tick(RealTime, false);
        dialog.Apply();
        REQUIRE(SavedDisabled(RealTime));
        REQUIRE(SavedDisabled(DynamicFps));
        REQUIRE(AppliedSpeedPatch(manager) == SpeedPatch::None);
        AddonsDialog reloaded{manager};
        REQUIRE_FALSE(reloaded.Ticked(RealTime));
        REQUIRE_FALSE(reloaded.Ticked(DynamicFps));
        reloaded.Tick(DynamicFps, true);
        reloaded.Apply();
        REQUIRE(AppliedSpeedPatch(manager) == SpeedPatch::DynamicFps);
    }
    // An explicit Dynamic FPS disable saved earlier survives while Real-time speed is enabled.
    {
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{DynamicFps}};
        AddonsDialog dialog{manager};
        REQUIRE(dialog.Ticked(RealTime));
        REQUIRE_FALSE(dialog.Ticked(DynamicFps));
        dialog.Apply();
        REQUIRE(Settings::values.disabled_addons[OcarinaTitleId] ==
                std::vector<std::string>{std::string{DynamicFps}});
        REQUIRE(AppliedSpeedPatch(manager) == SpeedPatch::RealTime);
        AddonsDialog unticked{manager};
        unticked.Tick(RealTime, false);
        unticked.Apply();
        REQUIRE(AppliedSpeedPatch(manager) == SpeedPatch::None);
    }
}
