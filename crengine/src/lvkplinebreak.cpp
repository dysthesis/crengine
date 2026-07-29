/** \file lvkplinebreak.cpp
    \brief dependency-free Knuth--Plass paragraph breaker

    CoolReader Engine

    This source code is distributed under the terms of
    GNU General Public License.

    See LICENSE file for details.
*/

#include "../include/lvkplinebreak.h"

#include <climits>
#include <cstdint>
#include <vector>

namespace {

typedef std::int64_t Demerits;
typedef __int128 WideInt;

const Demerits MAX_DEMERITS = INT64_MAX / 4;

enum Fitness {
    FITNESS_TIGHT,
    FITNESS_DECENT,
    FITNESS_LOOSE,
    FITNESS_VERY_LOOSE
};

struct Totals {
    std::int64_t width;
    std::int64_t stretch;
    std::int64_t shrink;

    Totals() : width(0), stretch(0), shrink(0) {}
};

struct LineFit {
    int badness;
    int fitness;
    int ratio_x1000;
    bool overfull;
};

struct BreakNode {
    int break_item;
    int previous;
    int line_count;
    int fitness;
    int ratio_x1000;
    bool flagged;
    Demerits demerits;
};

struct Candidate {
    int previous;
    int line_count;
    int fitness;
    int ratio_x1000;
    Demerits demerits;
};

bool addMetric(std::int64_t & total, int value)
{
    if (value < 0 || total > INT_MAX - value)
        return false;
    total += value;
    return true;
}

bool buildPrefixTotals(const KPItem * items, int n_items,
                       std::vector<Totals> & prefix)
{
    prefix.resize(n_items + 1);
    for (int i = 0; i < n_items; i++) {
        prefix[i + 1] = prefix[i];
        const KPItem & item = items[i];
        switch (item.type) {
        case KPItem::BOX:
            if (!addMetric(prefix[i + 1].width, item.width))
                return false;
            break;
        case KPItem::GLUE:
            if (!addMetric(prefix[i + 1].width, item.width)
                    || !addMetric(prefix[i + 1].stretch, item.stretch)
                    || !addMetric(prefix[i + 1].shrink, item.shrink))
                return false;
            break;
        case KPItem::PENALTY:
            if (item.width < 0)
                return false;
            break;
        default:
            return false;
        }
    }
    return true;
}

int ratioTimes1000(std::int64_t amount, std::int64_t capacity, bool negative)
{
    if (capacity == 0)
        return negative ? INT_MIN : INT_MAX;
    WideInt ratio = static_cast<WideInt>(amount) * 1000 / capacity;
    if (ratio > INT_MAX)
        return negative ? INT_MIN : INT_MAX;
    int result = static_cast<int>(ratio);
    return negative ? -result : result;
}

int calculateBadness(std::int64_t amount, std::int64_t capacity)
{
    if (amount == 0)
        return 0;
    if (capacity == 0)
        return KP_INFINITY;

    WideInt a = amount;
    WideInt c = capacity;
    WideInt numerator = 100 * a * a * a;
    WideInt denominator = c * c * c;
    if (numerator >= static_cast<WideInt>(KP_INFINITY) * denominator)
        return KP_INFINITY;
    return static_cast<int>((numerator + denominator / 2) / denominator);
}

LineFit fitLine(std::int64_t natural, std::int64_t stretch,
                std::int64_t shrink, int target, int emergency_stretch)
{
    LineFit result;
    result.overfull = false;

    std::int64_t shortfall = static_cast<std::int64_t>(target) - natural;
    if (shortfall == 0) {
        result.badness = 0;
        result.fitness = FITNESS_DECENT;
        result.ratio_x1000 = 0;
        return result;
    }

    if (shortfall > 0) {
        std::int64_t capacity = stretch + emergency_stretch;
        result.badness = calculateBadness(shortfall, capacity);
        result.ratio_x1000 = ratioTimes1000(shortfall, capacity, false);
        if (result.badness > 99)
            result.fitness = FITNESS_VERY_LOOSE;
        else if (result.badness > 12)
            result.fitness = FITNESS_LOOSE;
        else
            result.fitness = FITNESS_DECENT;
        return result;
    }

    std::int64_t excess = -shortfall;
    result.overfull = shrink == 0 || excess > shrink;
    result.badness = result.overfull ? KP_INFINITY
                                     : calculateBadness(excess, shrink);
    result.ratio_x1000 = ratioTimes1000(excess, shrink, true);
    result.fitness = result.badness > 12 ? FITNESS_TIGHT : FITNESS_DECENT;
    return result;
}

bool isForced(const KPItem & item)
{
    return item.type == KPItem::PENALTY && item.penalty <= -KP_INFINITY;
}

bool isLegalBreak(const KPItem * items, int index)
{
    const KPItem & item = items[index];
    if (item.type == KPItem::PENALTY)
        return item.penalty < KP_INFINITY;
    return item.type == KPItem::GLUE && index > 0
            && items[index - 1].type == KPItem::BOX;
}

Demerits addDemerits(Demerits left, Demerits right)
{
    if (right > 0 && left > MAX_DEMERITS - right)
        return MAX_DEMERITS;
    if (right < 0 && left < -MAX_DEMERITS - right)
        return -MAX_DEMERITS;
    return left + right;
}

Demerits lineDemerits(const BreakNode & previous, const KPItem & item,
                      const LineFit & fit, const KPParams & params,
                      bool final_break)
{
    Demerits base = static_cast<Demerits>(params.line_penalty) + fit.badness;
    Demerits result = base >= KP_INFINITY ? 100000000 : base * base;

    int penalty = item.type == KPItem::PENALTY ? item.penalty : 0;
    if (penalty > 0)
        result += static_cast<Demerits>(penalty) * penalty;
    else if (penalty > -KP_INFINITY)
        result -= static_cast<Demerits>(penalty) * penalty;

    bool flagged = item.type == KPItem::PENALTY && item.flagged;
    if (final_break && previous.flagged)
        result += params.final_hyphen_demerits;
    else if (flagged && previous.flagged)
        result += params.double_hyphen_demerits;

    int fitness_delta = fit.fitness - previous.fitness;
    if (fitness_delta < -1 || fitness_delta > 1)
        result += params.adj_demerits;
    return result;
}

bool validParams(const KPParams & params)
{
    return params.tolerance >= 0 && params.tolerance <= KP_INFINITY
            && params.line_penalty >= 0
            && params.adj_demerits >= 0
            && params.double_hyphen_demerits >= 0
            && params.final_hyphen_demerits >= 0
            && params.emergency_stretch >= 0;
}

} // namespace

int kp_break_paragraph(const KPItem * items, int n_items,
                       int first_line_width, int rest_width,
                       const KPParams & params, KPLine * out, int max_out)
{
    if (n_items < 0 || n_items == INT_MAX || max_out < 0
            || first_line_width <= 0 || rest_width <= 0 || !validParams(params))
        return -1;
    if (n_items == 0)
        return 0;
    if (!items || (max_out > 0 && !out) || !isForced(items[n_items - 1]))
        return -1;

    std::vector<Totals> prefix;
    if (!buildPrefixTotals(items, n_items, prefix))
        return -1;

    // A new line discards glue and non-forced penalties up to its first box.
    std::vector<int> next_nondiscardable(n_items + 1, n_items);
    for (int i = n_items - 1; i >= 0; i--) {
        if (items[i].type == KPItem::BOX || isForced(items[i]))
            next_nondiscardable[i] = i;
        else
            next_nondiscardable[i] = next_nondiscardable[i + 1];
    }

    std::vector<BreakNode> nodes;
    std::vector<int> active;
    nodes.reserve(n_items + 1);
    active.reserve(n_items + 1);
    BreakNode initial = { -1, -1, 0, FITNESS_DECENT, 0, false, 0 };
    nodes.push_back(initial);
    active.push_back(0);

    std::vector<int> final_nodes;
    for (int break_item = 0; break_item < n_items; break_item++) {
        if (!isLegalBreak(items, break_item))
            continue;

        const KPItem & item = items[break_item];
        bool forced = isForced(item);
        bool final_break = forced && break_item == n_items - 1;
        std::vector<int> surviving;
        std::vector<Candidate> candidates;
        if (!forced)
            surviving.reserve(active.size());

        for (std::vector<int>::const_iterator it = active.begin();
                it != active.end(); ++it) {
            int previous_index = *it;
            const BreakNode & previous = nodes[previous_index];
            int start = next_nondiscardable[previous.break_item + 1];
            if (start > break_item)
                start = break_item;

            std::int64_t natural = prefix[break_item].width - prefix[start].width;
            std::int64_t stretch = prefix[break_item].stretch - prefix[start].stretch;
            std::int64_t shrink = prefix[break_item].shrink - prefix[start].shrink;
            if (item.type == KPItem::PENALTY)
                natural += item.width;

            int target = previous.line_count == 0 ? first_line_width : rest_width;
            LineFit fit = fitLine(natural, stretch, shrink, target,
                                  params.emergency_stretch);
            bool permanently_overfull = fit.overfull;
            if (permanently_overfull && item.type == KPItem::PENALTY) {
                // Break-only penalty width vanishes at later candidates.
                permanently_overfull = natural - item.width >
                        static_cast<std::int64_t>(target) + shrink;
            }
            if (!forced && !permanently_overfull)
                surviving.push_back(previous_index);
            if (fit.overfull || fit.badness > params.tolerance)
                continue;

            Demerits total = addDemerits(previous.demerits,
                    lineDemerits(previous, item, fit, params, final_break));
            int line_count = previous.line_count + 1;
            int match = -1;
            for (int i = 0; i < static_cast<int>(candidates.size()); i++) {
                if (candidates[i].line_count == line_count
                        && candidates[i].fitness == fit.fitness) {
                    match = i;
                    break;
                }
            }
            if (match < 0) {
                Candidate candidate = { previous_index, line_count, fit.fitness,
                                        fit.ratio_x1000, total };
                candidates.push_back(candidate);
            } else if (total < candidates[match].demerits) {
                candidates[match].previous = previous_index;
                candidates[match].ratio_x1000 = fit.ratio_x1000;
                candidates[match].demerits = total;
            }
        }

        active.swap(surviving);
        for (int i = 0; i < static_cast<int>(candidates.size()); i++) {
            Demerits minimum = candidates[i].demerits;
            for (int j = 0; j < static_cast<int>(candidates.size()); j++) {
                if (candidates[j].line_count == candidates[i].line_count
                        && candidates[j].demerits < minimum)
                    minimum = candidates[j].demerits;
            }
            if (candidates[i].demerits > addDemerits(minimum,
                                                     params.adj_demerits))
                continue;

            const Candidate & candidate = candidates[i];
            BreakNode node = { break_item, candidate.previous,
                               candidate.line_count, candidate.fitness,
                               candidate.ratio_x1000,
                               item.type == KPItem::PENALTY && item.flagged,
                               candidate.demerits };
            nodes.push_back(node);
            active.push_back(static_cast<int>(nodes.size()) - 1);
        }

        if (forced && active.empty())
            return -1;
        if (final_break) {
            final_nodes = active;
            break;
        }
    }

    if (final_nodes.empty())
        return -1;
    int best = final_nodes[0];
    for (int i = 1; i < static_cast<int>(final_nodes.size()); i++) {
        if (nodes[final_nodes[i]].demerits < nodes[best].demerits)
            best = final_nodes[i];
    }

    int line_count = nodes[best].line_count;
    if (line_count > max_out || (line_count > 0 && !out))
        return -1;
    int node_index = best;
    for (int i = line_count - 1; i >= 0; i--) {
        out[i].break_item = nodes[node_index].break_item;
        out[i].ratio_x1000 = nodes[node_index].ratio_x1000;
        node_index = nodes[node_index].previous;
    }
    return node_index == 0 ? line_count : -1;
}
