#include "../crengine/include/lvkplinebreak.h"

// The self-check's assertions must survive release build flags.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Fit {
    int badness;
    int fitness;
    int ratio_x1000;
    bool overfull;
};

KPItem box(int width)
{
    KPItem item = { KPItem::BOX, width, 0, 0, 0, false, 0, 0, 0 };
    return item;
}

KPItem glue(int width, int stretch, int shrink)
{
    KPItem item = { KPItem::GLUE, width, stretch, shrink, 0, false, 0,
                    width, 0 };
    return item;
}

KPItem penalty(int width, int value, bool flagged = false)
{
    KPItem item = { KPItem::PENALTY, width, 0, 0, value, flagged, 0, 0, 0 };
    return item;
}

void finishParagraph(std::vector<KPItem> & items)
{
    items.push_back(penalty(0, KP_INFINITY));
    items.push_back(glue(0, 100000, 0));
    items.push_back(penalty(0, -KP_INFINITY));
}

int badness(std::int64_t amount, std::int64_t capacity)
{
    if (amount == 0)
        return 0;
    if (capacity == 0)
        return KP_INFINITY;
    typedef __int128 WideInt;
    WideInt a = amount;
    WideInt c = capacity;
    WideInt numerator = 100 * a * a * a;
    WideInt denominator = c * c * c;
    if (numerator >= static_cast<WideInt>(KP_INFINITY) * denominator)
        return KP_INFINITY;
    return static_cast<int>((numerator + denominator / 2) / denominator);
}

Fit lineFit(const std::vector<KPItem> & items, int previous_break,
            int break_item, int line_width, int emergency_stretch)
{
    int start = previous_break + 1;
    while (start < break_item && items[start].type != KPItem::BOX
            && !(items[start].type == KPItem::PENALTY
                 && items[start].penalty <= -KP_INFINITY))
        start++;

    std::int64_t natural = 0;
    std::int64_t stretch = 0;
    std::int64_t shrink = 0;
    for (int i = start; i < break_item; i++) {
        if (items[i].type == KPItem::BOX || items[i].type == KPItem::GLUE)
            natural += items[i].width;
        if (items[i].type == KPItem::GLUE) {
            stretch += items[i].stretch;
            shrink += items[i].shrink;
        }
    }
    if (items[break_item].type == KPItem::PENALTY)
        natural += items[break_item].width;

    std::int64_t shortfall = line_width - natural;
    Fit result;
    result.overfull = false;
    if (shortfall == 0) {
        result.badness = 0;
        result.fitness = 1;
        result.ratio_x1000 = 0;
    } else if (shortfall > 0) {
        std::int64_t capacity = stretch + emergency_stretch;
        result.badness = badness(shortfall, capacity);
        result.fitness = result.badness > 99 ? 3 : result.badness > 12 ? 2 : 1;
        result.ratio_x1000 = capacity ? static_cast<int>(shortfall * 1000 / capacity)
                                      : INT_MAX;
    } else {
        std::int64_t excess = -shortfall;
        result.overfull = shrink == 0 || excess > shrink;
        result.badness = result.overfull ? KP_INFINITY : badness(excess, shrink);
        result.fitness = result.badness > 12 ? 0 : 1;
        result.ratio_x1000 = shrink ? -static_cast<int>(excess * 1000 / shrink)
                                    : INT_MIN;
    }
    return result;
}

bool legalBreak(const std::vector<KPItem> & items, int index)
{
    if (items[index].type == KPItem::PENALTY)
        return items[index].penalty < KP_INFINITY;
    return items[index].type == KPItem::GLUE && index > 0
            && items[index - 1].type == KPItem::BOX;
}

bool calculateScore(const std::vector<KPItem> & items,
                    const std::vector<int> & breaks,
                    int first_width, int rest_width,
                    const KPParams & params, std::int64_t & score)
{
    score = 0;
    int previous_break = -1;
    int previous_fitness = 1;
    bool previous_flagged = false;
    for (int line = 0; line < static_cast<int>(breaks.size()); line++) {
        int break_item = breaks[line];
        Fit fit = lineFit(items, previous_break, break_item,
                          line == 0 ? first_width : rest_width,
                          params.emergency_stretch);
        if (fit.overfull || fit.badness > params.tolerance)
            return false;

        std::int64_t base = params.line_penalty + fit.badness;
        std::int64_t demerits = base >= KP_INFINITY ? 100000000 : base * base;
        const KPItem & item = items[break_item];
        int p = item.type == KPItem::PENALTY ? item.penalty : 0;
        if (p > 0)
            demerits += static_cast<std::int64_t>(p) * p;
        else if (p > -KP_INFINITY)
            demerits -= static_cast<std::int64_t>(p) * p;

        bool flagged = item.type == KPItem::PENALTY && item.flagged;
        bool final_break = break_item == static_cast<int>(items.size()) - 1;
        if (final_break && previous_flagged)
            demerits += params.final_hyphen_demerits;
        else if (flagged && previous_flagged)
            demerits += params.double_hyphen_demerits;
        if (fit.fitness - previous_fitness < -1
                || fit.fitness - previous_fitness > 1)
            demerits += params.adj_demerits;

        score += demerits;
        previous_break = break_item;
        previous_fitness = fit.fitness;
        previous_flagged = flagged;
    }
    return true;
}

bool isForcedBreak(const KPItem & item)
{
    return item.type == KPItem::PENALTY && item.penalty <= -KP_INFINITY;
}

bool minimumScore(const std::vector<KPItem> & items,
                  int first_width, int rest_width,
                  const KPParams & params, std::int64_t & minimum)
{
    int optional_count = 0;
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
        if (legalBreak(items, i) && !isForcedBreak(items[i]))
            optional_count++;
    }
    assert(optional_count < 63);

    bool found = false;
    const std::uint64_t combination_count =
            static_cast<std::uint64_t>(1) << optional_count;
    for (std::uint64_t mask = 0; mask < combination_count; mask++) {
        std::vector<int> breaks;
        int optional_index = 0;
        for (int i = 0; i < static_cast<int>(items.size()); i++) {
            if (!legalBreak(items, i))
                continue;
            if (isForcedBreak(items[i])
                    || (mask & (static_cast<std::uint64_t>(1)
                                << optional_index))) {
                breaks.push_back(i);
            }
            if (!isForcedBreak(items[i]))
                optional_index++;
        }

        std::int64_t score;
        if (calculateScore(items, breaks, first_width, rest_width, params, score)
                && (!found || score < minimum)) {
            minimum = score;
            found = true;
        }
    }
    return found;
}

void assertOptimal(const std::vector<KPItem> & items,
                   int first_width, int rest_width,
                   const KPParams & params)
{
    std::int64_t minimum;
    const bool feasible = minimumScore(items, first_width, rest_width,
                                       params, minimum);
    std::vector<KPLine> lines(items.size());
    const int count = kp_break_paragraph(&items[0], items.size(),
            first_width, rest_width, params, &lines[0], lines.size());
    if (!feasible) {
        assert(count == -1);
        return;
    }
    assert(count > 0);

    std::vector<int> actual;
    for (int i = 0; i < count; i++)
        actual.push_back(lines[i].break_item);
    std::int64_t actual_score;
    if (!calculateScore(items, actual, first_width, rest_width,
                        params, actual_score)
            || actual_score != minimum) {
        std::abort();
    }

    int actual_index = 0;
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
        if (!isForcedBreak(items[i]))
            continue;
        while (actual_index < count && actual[actual_index] < i)
            actual_index++;
        assert(actual_index < count && actual[actual_index] == i);
    }
}

struct SpacingOracleFit {
    bool feasible;
    bool has_ratio;
    int ratio_x1000;
};

struct SpacingOracleScore {
    int worst;
    std::int64_t adjacent;
    std::int64_t squared;
    std::int64_t breaks;

    SpacingOracleScore() : worst(0), adjacent(0), squared(0), breaks(0) {}
};

bool spacingScoreLess(const SpacingOracleScore & left,
                      const SpacingOracleScore & right)
{
    if (left.worst != right.worst)
        return left.worst < right.worst;
    if (left.adjacent != right.adjacent)
        return left.adjacent < right.adjacent;
    if (left.squared != right.squared)
        return left.squared < right.squared;
    return left.breaks < right.breaks;
}

bool isRaggedBreak(const std::vector<KPItem> & items, int index)
{
    if (index < 2 || !isForcedBreak(items[index]))
        return false;
    const KPItem & blocker = items[index - 2];
    const KPItem & fill = items[index - 1];
    return blocker.type == KPItem::PENALTY && blocker.penalty >= KP_INFINITY
            && fill.type == KPItem::GLUE && fill.width == 0
            && fill.shrink == 0 && fill.adjustable == 0;
}

SpacingOracleFit spacingLineFit(const std::vector<KPItem> & items,
                                int previous_break, int break_item,
                                int line_width)
{
    int start = previous_break + 1;
    while (start < break_item && items[start].type != KPItem::BOX
            && !isForcedBreak(items[start]))
        start++;

    std::int64_t natural = 0;
    std::int64_t adjustable = 0;
    std::int64_t stretch = 0;
    std::int64_t shrink = 0;
    for (int i = start; i < break_item; i++) {
        if (items[i].type == KPItem::BOX || items[i].type == KPItem::GLUE)
            natural += items[i].width;
        if (items[i].type == KPItem::GLUE) {
            adjustable += items[i].adjustable;
            stretch += items[i].stretch;
            shrink += items[i].shrink;
        }
    }
    if (items[break_item].type == KPItem::PENALTY)
        natural += items[break_item].width;

    std::int64_t target = line_width + items[break_item].protrusion;
    if (start < break_item && items[start].type == KPItem::BOX)
        target += items[start].protrusion;

    SpacingOracleFit result = { false, false, 0 };
    std::int64_t shortfall = target - natural;
    bool ragged = isRaggedBreak(items, break_item);
    if (shortfall == 0) {
        result.feasible = true;
        result.has_ratio = !ragged;
        return result;
    }
    if (shortfall > 0 && ragged) {
        result.feasible = true;
        return result;
    }

    std::int64_t amount = shortfall > 0 ? shortfall : -shortfall;
    std::int64_t capacity = shortfall > 0 ? stretch : shrink;
    if (adjustable == 0 || amount > capacity)
        return result;
    int magnitude = static_cast<int>((amount * KP_RATIO_SCALE
                                      + adjustable - 1) / adjustable);
    result.feasible = true;
    result.has_ratio = true;
    result.ratio_x1000 = shortfall > 0 ? magnitude : -magnitude;
    return result;
}

std::int64_t oracleBreakDemerits(const KPItem & item, bool previous_flagged,
                                 bool final_break,
                                 const KPSpacingParams & params)
{
    int value = item.type == KPItem::PENALTY ? item.penalty : 0;
    std::int64_t result = 0;
    if (value > 0)
        result = static_cast<std::int64_t>(value) * value;
    else if (value > -KP_INFINITY)
        result = -static_cast<std::int64_t>(value) * value;

    bool flagged = item.type == KPItem::PENALTY && item.flagged;
    if (final_break && previous_flagged)
        result += params.final_hyphen_demerits;
    else if (flagged && previous_flagged)
        result += params.double_hyphen_demerits;
    return result;
}

bool calculateSpacingScore(const std::vector<KPItem> & items,
                           const std::vector<int> & breaks,
                           int first_width, int rest_width,
                           const KPSpacingParams & params,
                           SpacingOracleScore & score,
                           std::vector<int> * ratios = NULL)
{
    score = SpacingOracleScore();
    if (ratios)
        ratios->clear();
    int previous_break = -1;
    int previous_ratio = 0;
    bool previous_has_ratio = false;
    bool previous_flagged = false;
    for (int line = 0; line < static_cast<int>(breaks.size()); line++) {
        int break_item = breaks[line];
        SpacingOracleFit fit = spacingLineFit(items, previous_break, break_item,
                line == 0 ? first_width : rest_width);
        if (!fit.feasible)
            return false;
        if (ratios)
            ratios->push_back(fit.has_ratio ? fit.ratio_x1000 : 0);

        if (fit.has_ratio) {
            int magnitude = fit.ratio_x1000 < 0 ? -fit.ratio_x1000
                                                : fit.ratio_x1000;
            if (magnitude > score.worst)
                score.worst = magnitude;
            if (previous_has_ratio) {
                std::int64_t difference = static_cast<std::int64_t>(
                        fit.ratio_x1000) - previous_ratio;
                score.adjacent += difference * difference;
            }
            score.squared += static_cast<std::int64_t>(fit.ratio_x1000)
                    * fit.ratio_x1000;
        }

        const KPItem & item = items[break_item];
        bool final_break = break_item == static_cast<int>(items.size()) - 1;
        score.breaks += oracleBreakDemerits(item, previous_flagged,
                                            final_break, params);
        previous_break = break_item;
        previous_ratio = fit.ratio_x1000;
        previous_has_ratio = fit.has_ratio;
        previous_flagged = item.type == KPItem::PENALTY && item.flagged;
    }
    return !breaks.empty()
            && breaks.back() == static_cast<int>(items.size()) - 1;
}

bool minimumSpacingScore(const std::vector<KPItem> & items,
                         int first_width, int rest_width,
                         const KPSpacingParams & params,
                         SpacingOracleScore & minimum)
{
    int optional_count = 0;
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
        if (legalBreak(items, i) && !isForcedBreak(items[i]))
            optional_count++;
    }
    assert(optional_count < 63);

    bool found = false;
    const std::uint64_t combination_count =
            static_cast<std::uint64_t>(1) << optional_count;
    for (std::uint64_t mask = 0; mask < combination_count; mask++) {
        std::vector<int> breaks;
        int optional_index = 0;
        for (int i = 0; i < static_cast<int>(items.size()); i++) {
            if (!legalBreak(items, i))
                continue;
            if (isForcedBreak(items[i])
                    || (mask & (static_cast<std::uint64_t>(1)
                                << optional_index)))
                breaks.push_back(i);
            if (!isForcedBreak(items[i]))
                optional_index++;
        }

        SpacingOracleScore score;
        if (calculateSpacingScore(items, breaks, first_width, rest_width,
                                  params, score)
                && (!found || spacingScoreLess(score, minimum))) {
            minimum = score;
            found = true;
        }
    }
    return found;
}

void assertSpacingOptimal(const std::vector<KPItem> & items,
                          int first_width, int rest_width,
                          const KPSpacingParams & params)
{
    SpacingOracleScore minimum;
    bool feasible = minimumSpacingScore(items, first_width, rest_width,
                                        params, minimum);
    std::vector<KPLine> lines(items.size());
    int count = kp_break_paragraph_spacing(&items[0], items.size(),
            first_width, rest_width, params, &lines[0], lines.size());
    if (!feasible) {
        assert(count == -1);
        return;
    }
    assert(count > 0);

    std::vector<int> actual;
    for (int i = 0; i < count; i++)
        actual.push_back(lines[i].break_item);
    SpacingOracleScore actual_score;
    std::vector<int> ratios;
    if (!calculateSpacingScore(items, actual, first_width, rest_width,
                               params, actual_score, &ratios)
            || spacingScoreLess(actual_score, minimum)
            || spacingScoreLess(minimum, actual_score))
        std::abort();
    assert(static_cast<int>(ratios.size()) == count);
    for (int i = 0; i < count; i++)
        assert(lines[i].ratio_x1000 == ratios[i]);
}

unsigned nextRandom(unsigned & state)
{
    state = state * 1664525U + 1013904223U;
    return state;
}

int draw(unsigned & state, int limit)
{
    return static_cast<int>(nextRandom(state) % static_cast<unsigned>(limit));
}

std::int64_t scoreBreaks(const std::vector<KPItem> & items,
                         const std::vector<int> & breaks,
                         int first_width, int rest_width,
                         const KPParams & params)
{
    std::int64_t score;
    if (!calculateScore(items, breaks, first_width, rest_width, params, score))
        std::abort();
    return score;
}

std::vector<int> greedyBreaks(const std::vector<KPItem> & items,
                              int first_width, int rest_width)
{
    std::vector<int> result;
    int previous_break = -1;
    while (previous_break != static_cast<int>(items.size()) - 1) {
        int chosen = -1;
        int line_width = result.empty() ? first_width : rest_width;
        for (int i = previous_break + 1; i < static_cast<int>(items.size()); i++) {
            if (!legalBreak(items, i))
                continue;
            Fit fit = lineFit(items, previous_break, i, line_width, 0);
            if (!fit.overfull)
                chosen = i;
            else if (chosen >= 0)
                break;
            if (items[i].type == KPItem::PENALTY
                    && items[i].penalty <= -KP_INFINITY)
                break;
        }
        assert(chosen >= 0);
        result.push_back(chosen);
        previous_break = chosen;
    }
    return result;
}

void checkHandcraftedParagraph()
{
    const int word_widths[] = { 3, 2, 2, 5, 2, 3 };
    std::vector<KPItem> items;
    for (int i = 0; i < 6; i++) {
        items.push_back(box(word_widths[i]));
        if (i != 5)
            items.push_back(glue(1, 1, 1));
    }
    finishParagraph(items);

    KPParams params;
    KPLine lines[8];
    int count = kp_break_paragraph(&items[0], items.size(), 8, 8,
                                   params, lines, 8);
    assert(count == 3);
    assert(lines[0].break_item == 5);
    assert(lines[1].break_item == 9);
    assert(lines[2].break_item == 13);

    KPLine untouched[2] = { { -7, -7 }, { -7, -7 } };
    assert(kp_break_paragraph(&items[0], items.size(), 8, 8,
                              params, untouched, 2) == -1);
    assert(untouched[0].break_item == -7 && untouched[1].break_item == -7);
}

int paperUnitWidth(char c)
{
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz";
    static const int lower_widths[] = {
        9, 10, 8, 10, 8, 6, 9, 10, 5, 6, 10, 5, 15,
        10, 9, 10, 10, 7, 7, 7, 10, 10, 13, 10, 10, 8
    };
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const int upper_widths[] = {
        14, 13, 13, 14, 12, 12, 14, 14, 7, 9, 14, 11, 17,
        14, 14, 12, 14, 13, 10, 13, 14, 14, 19, 14, 14, 11
    };
    const char * found = std::strchr(lower, c);
    if (found)
        return lower_widths[found - lower];
    found = std::strchr(upper, c);
    if (found)
        return upper_widths[found - upper];
    if (c == ',' || c == '.' || c == ';' || c == '\'')
        return 5;
    if (c == '-')
        return 6;
    assert(false);
    return 0;
}

void checkPaperParagraph()
{
    // Figure 1's 1/18-em units: 18-unit indent, glue (6, 3, 2), measure 421.
    static const char text[] =
        "In olden times when wishing still helped one, there lived a king whose "
        "daughters were all beautiful; and the youngest was so beautiful that "
        "the sun itself, which has seen so much, was astonished whenever it "
        "shone in her face. Close by the king's castle lay a great dark forest, "
        "and under an old lime-tree in the forest was a well, and when the day "
        "was very warm, the king's child went out into the forest and sat down "
        "by the side of the cool fountain; and when she was bored she took a "
        "golden ball, and threw it up on high and caught it; and this ball was "
        "her favorite plaything.";

    std::istringstream stream(text);
    std::vector<int> word_widths;
    std::string word;
    while (stream >> word) {
        int width = 0;
        for (std::string::const_iterator it = word.begin(); it != word.end(); ++it)
            width += paperUnitWidth(*it);
        word_widths.push_back(width);
    }
    word_widths[0] += 18;

    std::vector<KPItem> items;
    for (int i = 0; i < static_cast<int>(word_widths.size()); i++) {
        items.push_back(box(word_widths[i]));
        if (i + 1 != static_cast<int>(word_widths.size()))
            items.push_back(glue(6, 3, 2));
    }
    finishParagraph(items);

    KPParams params;
    params.tolerance = 100;
    KPLine lines[128];
    int count = kp_break_paragraph(&items[0], items.size(), 421, 421,
                                   params, lines, 128);
    assert(count > 0);

    std::vector<int> optimal;
    for (int i = 0; i < count; i++) {
        assert(lines[i].ratio_x1000 >= -1000);
        assert(lines[i].ratio_x1000 <= 1000);
        optimal.push_back(lines[i].break_item);
    }
    std::vector<int> greedy = greedyBreaks(items, 421, 421);
    assert(scoreBreaks(items, optimal, 421, 421, params)
           <= scoreBreaks(items, greedy, 421, 421, params));
}

void checkExhaustiveOptimality()
{
    const int tolerances[] = { 100, 200 };
    for (int first_word = 1; first_word <= 4; first_word++) {
        for (int second_word = 1; second_word <= 4; second_word++) {
            for (int third_word = 1; third_word <= 4; third_word++) {
                for (int glue_width = 1; glue_width <= 2; glue_width++) {
                    for (int stretch = 1; stretch <= 2; stretch++) {
                        for (int shrink = 0; shrink <= 1; shrink++) {
                            std::vector<KPItem> items;
                            items.push_back(box(first_word));
                            items.push_back(glue(glue_width, stretch, shrink));
                            items.push_back(box(second_word));
                            items.push_back(glue(glue_width, stretch, shrink));
                            items.push_back(box(third_word));
                            finishParagraph(items);

                            for (int first_width = 3; first_width <= 10; first_width++) {
                                for (int rest_width = 3; rest_width <= 10; rest_width++) {
                                    for (int tolerance_index = 0; tolerance_index < 2;
                                            tolerance_index++) {
                                        KPParams params;
                                        params.tolerance = tolerances[tolerance_index];
                                        std::int64_t optimum = INT64_MAX;
                                        for (int mask = 0; mask < 4; mask++) {
                                            std::vector<int> breaks;
                                            if (mask & 1)
                                                breaks.push_back(1);
                                            if (mask & 2)
                                                breaks.push_back(3);
                                            breaks.push_back(7);
                                            std::int64_t score;
                                            if (calculateScore(items, breaks, first_width,
                                                               rest_width, params, score)
                                                    && score < optimum)
                                                optimum = score;
                                        }

                                        KPLine lines[4];
                                        int count = kp_break_paragraph(&items[0], items.size(),
                                                first_width, rest_width, params, lines, 4);
                                        if (optimum == INT64_MAX) {
                                            assert(count == -1);
                                        } else {
                                            assert(count > 0);
                                            std::vector<int> actual;
                                            for (int i = 0; i < count; i++)
                                                actual.push_back(lines[i].break_item);
                                            std::int64_t actual_score;
                                            if (!calculateScore(items, actual, first_width,
                                                                rest_width, params, actual_score)
                                                    || actual_score != optimum)
                                                std::abort();
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

void checkGeneratedOptimality()
{
    static const int penalties[] = { -500, 0, 50, 5000 };
    static const int tolerances[] = { 100, 200, KP_INFINITY };
    unsigned state = 0x4b505f31U;

    for (int test = 0; test < 5000; test++) {
        std::vector<KPItem> items;
        const int word_count = 1 + draw(state, 5);
        for (int word = 0; word < word_count; word++) {
            items.push_back(box(1 + draw(state, 12)));
            if (word == word_count - 1)
                continue;

            switch (draw(state, 5)) {
            case 0:
                items.push_back(glue(1 + draw(state, 3),
                                     1 + draw(state, 4), draw(state, 3)));
                break;
            case 1:
                items.push_back(penalty(draw(state, 3),
                        penalties[draw(state, 4)], draw(state, 2) != 0));
                break;
            case 2:
                items.push_back(penalty(0, 5000));
                items.push_back(glue(1 + draw(state, 3),
                                     1 + draw(state, 4), draw(state, 3)));
                break;
            case 3:
                items.push_back(penalty(0, 0));
                break;
            default:
                items.push_back(penalty(0, KP_INFINITY));
                items.push_back(glue(0, 100000, 0));
                items.push_back(penalty(0, -KP_INFINITY));
                break;
            }
        }
        finishParagraph(items);

        KPParams params;
        params.tolerance = tolerances[draw(state, 3)];
        params.line_penalty = draw(state, 21);
        params.adj_demerits = draw(state, 3) == 0 ? 0 : 10000;
        params.double_hyphen_demerits = draw(state, 3) == 0 ? 0 : 10000;
        params.final_hyphen_demerits = draw(state, 3) == 0 ? 0 : 5000;
        params.emergency_stretch = draw(state, 7);
        assertOptimal(items, 3 + draw(state, 18), 3 + draw(state, 18), params);
    }
}

void checkSpacingObjective()
{
    KPSpacingParams params;
    KPLine lines[8];

    // A legal hyphen beats the feasible ordinary break on spacing quality.
    std::vector<KPItem> mixed;
    mixed.push_back(box(4));
    mixed.push_back(glue(2, 2, 2));
    mixed.push_back(box(4));
    mixed.push_back(glue(2, 2, 2));
    mixed.push_back(box(4));
    mixed.push_back(penalty(1, 50, true));
    mixed.push_back(box(4));
    mixed.push_back(glue(2, 2, 2));
    mixed.push_back(box(4));
    mixed.push_back(glue(2, 2, 2));
    mixed.push_back(box(4));
    finishParagraph(mixed);
    int count = kp_break_paragraph_spacing(&mixed[0], mixed.size(), 17, 17,
                                            params, lines, 8);
    assert(count == 2 && lines[0].break_item == 5);
    assert(lines[0].ratio_x1000 == 0 && lines[1].ratio_x1000 == 0);
    assertSpacingOptimal(mixed, 17, 17, params);

    // The bound rounds away from zero and capacities remain hard limits.
    std::vector<KPItem> bounded;
    bounded.push_back(box(3));
    bounded.push_back(glue(3, 1, 1));
    bounded.push_back(box(3));
    bounded.push_back(glue(3, 1, 1));
    bounded.push_back(box(3));
    finishParagraph(bounded);
    count = kp_break_paragraph_spacing(&bounded[0], bounded.size(), 10, 10,
                                       params, lines, 8);
    assert(count == 2 && lines[0].break_item == 3);
    assert(lines[0].ratio_x1000 == 334 && lines[1].ratio_x1000 == 0);
    assert(kp_break_paragraph_spacing(&bounded[0], bounded.size(), 11, 11,
                                      params, lines, 8) == -1);
    count = kp_break_paragraph_spacing(&bounded[0], bounded.size(), 8, 8,
                                       params, lines, 8);
    assert(count == 2 && lines[0].ratio_x1000 == -334);
    assert(kp_break_paragraph_spacing(&bounded[0], bounded.size(), 7, 7,
                                      params, lines, 8) == -1);

    std::vector<KPItem> no_space;
    no_space.push_back(box(3));
    no_space.push_back(penalty(0, 0));
    no_space.push_back(box(3));
    finishParagraph(no_space);
    assert(kp_break_paragraph_spacing(&no_space[0], no_space.size(), 3, 3,
                                      params, lines, 8) == 2);
    assert(kp_break_paragraph_spacing(&no_space[0], no_space.size(), 4, 4,
                                      params, lines, 8) == -1);

    KPLine untouched[1] = { { -7, -7 } };
    assert(kp_break_paragraph_spacing(&mixed[0], mixed.size(), 17, 17,
                                      params, untouched, 1) == -1);
    assert(untouched[0].break_item == -7 && untouched[0].ratio_x1000 == -7);
}

void checkGeneratedSpacingOptimality()
{
    static const int penalties[] = { -500, 0, 50, 5000 };
    unsigned state = 0x4b505f32U;
    for (int test = 0; test < 5000; test++) {
        std::vector<KPItem> items;
        int word_count = 1 + draw(state, 5);
        for (int word = 0; word < word_count; word++) {
            items.push_back(box(1 + draw(state, 12)));
            if (word == word_count - 1)
                continue;

            switch (draw(state, 5)) {
            case 0: {
                int width = 1 + draw(state, 4);
                items.push_back(glue(width, draw(state, width + 1),
                                     draw(state, width + 1)));
                break;
            }
            case 1:
                items.push_back(penalty(draw(state, 3),
                        penalties[draw(state, 4)], draw(state, 2) != 0));
                break;
            case 2: {
                items.push_back(penalty(0, KP_INFINITY));
                int width = 1 + draw(state, 4);
                items.push_back(glue(width, draw(state, width + 1),
                                     draw(state, width + 1)));
                break;
            }
            case 3:
                items.push_back(penalty(0, 0));
                break;
            default:
                items.push_back(penalty(0, KP_INFINITY));
                items.push_back(glue(0, 100000, 0));
                items.push_back(penalty(0, -KP_INFINITY));
                break;
            }
        }
        finishParagraph(items);
        // Hanging punctuation is worth a small fraction of a glyph, and turns
        // negative when a glyph overflows further than the margin allows.
        for (std::size_t i = 0; i < items.size(); i++)
            items[i].protrusion = draw(state, 5) - 1;

        KPSpacingParams params;
        params.double_hyphen_demerits = draw(state, 3) == 0 ? 0 : 10000;
        params.final_hyphen_demerits = draw(state, 3) == 0 ? 0 : 5000;
        assertSpacingOptimal(items, 3 + draw(state, 18),
                             3 + draw(state, 18), params);
    }
}

void checkEdgeCases()
{
    KPParams params;
    KPLine lines[8];

    std::vector<KPItem> forced;
    forced.push_back(box(3));
    forced.push_back(penalty(0, -KP_INFINITY));
    forced.push_back(box(3));
    finishParagraph(forced);
    int count = kp_break_paragraph(&forced[0], forced.size(), 3, 3,
                                   params, lines, 8);
    assert(count == 2 && lines[0].break_item == 1 && lines[1].break_item == 5);

    std::vector<KPItem> shaped;
    shaped.push_back(box(3));
    shaped.push_back(glue(1, 1, 1));
    shaped.push_back(box(3));
    shaped.push_back(glue(1, 1, 1));
    shaped.push_back(box(3));
    finishParagraph(shaped);
    count = kp_break_paragraph(&shaped[0], shaped.size(), 7, 3,
                               params, lines, 8);
    assert(count == 2 && lines[0].break_item == 3 && lines[1].break_item == 7);

    std::vector<KPItem> single;
    single.push_back(box(7));
    finishParagraph(single);
    assert(kp_break_paragraph(&single[0], single.size(), 10, 10,
                              params, lines, 8) == 1);

    std::vector<KPItem> breakOnlyWidth;
    breakOnlyWidth.push_back(box(4));
    breakOnlyWidth.push_back(penalty(2, 0));
    breakOnlyWidth.push_back(box(1));
    finishParagraph(breakOnlyWidth);
    count = kp_break_paragraph(&breakOnlyWidth[0], breakOnlyWidth.size(), 5, 5,
                               params, lines, 8);
    assert(count == 1 && lines[0].break_item == 5);

    std::vector<KPItem> unbreakable;
    unbreakable.push_back(box(20));
    finishParagraph(unbreakable);
    assert(kp_break_paragraph(&unbreakable[0], unbreakable.size(), 10, 10,
                              params, lines, 8) == -1);

    assert(kp_break_paragraph(NULL, 0, 10, 10, params, NULL, 0) == 0);

    std::vector<KPItem> impossible;
    impossible.push_back(box(3));
    impossible.push_back(glue(1, 1, 0));
    impossible.push_back(box(8));
    finishParagraph(impossible);
    params.tolerance = 100;
    assert(kp_break_paragraph(&impossible[0], impossible.size(), 10, 10,
                              params, lines, 8) == -1);
    params.emergency_stretch = 7;
    assert(kp_break_paragraph(&impossible[0], impossible.size(), 10, 10,
                              params, lines, 8) == 2);
}

} // namespace

int main()
{
    checkHandcraftedParagraph();
    checkPaperParagraph();
    checkExhaustiveOptimality();
    checkGeneratedOptimality();
    checkSpacingObjective();
    checkGeneratedSpacingOptimality();
    checkEdgeCases();
    std::puts("kp_selfcheck: ok");
    return 0;
}
