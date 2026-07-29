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
    KPItem item = { KPItem::BOX, width, 0, 0, 0, false, 0 };
    return item;
}

KPItem glue(int width, int stretch, int shrink)
{
    KPItem item = { KPItem::GLUE, width, stretch, shrink, 0, false, 0 };
    return item;
}

KPItem penalty(int width, int value, bool flagged = false)
{
    KPItem item = { KPItem::PENALTY, width, 0, 0, value, flagged, 0 };
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
    checkEdgeCases();
    std::puts("kp_selfcheck: ok");
    return 0;
}
