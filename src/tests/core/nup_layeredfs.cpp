// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/nup_container.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_layered.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace {

using FileSys::VirtualDir;
using FileSys::VirtualFile;

VirtualFile MakeFile(std::string name, const std::string& data) {
    return std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(data.begin(), data.end()),
                                                    std::move(name));
}

VirtualDir MakeDir(std::string name, std::vector<VirtualFile> files,
                   std::vector<VirtualDir> dirs = {}) {
    return std::make_shared<FileSys::VectorVfsDirectory>(std::move(files), std::move(dirs),
                                                         std::move(name));
}

std::string Read(const VirtualFile& file) {
    REQUIRE(file != nullptr);
    const auto bytes = file->ReadAllBytes();
    return std::string(bytes.begin(), bytes.end());
}

} // namespace

TEST_CASE("A mod romfs layered over a merged NUP tree replaces files in the packed RomFS",
          "[nup]") {
    const auto base = MakeDir("romfs", {MakeFile("root.bin", "base-root")},
                              {MakeDir("Tex", {MakeFile("a.bntx.zs", std::string(64, '\0')),
                                               MakeFile("b.bntx.zs", "base-b")})});
    const auto update =
        MakeDir("romfs", {}, {MakeDir("Tex", {MakeFile("b.bntx.zs", "update-b")})});
    const auto merged = FileSys::NupContainer::MergeShadowDirs(base, update);
    const auto mod =
        MakeDir("romfs", {}, {MakeDir("Tex", {MakeFile("a.bntx.zs", "mod-a, a different size")})});

    const auto layered =
        FileSys::LayeredVfsDirectory::MakeLayeredDirectory({mod, merged}, merged->GetName());
    const auto packed = FileSys::CreateRomFS(layered);
    REQUIRE(packed != nullptr);

    const auto extracted = FileSys::ExtractRomFS(packed);
    REQUIRE(extracted != nullptr);
    REQUIRE(Read(extracted->GetFileRelative("Tex/a.bntx.zs")) == "mod-a, a different size");
    REQUIRE(Read(extracted->GetFileRelative("Tex/b.bntx.zs")) == "update-b");
    REQUIRE(Read(extracted->GetFileRelative("root.bin")) == "base-root");
    REQUIRE(extracted->GetSubdirectory("Tex")->GetFiles().size() == 2);
}
