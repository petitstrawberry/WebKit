#include "config.h"
#include "ScarletElementInspection.h"

#include "CSSComputedStyleDeclaration.h"
#include "CSSPropertyNames.h"
#include "CachedFont.h"
#include "CachedImage.h"
#include "CachedResourceLoader.h"
#include "ContainerNodeInlines.h"
#include "Document.h"
#include "DocumentResourceLoader.h"
#include "Element.h"
#include "ExceptionCode.h"
#include "FloatRect.h"
#include "Font.h"
#include "FontCascade.h"
#include "FontCreationContext.h"
#include "FontPlatformData.h"
#include "GlyphPage.h"
#include "HTMLImageElement.h"
#include "PseudoElement.h"
#include "PseudoElementIdentifier.h"
#include "RenderDescendantIterator.h"
#include "RenderElement.h"
#include "RenderText.h"
#include "StyleComputedStyle+GettersInlines.h"
#include <cairo.h>
#include <wtf/RefPtr.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

#include <optional>
#include <span>

using namespace WebCore;

namespace {
std::string utf8(const String& value)
{
    auto bytes = value.utf8();
    return { reinterpret_cast<const char*>(bytes.data()), bytes.length() };
}

std::string property(const CSSComputedStyleDeclaration& style, CSSPropertyID id)
{
    return utf8(style.getPropertyValue(id));
}

const char* resourceStatusName(CachedResource::Status status)
{
    switch (status) {
    case CachedResource::Unknown:
        return "Unknown";
    case CachedResource::Pending:
        return "Pending";
    case CachedResource::Cached:
        return "Cached";
    case CachedResource::LoadError:
        return "LoadError";
    case CachedResource::DecodeError:
        return "DecodeError";
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void inspectBeforeGlyph(Document& document, Element& element, uint32_t codepoint,
    const std::vector<String>& sourceURLs, ScarletElementInspection& result)
{
    result.beforeGlyphCodepoint = codepoint;
    auto* pseudo = element.beforePseudoElement();
    if (!pseudo)
        return;
    auto* renderer = pseudo->renderer();
    if (!renderer)
        return;

    for (auto& textRenderer : descendantsOfType<RenderText>(*renderer)) {
        auto& text = textRenderer.text();
        result.beforeGlyphPseudoText += utf8(text);
        for (auto character : StringView(text).codePoints()) {
            if (static_cast<uint32_t>(character) == codepoint)
                result.beforeGlyphContainsCodepoint = true;
        }
    }

    auto& cascade = renderer->style().fontCascade();
    auto glyphData = cascade.glyphDataForCharacter(static_cast<char32_t>(codepoint), false);
    result.beforeGlyphInspected = true;
    result.beforeGlyph = glyphData.glyph;
    RefPtr<const Font> glyphFont = glyphData.font.get();
    if (!glyphFont)
        return;

    result.beforeGlyphFontPresent = true;
    result.beforeGlyphOrigin = glyphFont->origin() == Font::Origin::Remote ? "remote" : "local";
    result.beforeGlyphInterstitial = glyphFont->isInterstitial();
    result.beforeGlyphFamily = utf8(glyphFont->platformData().familyName());
    if (glyphFont->origin() != Font::Origin::Remote || glyphFont->isInterstitial())
        return;

    auto* glyphScaledFont = glyphFont->platformData().scaledFont();
    if (!glyphScaledFont || cairo_scaled_font_status(glyphScaledFont) != CAIRO_STATUS_SUCCESS)
        return;
    auto* glyphFace = cairo_scaled_font_get_font_face(glyphScaledFont);
    if (!glyphFace || cairo_font_face_status(glyphFace) != CAIRO_STATUS_SUCCESS)
        return;

    for (auto& sourceURL : sourceURLs) {
        RefPtr<CachedFont> resource = dynamicDowncast<CachedFont>(document.cachedResourceLoader().cachedResource(sourceURL));
        if (!resource || resource->status() != CachedResource::Cached || resource->errorOccurred() || resource->isLoading())
            continue;
        // Real native decoding is permitted for this already loaded resource.
        // No resource request is made, and unsuccessful candidates yield no proof.
        if (!resource->ensureCustomFontData())
            continue;
        auto sourceFont = resource->createFont(cascade.fontDescription(), FontCreationContext { });
        if (!sourceFont)
            continue;
        auto* sourceScaledFont = sourceFont->platformData().scaledFont();
        if (!sourceScaledFont || cairo_scaled_font_status(sourceScaledFont) != CAIRO_STATUS_SUCCESS)
            continue;
        auto* sourceFace = cairo_scaled_font_get_font_face(sourceScaledFont);
        if (!sourceFace || cairo_font_face_status(sourceFace) != CAIRO_STATUS_SUCCESS || sourceFace != glyphFace)
            continue;

        result.beforeGlyphSourceURL = utf8(sourceURL);
        result.beforeGlyphSourceDecoded = true;
        result.beforeGlyphSourceFaceMatches = true;
        break;
    }
}
}

bool inspectScarletElements(Document& document,
    const std::vector<std::pair<std::string, std::string>>& aliasSelectors,
    std::vector<ScarletElementInspection>& out, std::string& error,
    bool inspectBefore, uint32_t beforeGlyphCodepoint,
    const std::vector<std::string>& beforeFontSourceURLs)
{
    out.clear();
    error.clear();
    if (beforeGlyphCodepoint && (!inspectBefore || beforeGlyphCodepoint > 0x10ffff || (beforeGlyphCodepoint >= 0xd800 && beforeGlyphCodepoint <= 0xdfff))) {
        error = "before glyph inspection requires before style inspection and a Unicode scalar value";
        return false;
    }
    if (!beforeGlyphCodepoint && !beforeFontSourceURLs.empty()) {
        error = "before font source inspection requires a nonzero glyph codepoint";
        return false;
    }
    std::vector<String> sourceURLs;
    sourceURLs.reserve(beforeFontSourceURLs.size());
    for (auto& sourceURL : beforeFontSourceURLs) {
        auto value = String::fromUTF8(std::span<const char>(sourceURL.data(), sourceURL.size()));
        if (value.isNull()) {
            error = "before font source URL is not valid UTF-8";
            return false;
        }
        sourceURLs.push_back(std::move(value));
    }
    std::vector<ScarletElementInspection> results;
    results.reserve(aliasSelectors.size());

    for (auto& [alias, selector] : aliasSelectors) {
        auto selectorString = String::fromUTF8(std::span<const char>(selector.data(), selector.size()));
        if (selectorString.isNull()) {
            error = "selector is not valid UTF-8 for alias " + alias;
            return false;
        }

        auto query = document.querySelector(selectorString);
        if (query.hasException()) {
            auto exception = query.releaseException();
            error = "document.querySelector ";
            error += exception.code() == ExceptionCode::SyntaxError ? "SyntaxError" : "failed";
            error += " for alias " + alias + ": " + selector;
            auto message = utf8(exception.message());
            if (!message.empty())
                error += " (" + message + ")";
            return false;
        }

        ScarletElementInspection result;
        result.alias = alias;
        result.selector = selector;
        if (auto* element = query.releaseReturnValue()) {
            result.found = true;
            auto bounds = element->boundingClientRect();
            result.x = bounds.x();
            result.y = bounds.y();
            result.width = bounds.width();
            result.height = bounds.height();
            result.text = utf8(element->textContent());

            if (auto* image = dynamicDowncast<HTMLImageElement>(*element)) {
                result.isImage = true;
                result.imageComplete = image->complete();
                result.naturalWidth = image->naturalWidth();
                result.naturalHeight = image->naturalHeight();
                if (auto* resource = image->cachedImage()) {
                    result.imageResourcePresent = true;
                    result.imageCanRender = resource->canRender();
                    result.imageResourceStatus = resourceStatusName(resource->status());
                }
            }

            auto style = CSSComputedStyleDeclaration::create(*element, CSSComputedStyleDeclaration::AllowVisited::No);
            result.display = property(style.get(), CSSPropertyID::Display);
            result.visibility = property(style.get(), CSSPropertyID::Visibility);
            result.fontFamily = property(style.get(), CSSPropertyID::FontFamily);
            result.fontSize = property(style.get(), CSSPropertyID::FontSize);
            result.fontWeight = property(style.get(), CSSPropertyID::FontWeight);

            if (inspectBefore) {
                std::optional<Style::PseudoElementIdentifier> before { Style::PseudoElementIdentifier { PseudoElementType::Before } };
                auto beforeStyle = CSSComputedStyleDeclaration::create(*element, before);
                result.beforeInspected = true;
                result.beforeContent = property(beforeStyle.get(), CSSPropertyID::Content);
                result.beforeFontFamily = property(beforeStyle.get(), CSSPropertyID::FontFamily);
                result.beforeFontSize = property(beforeStyle.get(), CSSPropertyID::FontSize);
                result.beforeFontWeight = property(beforeStyle.get(), CSSPropertyID::FontWeight);
                if (beforeGlyphCodepoint)
                    inspectBeforeGlyph(document, *element, beforeGlyphCodepoint, sourceURLs, result);
            }
        }
        results.push_back(std::move(result));
    }
    out = std::move(results);
    return true;
}
