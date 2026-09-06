#include "crengine.h"
#include "hyphman.h"
#include "lvtextfm.h"
#include "textlang.h"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

struct Options {
    std::string font;
    std::string font_face;
    std::string hyph_dir;
    std::string input = "-";
    int font_size = 24;
    int width_first = 320;
    int width_last = 1000;
    int width_step = 8;
    std::string optimal = "both";
    bool selfcheck = false;
};

std::string quoted(const std::string & value)
{
    std::ostringstream out;
    out << '"';
    static const char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20)
                out << "\\u00" << hex[c >> 4] << hex[c & 15];
            else
                out << c;
        }
    }
    return out.str() + '"';
}

bool validParagraph(const std::string & text)
{
    const unsigned char * p = reinterpret_cast<const unsigned char *>(text.data());
    const unsigned char * end = p + text.size();
    while (p < end) {
        unsigned char c = *p++;
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7F)
                return false;
            continue;
        }
        int tails = c >= 0xC2 && c <= 0xDF ? 1 :
                    c >= 0xE0 && c <= 0xEF ? 2 :
                    c >= 0xF0 && c <= 0xF4 ? 3 : -1;
        if (tails < 0 || end - p < tails)
            return false;
        if ((c == 0xE0 && p[0] < 0xA0) || (c == 0xED && p[0] >= 0xA0) ||
                (c == 0xF0 && p[0] < 0x90) || (c == 0xF4 && p[0] >= 0x90))
            return false;
        std::uint32_t codepoint = c & (0x7F >> tails);
        while (tails--) {
            codepoint = (codepoint << 6) | (*p & 0x3F);
            if ((*p++ & 0xC0) != 0x80)
                return false;
        }
        if (codepoint >= 0x80 && codepoint <= 0x9F)
            return false;
    }
    return true;
}

std::uint64_t paragraphHash(const std::string & text)
{
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char c : text) {
        hash ^= c;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

std::string hexId(std::uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (int i = 15; i >= 0; --i) {
        result[i] = digits[value & 15];
        value >>= 4;
    }
    return result;
}

#if defined(CR_KP_FORMATTER_TRACE)
const char * reasonName(int reason)
{
    static const char * names[] = {
        "none", "disabled", "empty", "preformatted", "cjk", "float",
        "alignment", "justified_final", "variable_width", "unsupported_source",
        "bad_width", "no_solution", "invalid_solution", "space_count",
        "no_adjustable_space", "insufficient_capacity", "apportionment"
    };
    return reason >= 0 && reason < static_cast<int>(sizeof(names) / sizeof(names[0])) ?
            names[reason] : "unknown";
}

struct TraceCapture {
    std::vector<std::string> events;
    std::vector<int> successful_pass_breaks;
    int eligibility_count = 0;
    bool selection_seen = false;
    bool contract_valid = true;
    bool shrinking_ragged_seen = false;
    int allocation_count = 0;
    bool allocation_failure = false;

    static void callback(const KPFormatterTraceEvent * event, void * userdata)
    {
        static_cast<TraceCapture *>(userdata)->append(*event);
    }

    void append(const KPFormatterTraceEvent & event)
    {
        if (event.kind == KP_TRACE_ELIGIBILITY)
            ++eligibility_count;
        if (event.kind == KP_TRACE_PASS && event.success) {
            successful_pass_breaks.clear();
            for (int i = 0; i < event.break_count; ++i)
                successful_pass_breaks.push_back(event.breaks[i].source_pos);
        }
        if (event.kind == KP_TRACE_SELECTION) {
            selection_seen = true;
            if (event.break_count != static_cast<int>(successful_pass_breaks.size()))
                contract_valid = false;
            for (int i = 0; i < event.break_count; ++i) {
                if (i >= static_cast<int>(successful_pass_breaks.size()) ||
                        event.breaks[i].source_pos != successful_pass_breaks[i])
                    contract_valid = false;
                if (event.breaks[i].ragged && event.breaks[i].scored &&
                        event.breaks[i].ratio_x1000 < 0)
                    shrinking_ragged_seen = true;
            }
        }
        if (event.kind == KP_TRACE_ALLOCATION) {
            ++allocation_count;
            if (!event.success)
                allocation_failure = true;
            if (event.requested_adjustment !=
                    event.applied_adjustment + event.final_residual)
                contract_valid = false;
            if (event.success) {
                int applied = 0;
                for (int i = 0; i < event.space_count; ++i) {
                    if (!event.adjustments || !event.word_x)
                        contract_valid = false;
                    else
                        applied += event.adjustments[i];
                }
                if (applied != event.applied_adjustment)
                    contract_valid = false;
            }
        }
        std::ostringstream out;
        const char * kinds[] = { "eligibility", "pass", "selection", "allocation" };
        out << "{\"kind\":" << quoted(kinds[event.kind])
            << ",\"reason\":" << quoted(reasonName(event.reason))
            << ",\"paragraph_source\":[" << event.paragraph_start << ','
            << event.paragraph_end << ']';
        if (event.kind == KP_TRACE_ELIGIBILITY) {
            out << ",\"enabled\":" << (event.enabled ? "true" : "false")
                << ",\"eligible\":" << (event.eligible ? "true" : "false");
        }
        if (event.kind == KP_TRACE_PASS || event.kind == KP_TRACE_SELECTION) {
            out << ",\"first_width\":" << event.first_width
                << ",\"rest_width\":" << event.rest_width;
        }
        if (event.kind == KP_TRACE_PASS) {
            out << ",\"deprecated\":" << (event.deprecated_pass ? "true" : "false")
                << ",\"success\":" << (event.success ? "true" : "false")
                << ",\"items\":[";
            for (int i = 0; i < event.item_count; ++i) {
                if (i) out << ',';
                const KPItem & item = event.items[i];
                out << "{\"index\":" << i << ",\"type\":" << item.type
                    << ",\"width\":" << item.width << ",\"stretch\":" << item.stretch
                    << ",\"shrink\":" << item.shrink << ",\"penalty\":" << item.penalty
                    << ",\"flagged\":" << (item.flagged ? "true" : "false")
                    << ",\"source_pos\":" << item.pos
                    << ",\"adjustable\":" << item.adjustable
                    << ",\"protrusion\":" << item.protrusion << '}';
            }
            out << ']';
        }
        if (event.kind == KP_TRACE_PASS || event.kind == KP_TRACE_SELECTION) {
            out << ",\"breaks\":[";
            for (int i = 0; i < event.break_count; ++i) {
                if (i) out << ',';
                const KPFormatterTraceBreak & line = event.breaks[i];
                out << "{\"line\":" << i;
                if (event.kind == KP_TRACE_PASS)
                    out << ",\"item_index\":" << line.item_index;
                out << ",\"source_pos\":" << line.source_pos << ",\"ratio_x1000\":";
                if (line.scored) out << line.ratio_x1000; else out << "null";
                if (event.kind == KP_TRACE_PASS)
                    out << ",\"natural_width\":" << line.natural_width
                        << ",\"stretch\":" << line.stretch
                        << ",\"shrink\":" << line.shrink
                        << ",\"adjustable\":" << line.adjustable
                        << ",\"ragged_fill_stretch\":" << line.ragged_fill_stretch;
                out << ",\"target_width\":" << line.target_width
                    << ",\"hang_left\":" << line.hang_left
                    << ",\"hang_right\":" << line.hang_right
                    << ",\"scored\":" << (line.scored ? "true" : "false")
                    << ",\"ragged\":" << (line.ragged ? "true" : "false") << '}';
            }
            out << ']';
        }
        if (event.kind == KP_TRACE_ALLOCATION) {
            out << ",\"line\":" << event.line_index
                << ",\"gap_geometry\":\"advance_space\""
                << ",\"success\":" << (event.success ? "true" : "false")
                << ",\"requested\":" << event.requested_adjustment
                << ",\"applied\":" << event.applied_adjustment
                << ",\"final_residual\":" << event.final_residual
                << ",\"gaps\":[";
            for (int i = 0; i < event.space_count; ++i) {
                if (i) out << ',';
                out << "{\"word\":" << i << ",\"natural\":"
                    << (event.natural_spaces ? std::to_string(event.natural_spaces[i]) : "null")
                    << ",\"capacity\":"
                    << (event.capacities ? std::to_string(event.capacities[i]) : "null")
                    << ",\"adjustment\":"
                    << (event.adjustments ? std::to_string(event.adjustments[i]) : "null")
                    << ",\"word_x\":"
                    << (event.word_x ? std::to_string(event.word_x[i]) : "null")
                    << ",\"gap_start\":";
                bool hasInterval = event.natural_spaces && event.natural_spaces[i] > 0 &&
                        event.word_x && event.word_widths && i + 1 < event.space_count;
                if (hasInterval)
                    out << event.word_x[i] + event.word_widths[i] -
                            event.natural_spaces[i];
                else
                    out << "null";
                out << ",\"gap_end\":";
                if (hasInterval)
                    out << event.word_x[i + 1];
                else
                    out << "null";
                out << '}';
            }
            out << ']';
        }
        out << '}';
        events.push_back(out.str());
    }
};
#endif

int parseInteger(const std::string & value, const char * name, int minimum, int maximum)
{
    int result = 0;
    std::from_chars_result parsed = std::from_chars(value.data(),
            value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc() ||
            parsed.ptr != value.data() + value.size() ||
            result < minimum || result > maximum)
        throw std::runtime_error(std::string("invalid ") + name);
    return result;
}

void parseWidths(const std::string & spec, Options & options)
{
    std::size_t first = spec.find(':');
    std::size_t second = first == std::string::npos ? first : spec.find(':', first + 1);
    if (first == std::string::npos || second == std::string::npos ||
            spec.find(':', second + 1) != std::string::npos)
        throw std::runtime_error("invalid --widths (expected FIRST:LAST:STEP)");
    options.width_first = parseInteger(spec.substr(0, first), "width start", 1, 65535);
    options.width_last = parseInteger(spec.substr(first + 1, second - first - 1),
                                      "width end", 1, 65535);
    options.width_step = parseInteger(spec.substr(second + 1), "width step", 1, 65535);
    if (options.width_last < options.width_first)
        throw std::runtime_error("width end precedes width start");
}

Options parseOptions(int argc, char ** argv)
{
    Options options;
    bool input_set = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        auto value = [&](const char * name) -> std::string {
            if (++i >= argc) throw std::runtime_error(std::string("missing ") + name);
            return argv[i];
        };
        if (arg == "--font") options.font = value("font path");
        else if (arg == "--font-face") options.font_face = value("font face");
        else if (arg == "--hyph-dir") options.hyph_dir = value("hyphen directory");
        else if (arg == "--font-size")
            options.font_size = parseInteger(value("font size"), "font size", 1, 255);
        else if (arg == "--widths") parseWidths(value("width range"), options);
        else if (arg == "--optimal") options.optimal = value("optimal mode");
        else if (arg == "--selfcheck") options.selfcheck = true;
        else if (arg == "-" || (!arg.empty() && arg[0] != '-')) {
            if (input_set)
                throw std::runtime_error("multiple input paths");
            options.input = arg;
            input_set = true;
        }
        else throw std::runtime_error("unknown option: " + arg);
    }
    if (options.font.empty() || options.font_face.empty() || options.hyph_dir.empty())
        throw std::runtime_error("--font, --font-face, and --hyph-dir are required");
    if (options.optimal != "both" && options.optimal != "on" && options.optimal != "off")
        throw std::runtime_error("--optimal must be both, on, or off");
    return options;
}

void emitRecord(const std::string & text, std::size_t paragraph_index, int width,
                bool optimal, LVFontRef font, TextLangCfg * lang)
{
    LFormattedText formatted;
    formatted.setOptimalLineBreaking(optimal);
    formatted.setSpaceWidthScalePercent(95);
    formatted.setMinSpaceCondensingPercent(75);
    formatted.setUnusedSpaceThresholdPercent(5);
    formatted.setMaxAddedLetterSpacingPercent(0);
    lString32 source = Utf8ToUnicode(text.c_str(), static_cast<int>(text.size()));
#if defined(CR_KP_FORMATTER_TRACE)
    TraceCapture capture;
    formatted.setKPFormatterTrace(TraceCapture::callback, &capture);
#endif
    formatted.AddSourceLine(source.c_str(), source.length(), 0, 0xFFFFFF, font.get(), lang,
            LTEXT_ALIGN_WIDTH | LTEXT_LAST_LINE_ALIGN_LEFT | LTEXT_FLAG_OWNTEXT |
                LTEXT_HYPHENATE,
            font->getHeight(), 0, 0);
    formatted.setStrut(font->getHeight(), font->getBaseline());
    formatted.Format(width, 65535, 1, 0, 0, true);

    std::string paragraph_id = "p" + std::to_string(paragraph_index) + "-" +
            hexId(paragraphHash(text));
    std::ostringstream out;
    out << "{\"schema\":\"kp-formatter-trace-v1\",\"paragraph_id\":"
        << quoted(paragraph_id) << ",\"paragraph_index\":" << paragraph_index
        << ",\"config_id\":" << quoted(paragraph_id + ":w" + std::to_string(width) +
                (optimal ? ":optimal" : ":greedy"))
        << ",\"width\":" << width << ",\"optimal_enabled\":"
        << (optimal ? "true" : "false")
        << ",\"settings\":{\"space_width_percent\":95,\"min_space_percent\":75,"
           "\"unused_space_percent\":5,\"max_letter_spacing_percent\":0,"
           "\"hanging_punctuation\":true,\"language\":\"en-US\","
           "\"hyphen_method\":\"English_US.pattern\",\"font_face\":"
        << quoted(font->getTypeFace().c_str()) << ",\"font_size\":" << font->getSize()
        << "},\"trace\":";
#if defined(CR_KP_FORMATTER_TRACE)
    out << '[';
    for (std::size_t i = 0; i < capture.events.size(); ++i) {
        if (i) out << ',';
        out << capture.events[i];
    }
    out << ']';
#else
    out << "null";
#endif
    out << ",\"height\":" << formatted.GetBuffer()->height << ",\"lines\":[";
    for (int i = 0; i < formatted.GetLineCount(); ++i) {
        if (i) out << ',';
        const formatted_line_t * line = formatted.GetLineInfo(i);
        out << "{\"index\":" << i << ",\"x\":" << line->x << ",\"y\":" << line->y
            << ",\"width\":" << line->width << ",\"height\":" << line->height
            << ",\"baseline\":" << line->baseline
            << ",\"width_overflow\":" << line->width_overflow
            << ",\"align\":" << static_cast<int>(line->align)
            << ",\"flags\":" << static_cast<int>(line->flags) << ",\"words\":[";
        for (int j = 0; j < line->word_count; ++j) {
            if (j) out << ',';
            const formatted_word_t & word = line->words[j];
            out << "{\"index\":" << j << ",\"source_index\":" << word.src_text_index
                << ",\"source_start\":" << word.t.start
                << ",\"source_end\":" << word.t.start + word.t.len
                << ",\"source_length\":" << word.t.len << ",\"x\":" << word.x
                << ",\"absolute_x\":" << line->x + word.x << ",\"y\":" << word.y
                << ",\"width\":" << word.width << ",\"min_width\":" << word.min_width
                << ",\"flags\":" << word.flags
                << ",\"added_letter_spacing\":" << word.added_letter_spacing
                << ",\"distinct_glyphs\":" << word.distinct_glyphs << '}';
        }
        out << "]}";
    }
    std::cout << out.str() << "]}\n";
    if (!std::cout)
        throw std::runtime_error("failed to write output");
}

void selfcheck(LVFontRef font, TextLangCfg * lang)
{
    if (quoted("a\n\"") != "\"a\\n\\\"\"" || !validParagraph("\xC2\xA3") ||
            validParagraph("\xC0\x80") || validParagraph("bad\ttext") ||
            paragraphHash("abc") != UINT64_C(0xe71fa2190541574b))
        throw std::runtime_error("collector primitive selfcheck failed");
    LFormattedText formatted;
    lString32 source = U"Hyphenation demonstrates genuine formatter geometry across several words.";
    formatted.setOptimalLineBreaking(true);
    formatted.setSpaceWidthScalePercent(95);
    formatted.setMinSpaceCondensingPercent(75);
    formatted.AddSourceLine(source.c_str(), source.length(), 0, 0xFFFFFF, font.get(), lang,
            LTEXT_ALIGN_WIDTH | LTEXT_LAST_LINE_ALIGN_LEFT | LTEXT_FLAG_OWNTEXT |
                LTEXT_HYPHENATE,
            font->getHeight());
    formatted.setStrut(font->getHeight(), font->getBaseline());
#if defined(CR_KP_FORMATTER_TRACE)
    TraceCapture capture;
    formatted.setKPFormatterTrace(TraceCapture::callback, &capture);
#endif
    formatted.Format(180, 65535, 1);
    if (formatted.GetLineCount() < 2)
        throw std::runtime_error("formatter selfcheck did not wrap");
    for (int i = 0; i < formatted.GetLineCount(); ++i) {
        const formatted_line_t * line = formatted.GetLineInfo(i);
        for (int j = 0; j < line->word_count; ++j) {
            const formatted_word_t & word = line->words[j];
            if (word.src_text_index != 0 || word.t.start + word.t.len > source.length() ||
                    (j && word.x < line->words[j - 1].x))
                throw std::runtime_error("formatter geometry selfcheck failed");
        }
    }
#if defined(CR_KP_FORMATTER_TRACE)
    if (capture.events.empty() || capture.eligibility_count != 1 ||
            !capture.selection_seen || !capture.contract_valid ||
            capture.allocation_failure)
        throw std::runtime_error("trace event contract selfcheck failed");
    LFormattedText probe;
    probe.setSpaceWidthScalePercent(95);
    probe.setMinSpaceCondensingPercent(75);
    probe.AddSourceLine(source.c_str(), source.length(), 0, 0xFFFFFF,
            font.get(), lang, LTEXT_ALIGN_WIDTH | LTEXT_LAST_LINE_ALIGN_LEFT |
                LTEXT_FLAG_OWNTEXT | LTEXT_HYPHENATE,
            font->getHeight());
    probe.setStrut(font->getHeight(), font->getBaseline());
    probe.Format(2000, 65535, 1);
    if (probe.GetLineCount() != 1 || probe.GetLineInfo(0)->width <= 1)
        throw std::runtime_error("trace shrink probe selfcheck failed");

    LFormattedText trial;
    TraceCapture trialCapture;
    trial.setKPFormatterTrace(TraceCapture::callback, &trialCapture);
    trial.setOptimalLineBreaking(true);
    trial.setSpaceWidthScalePercent(95);
    trial.setMinSpaceCondensingPercent(75);
    trial.AddSourceLine(source.c_str(), source.length(), 0, 0xFFFFFF,
            font.get(), lang, LTEXT_ALIGN_WIDTH | LTEXT_LAST_LINE_ALIGN_LEFT |
                LTEXT_FLAG_OWNTEXT | LTEXT_HYPHENATE,
            font->getHeight());
    trial.setStrut(font->getHeight(), font->getBaseline());
    trial.Format(probe.GetLineInfo(0)->width - 1, 65535, 1);
    if (!trialCapture.contract_valid || !trialCapture.selection_seen ||
            !trialCapture.shrinking_ragged_seen ||
            trialCapture.allocation_count == 0 || trialCapture.allocation_failure)
        throw std::runtime_error("trace selfcheck found no shrinking ragged final");
#endif
    std::cerr << "kp_formatter_trace selfcheck: ok\n";
}

} // namespace

int main(int argc, char ** argv)
{
    try {
        Options options = parseOptions(argc, argv);
        if (!InitFontManager(lString8::empty_str) || !fontMan->RegisterFont(lString8(options.font.c_str())))
            throw std::runtime_error("failed to initialise/register font");
        fontMan->SetKerningMode(KERNING_MODE_HARFBUZZ);
        LVFontRef font = fontMan->GetFont(options.font_size, 400, false, css_ff_serif,
                                          lString8(options.font_face.c_str()));
        if (font.isNull())
            throw std::runtime_error("failed to load requested font");
        if (font->getTypeFace() != lString8(options.font_face.c_str()))
            throw std::runtime_error("requested font face resolved to " +
                    std::string(font->getTypeFace().c_str()));
        lString32 hyphDir = Utf8ToUnicode(options.hyph_dir.c_str());
        LVAppendPathDelimiter(hyphDir);
        if (!HyphMan::initDictionaries(hyphDir))
            throw std::runtime_error("failed to load shipped hyphenation patterns");
        TextLangMan::setMainLang(U"en-US");
        TextLangMan::setHyphenationEnabled(true);
        TextLangMan::setHyphenationSoftHyphensOnly(false);
        TextLangMan::setHyphenationForceAlgorithmic(false);
        TextLangCfg * lang = TextLangMan::getTextLangCfg(U"en-US", true);
        HyphMethod * hyph = lang->getHyphMethod();
        if (!hyph || hyph->getId() != U"English_US.pattern" || hyph->getSize() == 0)
            throw std::runtime_error("shipped English_US.pattern was not loaded");
        if (options.selfcheck) {
            selfcheck(font, lang);
            HyphMan::uninit();
            font.Clear();
            ShutdownFontManager();
            return 0;
        }

        std::ifstream file;
        std::istream * input = &std::cin;
        if (options.input != "-") {
            file.open(options.input);
            if (!file) throw std::runtime_error("failed to open input: " + options.input);
            input = &file;
        }
        std::string line;
        std::size_t paragraph = 0;
        while (std::getline(*input, line)) {
            ++paragraph;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty())
                throw std::runtime_error("empty paragraph at input line " +
                                         std::to_string(paragraph));
            if (line.size() > 65535)
                throw std::runtime_error("paragraph exceeds 65535 UTF-8 bytes at input line " +
                                         std::to_string(paragraph));
            if (!validParagraph(line))
                throw std::runtime_error("invalid UTF-8 or control character at input line " +
                                         std::to_string(paragraph));
            for (int width = options.width_first; width <= options.width_last;
                    width += options.width_step) {
                if (options.optimal != "on") emitRecord(line, paragraph, width, false, font, lang);
                if (options.optimal != "off") emitRecord(line, paragraph, width, true, font, lang);
                if (options.width_last - width < options.width_step) break;
            }
        }
        if (input->bad())
            throw std::runtime_error("failed while reading input");
        std::cout.flush();
        if (!std::cout)
            throw std::runtime_error("failed to flush output");
        HyphMan::uninit();
        font.Clear();
        ShutdownFontManager();
        return 0;
    }
    catch (const std::exception & error) {
        std::cerr << "kp_formatter_trace: " << error.what() << '\n';
        return 2;
    }
}
