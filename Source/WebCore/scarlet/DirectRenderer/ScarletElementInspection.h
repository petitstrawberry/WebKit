#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace WebCore {
class Document;
}

// Values come directly from WebCore's DOM geometry and computed CSSOM style.
// CSS values retain WebCore's serialization (for example, fontSize is "16px").
struct ScarletElementInspection {
    std::string alias;
    std::string selector;
    bool found { false };
    double x { 0 };
    double y { 0 };
    double width { 0 };
    double height { 0 };
    std::string text;
    std::string display;
    std::string visibility;
    std::string fontFamily;
    std::string fontSize;
    std::string fontWeight;

    bool isImage { false };
    bool imageComplete { false };
    unsigned naturalWidth { 0 };
    unsigned naturalHeight { 0 };
    bool imageResourcePresent { false };
    bool imageCanRender { false };
    // Exact CachedResource::Status name when a CachedImage exists. "Cached"
    // describes the resource cache state; it is not an img.decode() result.
    std::string imageResourceStatus;

    // This flag means the ::before computed style was inspected. It does not
    // claim a generated pseudo-element exists or has a rendered box.
    bool beforeInspected { false };
    std::string beforeContent;
    std::string beforeFontFamily;
    std::string beforeFontSize;
    std::string beforeFontWeight;

    // Glyph lookup is attempted only for an existing rendered ::before.
    // Missing glyph/font/source evidence retains zero, false, or empty values.
    bool beforeGlyphInspected { false };
    uint32_t beforeGlyphCodepoint { 0 };
    std::string beforeGlyphPseudoText;
    bool beforeGlyphContainsCodepoint { false };
    unsigned beforeGlyph { 0 };
    bool beforeGlyphFontPresent { false };
    std::string beforeGlyphOrigin;
    bool beforeGlyphInterstitial { false };
    std::string beforeGlyphFamily;
    std::string beforeGlyphSourceURL;
    bool beforeGlyphSourceDecoded { false };
    bool beforeGlyphSourceFaceMatches { false };
};

// The caller completes document layout before inspection. An unmatched valid
// selector produces found=false. Invalid UTF-8 or a querySelector exception
// returns false, with error set and no partial results in out. A nonzero glyph
// codepoint requires inspectBefore and must be a Unicode scalar value. Source
// candidates inspect only existing completed cached resources; they never load
// a URL. Their real decoded font face must equal the selected glyph's face.
bool inspectScarletElements(WebCore::Document&,
    const std::vector<std::pair<std::string, std::string>>& aliasSelectors,
    std::vector<ScarletElementInspection>& out, std::string& error,
    bool inspectBefore = false, uint32_t beforeGlyphCodepoint = 0,
    const std::vector<std::string>& beforeFontSourceURLs = { });
