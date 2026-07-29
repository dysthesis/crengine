/** \file lvkplinebreak.h
    \brief dependency-free Knuth--Plass paragraph breaker

    CoolReader Engine

    This source code is distributed under the terms of
    GNU General Public License.

    See LICENSE file for details.
*/

#ifndef LVKPLINEBREAK_H_INCLUDED
#define LVKPLINEBREAK_H_INCLUDED

enum { KP_INFINITY = 10000 };

struct KPItem {
    enum Type { BOX, GLUE, PENALTY } type;
    int width;
    int stretch;
    int shrink;
    int penalty;
    bool flagged;
    int pos;
};

struct KPParams {
    int tolerance;
    int line_penalty;
    int adj_demerits;
    int double_hyphen_demerits;
    int final_hyphen_demerits;
    int emergency_stretch;

    KPParams()
        : tolerance(200), line_penalty(10), adj_demerits(10000)
        , double_hyphen_demerits(10000), final_hyphen_demerits(5000)
        , emergency_stretch(0)
    {
    }
};

struct KPLine {
    int break_item;
    int ratio_x1000;
};

/**
 * Find the minimum-demerit break sequence for one pass.
 *
 * Widths and glue components must be non-negative. The item list must end in a
 * forced penalty; callers normally precede it with zero-width infinitely
 * stretchable glue. Run separate no-hyphen, hyphen, and emergency-stretch
 * passes by changing the items and params between calls.
 *
 * Returns the line count, or -1 for invalid input, an infeasible paragraph, or
 * an output buffer that is too small. Empty input returns zero.
 */
int kp_break_paragraph(const KPItem * items, int n_items,
                       int first_line_width, int rest_width,
                       const KPParams & params, KPLine * out, int max_out);

#endif // LVKPLINEBREAK_H_INCLUDED
