// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <span>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "video_core/texture_cache/types.h"

namespace VideoCommon {

/// The last feedback loop answer of the texture cache and what it was worked out for: the
/// render target and texture table serials, the sampled views in order, and the view lifetime
/// epoch. View ids are recycled slots, so a view retired and another allocated into its slot
/// keeps the id; the epoch, advanced whenever views are retired, tells them apart.
class FeedbackLoopCache {
public:
    /// keep_views false: serials only (the key before the views were added).
    [[nodiscard]] bool Hit(u64 render_targets_serial, u64 texture_serial, u64 view_epoch,
                           std::span<const ImageViewId> views, bool keep_views) const {
        if (!valid || render_targets_serial != rt_serial || texture_serial != tex_serial) {
            return false;
        }
        return !keep_views || (view_epoch == epoch && std::ranges::equal(views, view_ids));
    }

    void Store(u64 render_targets_serial, u64 texture_serial, u64 view_epoch,
               std::span<const ImageViewId> views, bool requires_barrier) {
        rt_serial = render_targets_serial;
        tex_serial = texture_serial;
        epoch = view_epoch;
        view_ids.assign(views.begin(), views.end());
        answer = requires_barrier;
        valid = true;
    }

    [[nodiscard]] bool Answer() const noexcept {
        return answer;
    }

    [[nodiscard]] u64 RenderTargetsSerial() const noexcept {
        return rt_serial;
    }

private:
    u64 rt_serial{};
    u64 tex_serial{};
    u64 epoch{};
    boost::container::small_vector<ImageViewId, 32> view_ids;
    bool answer{};
    bool valid{};
};

} // namespace VideoCommon
