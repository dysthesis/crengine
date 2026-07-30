/** \file kp_fuzz.cpp
    \brief libFuzzer harness for the Knuth--Plass paragraph breaker

    CoolReader Engine

    This source code is distributed under the terms of
    GNU General Public License.

    See LICENSE file for details.
*/

#include "../crengine/include/lvkplinebreak.h"

#include <fuzzer/FuzzedDataProvider.h>

#include <cassert>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <vector>

#ifdef NDEBUG
#error "the oracles below are assert()s; build without NDEBUG"
#endif

namespace {

const int MAX_ITEMS = 128;

// Half the draws are typographically plausible so most executions reach a
// feasible paragraph, half span the whole range to exercise saturation in the
// width and demerit accumulators.
int drawSize(FuzzedDataProvider & fdp)
{
    return fdp.ConsumeBool() ? fdp.ConsumeIntegralInRange<int>(0, 2000)
                             : fdp.ConsumeIntegralInRange<int>(0, INT_MAX);
}

std::vector<KPItem> drawItems(FuzzedDataProvider & fdp)
{
    std::vector<KPItem> items;
    const int count = fdp.ConsumeIntegralInRange<int>(0, MAX_ITEMS);
    for (int i = 0; i < count && fdp.remaining_bytes() > 0; i++) {
        KPItem item = { KPItem::BOX, 0, 0, 0, 0, false, i, 0, 0 };
        // Protrusion is signed: a glyph overflowing the margin more than the
        // margin can absorb pulls the edge inwards.
        item.protrusion = fdp.ConsumeBool()
                ? fdp.ConsumeIntegralInRange<int>(-64, 64)
                : fdp.ConsumeIntegralInRange<int>(INT_MIN, INT_MAX);
        switch (fdp.ConsumeIntegralInRange<int>(0, 2)) {
        case 0:
            item.width = drawSize(fdp);
            break;
        case 1:
            item.type = KPItem::GLUE;
            item.width = drawSize(fdp);
            item.stretch = drawSize(fdp);
            item.shrink = drawSize(fdp);
            break;
        default:
            item.type = KPItem::PENALTY;
            item.width = drawSize(fdp);
            item.penalty = fdp.ConsumeIntegralInRange<int>(-KP_INFINITY, KP_INFINITY);
            item.flagged = fdp.ConsumeBool();
            break;
        }
        items.push_back(item);
    }
    return items;
}

// Without the documented terminator every execution bounces off input
// validation, so append it rather than hoping the fuzzer guesses it.
void finishParagraph(std::vector<KPItem> & items)
{
    const KPItem no_break = { KPItem::PENALTY, 0, 0, 0, KP_INFINITY, false, 0, 0, 0 };
    const KPItem filler = { KPItem::GLUE, 0, 100000, 0, 0, false, 0, 0, 0 };
    const KPItem forced = { KPItem::PENALTY, 0, 0, 0, -KP_INFINITY, false, 0, 0, 0 };
    items.push_back(no_break);
    items.push_back(filler);
    items.push_back(forced);
}

void checkBreaks(const std::vector<KPLine> & lines, int count, int n_items)
{
    int previous = -1;
    for (int i = 0; i < count; i++) {
        assert(lines[i].break_item > previous);
        assert(lines[i].break_item < n_items);
        previous = lines[i].break_item;
    }
    assert(previous == n_items - 1);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t * data, std::size_t size)
{
    FuzzedDataProvider fdp(data, size);

    KPParams params;
    params.tolerance = fdp.ConsumeIntegralInRange<int>(0, KP_INFINITY);
    params.line_penalty = fdp.ConsumeIntegralInRange<int>(0, 10000);
    params.adj_demerits = fdp.ConsumeIntegralInRange<int>(0, INT_MAX);
    params.double_hyphen_demerits = fdp.ConsumeIntegralInRange<int>(0, INT_MAX);
    params.final_hyphen_demerits = fdp.ConsumeIntegralInRange<int>(0, INT_MAX);
    params.emergency_stretch = fdp.ConsumeIntegralInRange<int>(0, INT_MAX);

    const int first_width = drawSize(fdp);
    const int rest_width = drawSize(fdp);

    std::vector<KPItem> items = drawItems(fdp);
    finishParagraph(items);
    const int n_items = static_cast<int>(items.size());

    std::vector<KPLine> lines(n_items);
    const int count = kp_break_paragraph(&items[0], n_items, first_width,
                                         rest_width, params, &lines[0], n_items);
    assert(count >= -1 && count <= n_items);
    if (count > 0)
        checkBreaks(lines, count, n_items);

    // Pure function: same input, same answer.
    std::vector<KPLine> again(n_items);
    const int recount = kp_break_paragraph(&items[0], n_items, first_width,
                                           rest_width, params, &again[0], n_items);
    assert(recount == count);
    for (int i = 0; i < count; i++) {
        assert(again[i].break_item == lines[i].break_item);
        assert(again[i].ratio_x1000 == lines[i].ratio_x1000);
    }

    // Documented: too small an output buffer is rejected, never overrun.
    if (count > 0) {
        std::vector<KPLine> tight(count);
        assert(kp_break_paragraph(&items[0], n_items, first_width, rest_width,
                                  params, &tight[0], count - 1) == -1);
    }

    std::vector<KPItem> spacing_items = items;
    for (int i = 0; i < n_items; i++) {
        KPItem & item = spacing_items[i];
        if (item.type != KPItem::GLUE)
            continue;
        item.adjustable = item.width;
        if (item.stretch > item.adjustable)
            item.stretch = item.adjustable;
        if (item.shrink > item.adjustable)
            item.shrink = item.adjustable;
    }
    KPSpacingParams spacing_params;
    spacing_params.double_hyphen_demerits = params.double_hyphen_demerits;
    spacing_params.final_hyphen_demerits = params.final_hyphen_demerits;

    std::vector<KPLine> spacing_lines(n_items);
    const int spacing_count = kp_break_paragraph_spacing(&spacing_items[0],
            n_items, first_width, rest_width, spacing_params,
            &spacing_lines[0], n_items);
    assert(spacing_count >= -1 && spacing_count <= n_items);
    if (spacing_count > 0) {
        checkBreaks(spacing_lines, spacing_count, n_items);
        for (int i = 0; i < spacing_count; i++) {
            assert(spacing_lines[i].ratio_x1000 >= -KP_RATIO_SCALE);
            assert(spacing_lines[i].ratio_x1000 <= KP_RATIO_SCALE);
        }
    }

    std::vector<KPLine> spacing_again(n_items);
    const int spacing_recount = kp_break_paragraph_spacing(&spacing_items[0],
            n_items, first_width, rest_width, spacing_params,
            &spacing_again[0], n_items);
    assert(spacing_recount == spacing_count);
    for (int i = 0; i < spacing_count; i++) {
        assert(spacing_again[i].break_item == spacing_lines[i].break_item);
        assert(spacing_again[i].ratio_x1000 == spacing_lines[i].ratio_x1000);
    }
    if (spacing_count > 0) {
        std::vector<KPLine> tight(spacing_count);
        assert(kp_break_paragraph_spacing(&spacing_items[0], n_items,
                first_width, rest_width, spacing_params, &tight[0],
                spacing_count - 1) == -1);
    }

    return 0;
}
