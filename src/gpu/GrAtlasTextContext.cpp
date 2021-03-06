/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "GrAtlasTextContext.h"

#include "GrAtlas.h"
#include "GrBatch.h"
#include "GrBatchFontCache.h"
#include "GrBatchTarget.h"
#include "GrDefaultGeoProcFactory.h"
#include "GrDrawTarget.h"
#include "GrFontScaler.h"
#include "GrIndexBuffer.h"
#include "GrStrokeInfo.h"
#include "GrTextBlobCache.h"
#include "GrTexturePriv.h"

#include "SkAutoKern.h"
#include "SkColorPriv.h"
#include "SkDraw.h"
#include "SkDrawFilter.h"
#include "SkDrawProcs.h"
#include "SkGlyphCache.h"
#include "SkGpuDevice.h"
#include "SkGr.h"
#include "SkPath.h"
#include "SkRTConf.h"
#include "SkStrokeRec.h"
#include "SkTextBlob.h"
#include "SkTextMapStateProc.h"

#include "effects/GrBitmapTextGeoProc.h"
#include "effects/GrSimpleTextureEffect.h"

namespace {
static const size_t kLCDTextVASize = sizeof(SkPoint) + sizeof(SkIPoint16);

// position + local coord
static const size_t kColorTextVASize = sizeof(SkPoint) + sizeof(SkIPoint16);

static const size_t kGrayTextVASize = sizeof(SkPoint) + sizeof(GrColor) + sizeof(SkIPoint16);

static const int kVerticesPerGlyph = 4;
static const int kIndicesPerGlyph = 6;

static size_t get_vertex_stride(GrMaskFormat maskFormat) {
    switch (maskFormat) {
        case kA8_GrMaskFormat:
            return kGrayTextVASize;
        case kARGB_GrMaskFormat:
            return kColorTextVASize;
        default:
            return kLCDTextVASize;
    }
}

};

// TODO
// Gamma slotting to preserve color
// Better reuse on regeneration
// Telemetry tests
// possibly consider having a regeneration ratio on the textblob itself for animated textblobs

GrAtlasTextContext::GrAtlasTextContext(GrContext* context,
                                       SkGpuDevice* gpuDevice,
                                       const SkDeviceProperties& properties)
    : INHERITED(context, gpuDevice, properties) {
    // We overallocate vertices in our textblobs based on the assumption that A8 has the greatest
    // vertexStride
    SK_COMPILE_ASSERT(kGrayTextVASize >= kColorTextVASize && kGrayTextVASize >= kLCDTextVASize,
                      vertex_attribute_changed);
    fCurrStrike = NULL;
    fCache = context->getTextBlobCache();
}

GrAtlasTextContext* GrAtlasTextContext::Create(GrContext* context,
                                               SkGpuDevice* gpuDevice,
                                               const SkDeviceProperties& props) {
    return SkNEW_ARGS(GrAtlasTextContext, (context, gpuDevice, props));
}

bool GrAtlasTextContext::canDraw(const GrRenderTarget*,
                                 const GrClip&,
                                 const GrPaint&,
                                 const SkPaint& skPaint,
                                 const SkMatrix& viewMatrix) {
    return !SkDraw::ShouldDrawTextAsPaths(skPaint, viewMatrix);
}

GrColor GrAtlasTextContext::ComputeCanonicalColor(const SkPaint& paint, bool lcd) {
    GrColor canonicalColor = paint.computeLuminanceColor();
    if (lcd) {
        // This is the correct computation, but there are tons of cases where LCD can be overridden.
        // For now we just regenerate if any run in a textblob has LCD.
        // TODO figure out where all of these overrides are and see if we can incorporate that logic
        // at a higher level *OR* use sRGB
        SkASSERT(false);
        //canonicalColor = SkMaskGamma::CanonicalColor(canonicalColor);
    } else {
        // A8, though can have mixed BMP text but it shouldn't matter because BMP text won't have
        // gamma corrected masks anyways, nor color
        U8CPU lum = SkComputeLuminance(SkColorGetR(canonicalColor),
                                       SkColorGetG(canonicalColor),
                                       SkColorGetB(canonicalColor));
        // reduce to our finite number of bits
        canonicalColor = SkMaskGamma::CanonicalColor(SkColorSetRGB(lum, lum, lum));
    }
    return canonicalColor;
}

// TODO if this function ever shows up in profiling, then we can compute this value when the
// textblob is being built and cache it.  However, for the time being textblobs mostly only have 1
// run so this is not a big deal to compute here.
bool GrAtlasTextContext::HasLCD(const SkTextBlob* blob) {
    SkTextBlob::RunIterator it(blob);
    for (; !it.done(); it.next()) {
        if (it.isLCD()) {
            return true;
        }
    }
    return false;
}

bool GrAtlasTextContext::MustRegenerateBlob(SkScalar* outTransX, SkScalar* outTransY,
                                            const BitmapTextBlob& blob, const SkPaint& paint,
                                            const SkMaskFilter::BlurRec& blurRec,
                                            const SkMatrix& viewMatrix, SkScalar x, SkScalar y) {
    // If we have LCD text then our canonical color will be set to transparent, in this case we have
    // to regenerate the blob on any color change
    if (blob.fKey.fCanonicalColor == SK_ColorTRANSPARENT && blob.fPaintColor != paint.getColor()) {
        return true;
    }

    if (blob.fViewMatrix.hasPerspective() != viewMatrix.hasPerspective()) {
        return true;
    }

    if (blob.fViewMatrix.hasPerspective() && !blob.fViewMatrix.cheapEqualTo(viewMatrix)) {
        return true;
    }

    if (blob.fViewMatrix.getScaleX() != viewMatrix.getScaleX() ||
        blob.fViewMatrix.getScaleY() != viewMatrix.getScaleY() ||
        blob.fViewMatrix.getSkewX() != viewMatrix.getSkewX() ||
        blob.fViewMatrix.getSkewY() != viewMatrix.getSkewY()) {
        return true;
    }

    // We only cache one masked version
    if (blob.fKey.fHasBlur &&
        (blob.fBlurRec.fSigma != blurRec.fSigma ||
         blob.fBlurRec.fStyle != blurRec.fStyle ||
         blob.fBlurRec.fQuality != blurRec.fQuality)) {
        return true;
    }

    // Similarly, we only cache one version for each style
    if (blob.fKey.fStyle != SkPaint::kFill_Style &&
        (blob.fStrokeInfo.fFrameWidth != paint.getStrokeWidth() ||
         blob.fStrokeInfo.fMiterLimit != paint.getStrokeMiter() ||
         blob.fStrokeInfo.fJoin != paint.getStrokeJoin())) {
        return true;
    }

    // We can update the positions in the cachedtextblobs without regenerating the whole blob, but
    // only for integer translations.
    // This cool bit of math will determine the necessary translation to apply to the already
    // generated vertex coordinates to move them to the correct position
    SkScalar transX = viewMatrix.getTranslateX() +
                      viewMatrix.getScaleX() * (x - blob.fX) +
                      viewMatrix.getSkewX() * (y - blob.fY) -
                      blob.fViewMatrix.getTranslateX();
    SkScalar transY = viewMatrix.getTranslateY() +
                      viewMatrix.getSkewY() * (x - blob.fX) +
                      viewMatrix.getScaleY() * (y - blob.fY) -
                      blob.fViewMatrix.getTranslateY();
    if (SkScalarFraction(transX) > SK_ScalarNearlyZero ||
        SkScalarFraction(transY) > SK_ScalarNearlyZero) {
        return true;
    }

#ifdef SK_DEBUG
    static const SkScalar kMinDiscernableTranslation = 0.0625;
    // As a safeguard when debugging, we store the total error across all translations and print if
    // the error becomes discernable.  This is pretty unlikely to occur given the tight bounds above
    // on translation
    blob.fTotalXError += SkScalarAbs(SkScalarFraction(transX));
    blob.fTotalYError += SkScalarAbs(SkScalarFraction(transY));
    if (blob.fTotalXError > kMinDiscernableTranslation ||
        blob.fTotalYError > kMinDiscernableTranslation) {
        SkDebugf("Exceeding error threshold for bitmap text translation");
    }
#endif
    (*outTransX) = transX;
    (*outTransY) = transY;
    return false;
}


inline SkGlyphCache* GrAtlasTextContext::setupCache(BitmapTextBlob::Run* run,
                                                    const SkPaint& skPaint,
                                                    const SkMatrix& viewMatrix) {
    skPaint.getScalerContextDescriptor(&run->fDescriptor, &fDeviceProperties, &viewMatrix, false);
    run->fTypeface.reset(SkSafeRef(skPaint.getTypeface()));
    return SkGlyphCache::DetachCache(run->fTypeface, run->fDescriptor.getDesc());
}

void GrAtlasTextContext::drawTextBlob(GrRenderTarget* rt, const GrClip& clip,
                                      const SkPaint& skPaint, const SkMatrix& viewMatrix,
                                      const SkTextBlob* blob, SkScalar x, SkScalar y,
                                      SkDrawFilter* drawFilter, const SkIRect& clipBounds) {
    SkAutoTUnref<BitmapTextBlob> cacheBlob;
    SkMaskFilter::BlurRec blurRec;
    BitmapTextBlob::Key key;
    // It might be worth caching these things, but its not clear at this time
    // TODO for animated mask filters, this will fill up our cache.  We need a safeguard here
    const SkMaskFilter* mf = skPaint.getMaskFilter();
    bool canCache = !(skPaint.getPathEffect() ||
                      (mf && !mf->asABlur(&blurRec)) ||
                      drawFilter);

    if (canCache) {
        bool hasLCD = HasLCD(blob);
        // TODO we want to figure out a way to be able to use the canonical color on LCD text,
        // see the note on ComputeCanonicalColor above.  We pick a dummy value for LCD text to
        // ensure we always match the same key
        GrColor canonicalColor = hasLCD ? SK_ColorTRANSPARENT :
                                          ComputeCanonicalColor(skPaint, hasLCD);

        key.fUniqueID = blob->uniqueID();
        key.fStyle = skPaint.getStyle();
        key.fHasBlur = SkToBool(mf);
        key.fCanonicalColor = canonicalColor;
        cacheBlob.reset(SkSafeRef(fCache->find(key)));
    }

    SkIRect clipRect;
    clip.getConservativeBounds(rt->width(), rt->height(), &clipRect);

    SkScalar transX = 0.f;
    SkScalar transY = 0.f;

    // Though for the time being runs in the textblob can override the paint, they only touch font
    // info.
    GrPaint grPaint;
    SkPaint2GrPaintShader(fContext, rt, skPaint, viewMatrix, true, &grPaint);

    if (cacheBlob) {
        if (MustRegenerateBlob(&transX, &transY, *cacheBlob, skPaint, blurRec, viewMatrix, x, y)) {
            // We have to remake the blob because changes may invalidate our masks.
            // TODO we could probably get away reuse most of the time if the pointer is unique,
            // but we'd have to clear the subrun information
            fCache->remove(cacheBlob);
            cacheBlob.reset(SkRef(fCache->createCachedBlob(blob, key, blurRec, skPaint,
                                                           kGrayTextVASize)));
            this->regenerateTextBlob(cacheBlob, skPaint, grPaint.getColor(), viewMatrix, blob, x, y,
                                     drawFilter, clipRect);
        } else {
            // If we can reuse the blob, then make sure we update the blob's viewmatrix, and x/y
            // offsets
            cacheBlob->fViewMatrix = viewMatrix;
            cacheBlob->fX = x;
            cacheBlob->fY = y;
            fCache->makeMRU(cacheBlob);
        }
    } else {
        if (canCache) {
            cacheBlob.reset(SkRef(fCache->createCachedBlob(blob, key, blurRec, skPaint,
                                                           kGrayTextVASize)));
        } else {
            cacheBlob.reset(fCache->createBlob(blob, kGrayTextVASize));
        }
        this->regenerateTextBlob(cacheBlob, skPaint, grPaint.getColor(), viewMatrix, blob, x, y,
                                 drawFilter, clipRect);
    }

    cacheBlob->fPaintColor = skPaint.getColor();
    this->flush(fContext->getTextTarget(), blob, cacheBlob, rt, skPaint, grPaint, drawFilter,
                clip, viewMatrix, clipBounds, x, y, transX, transY);
}

void GrAtlasTextContext::regenerateTextBlob(BitmapTextBlob* cacheBlob,
                                            const SkPaint& skPaint, GrColor color,
                                            const SkMatrix& viewMatrix,
                                            const SkTextBlob* blob, SkScalar x, SkScalar y,
                                            SkDrawFilter* drawFilter, const SkIRect& clipRect) {
    cacheBlob->fViewMatrix = viewMatrix;
    cacheBlob->fX = x;
    cacheBlob->fY = y;

    // Regenerate textblob
    SkPaint runPaint = skPaint;
    SkTextBlob::RunIterator it(blob);
    for (int run = 0; !it.done(); it.next(), run++) {
        int glyphCount = it.glyphCount();
        size_t textLen = glyphCount * sizeof(uint16_t);
        const SkPoint& offset = it.offset();
        // applyFontToPaint() always overwrites the exact same attributes,
        // so it is safe to not re-seed the paint for this reason.
        it.applyFontToPaint(&runPaint);

        if (drawFilter && !drawFilter->filter(&runPaint, SkDrawFilter::kText_Type)) {
            // A false return from filter() means we should abort the current draw.
            runPaint = skPaint;
            continue;
        }

        runPaint.setFlags(fGpuDevice->filterTextFlags(runPaint));

        SkGlyphCache* cache = this->setupCache(&cacheBlob->fRuns[run], runPaint, viewMatrix);

        // setup vertex / glyphIndex for the new run
        if (run > 0) {
            PerSubRunInfo& newRun = cacheBlob->fRuns[run].fSubRunInfo.back();
            PerSubRunInfo& lastRun = cacheBlob->fRuns[run - 1].fSubRunInfo.back();

            newRun.fVertexStartIndex = lastRun.fVertexEndIndex;
            newRun.fVertexEndIndex = lastRun.fVertexEndIndex;

            newRun.fGlyphStartIndex = lastRun.fGlyphEndIndex;
            newRun.fGlyphEndIndex = lastRun.fGlyphEndIndex;
        }

        if (SkDraw::ShouldDrawTextAsPaths(runPaint, viewMatrix)) {
            cacheBlob->fRuns[run].fDrawAsPaths = true;
            continue;
        }
        cacheBlob->fRuns[run].fDrawAsPaths = false;

        switch (it.positioning()) {
            case SkTextBlob::kDefault_Positioning:
                this->internalDrawText(cacheBlob, run, cache, runPaint, color, viewMatrix,
                                       (const char *)it.glyphs(), textLen,
                                       x + offset.x(), y + offset.y(), clipRect);
                break;
            case SkTextBlob::kHorizontal_Positioning:
                this->internalDrawPosText(cacheBlob, run, cache, runPaint, color, viewMatrix,
                                          (const char*)it.glyphs(), textLen, it.pos(), 1,
                                          SkPoint::Make(x, y + offset.y()), clipRect);
                break;
            case SkTextBlob::kFull_Positioning:
                this->internalDrawPosText(cacheBlob, run, cache, runPaint, color, viewMatrix,
                                          (const char*)it.glyphs(), textLen, it.pos(), 2,
                                          SkPoint::Make(x, y), clipRect);
                break;
        }

        if (drawFilter) {
            // A draw filter may change the paint arbitrarily, so we must re-seed in this case.
            runPaint = skPaint;
        }

        SkGlyphCache::AttachCache(cache);
    }
}

void GrAtlasTextContext::onDrawText(GrRenderTarget* rt, const GrClip& clip,
                                    const GrPaint& paint, const SkPaint& skPaint,
                                    const SkMatrix& viewMatrix,
                                    const char text[], size_t byteLength,
                                    SkScalar x, SkScalar y, const SkIRect& regionClipBounds) {
    int glyphCount = skPaint.countText(text, byteLength);
    SkAutoTUnref<BitmapTextBlob> blob(fCache->createBlob(glyphCount, 1, kGrayTextVASize));
    blob->fViewMatrix = viewMatrix;
    blob->fX = x;
    blob->fY = y;

    SkIRect clipRect;
    clip.getConservativeBounds(rt->width(), rt->height(), &clipRect);

    // setup cache
    SkGlyphCache* cache = this->setupCache(&blob->fRuns[0], skPaint, viewMatrix);
    this->internalDrawText(blob, 0, cache, skPaint, paint.getColor(), viewMatrix, text, byteLength,
                           x, y, clipRect);
    SkGlyphCache::AttachCache(cache);

    this->flush(fContext->getTextTarget(), blob, rt, skPaint, paint, clip, viewMatrix);
}

void GrAtlasTextContext::internalDrawText(BitmapTextBlob* blob, int runIndex,
                                          SkGlyphCache* cache, const SkPaint& skPaint,
                                          GrColor color,
                                          const SkMatrix& viewMatrix,
                                          const char text[], size_t byteLength,
                                          SkScalar x, SkScalar y, const SkIRect& clipRect) {
    SkASSERT(byteLength == 0 || text != NULL);

    // nothing to draw
    if (text == NULL || byteLength == 0) {
        return;
    }

    fCurrStrike = NULL;
    SkDrawCacheProc glyphCacheProc = skPaint.getDrawCacheProc();

    // Get GrFontScaler from cache
    GrFontScaler* fontScaler = GetGrFontScaler(cache);

    // transform our starting point
    {
        SkPoint loc;
        viewMatrix.mapXY(x, y, &loc);
        x = loc.fX;
        y = loc.fY;
    }

    // need to measure first
    if (skPaint.getTextAlign() != SkPaint::kLeft_Align) {
        SkVector    stopVector;
        MeasureText(cache, glyphCacheProc, text, byteLength, &stopVector);

        SkScalar    stopX = stopVector.fX;
        SkScalar    stopY = stopVector.fY;

        if (skPaint.getTextAlign() == SkPaint::kCenter_Align) {
            stopX = SkScalarHalf(stopX);
            stopY = SkScalarHalf(stopY);
        }
        x -= stopX;
        y -= stopY;
    }

    const char* stop = text + byteLength;

    SkAutoKern autokern;

    SkFixed fxMask = ~0;
    SkFixed fyMask = ~0;
    SkScalar halfSampleX, halfSampleY;
    if (cache->isSubpixel()) {
        halfSampleX = halfSampleY = SkFixedToScalar(SkGlyph::kSubpixelRound);
        SkAxisAlignment baseline = SkComputeAxisAlignmentForHText(viewMatrix);
        if (kX_SkAxisAlignment == baseline) {
            fyMask = 0;
            halfSampleY = SK_ScalarHalf;
        } else if (kY_SkAxisAlignment == baseline) {
            fxMask = 0;
            halfSampleX = SK_ScalarHalf;
        }
    } else {
        halfSampleX = halfSampleY = SK_ScalarHalf;
    }

    Sk48Dot16 fx = SkScalarTo48Dot16(x + halfSampleX);
    Sk48Dot16 fy = SkScalarTo48Dot16(y + halfSampleY);

    while (text < stop) {
        const SkGlyph& glyph = glyphCacheProc(cache, &text, fx & fxMask, fy & fyMask);

        fx += autokern.adjust(glyph);

        if (glyph.fWidth) {
            this->appendGlyph(blob,
                              runIndex,
                              GrGlyph::Pack(glyph.getGlyphID(),
                                            glyph.getSubXFixed(),
                                            glyph.getSubYFixed(),
                                            GrGlyph::kCoverage_MaskStyle),
                              Sk48Dot16FloorToInt(fx),
                              Sk48Dot16FloorToInt(fy),
                              color,
                              fontScaler,
                              clipRect);
        }

        fx += glyph.fAdvanceX;
        fy += glyph.fAdvanceY;
    }
}

void GrAtlasTextContext::onDrawPosText(GrRenderTarget* rt, const GrClip& clip,
                                       const GrPaint& paint, const SkPaint& skPaint,
                                       const SkMatrix& viewMatrix,
                                       const char text[], size_t byteLength,
                                       const SkScalar pos[], int scalarsPerPosition,
                                       const SkPoint& offset, const SkIRect& regionClipBounds) {
    int glyphCount = skPaint.countText(text, byteLength);
    SkAutoTUnref<BitmapTextBlob> blob(fCache->createBlob(glyphCount, 1, kGrayTextVASize));
    blob->fViewMatrix = viewMatrix;

    SkIRect clipRect;
    clip.getConservativeBounds(rt->width(), rt->height(), &clipRect);

    // setup cache
    SkGlyphCache* cache = this->setupCache(&blob->fRuns[0], skPaint, viewMatrix);
    this->internalDrawPosText(blob, 0, cache, skPaint, paint.getColor(), viewMatrix, text,
                              byteLength, pos, scalarsPerPosition, offset, clipRect);
    SkGlyphCache::AttachCache(cache);

    this->flush(fContext->getTextTarget(), blob, rt, skPaint, paint, clip, viewMatrix);
}

void GrAtlasTextContext::internalDrawPosText(BitmapTextBlob* blob, int runIndex,
                                             SkGlyphCache* cache, const SkPaint& skPaint,
                                             GrColor color,
                                             const SkMatrix& viewMatrix,
                                             const char text[], size_t byteLength,
                                             const SkScalar pos[], int scalarsPerPosition,
                                             const SkPoint& offset, const SkIRect& clipRect) {
    SkASSERT(byteLength == 0 || text != NULL);
    SkASSERT(1 == scalarsPerPosition || 2 == scalarsPerPosition);

    // nothing to draw
    if (text == NULL || byteLength == 0) {
        return;
    }

    fCurrStrike = NULL;
    SkDrawCacheProc glyphCacheProc = skPaint.getDrawCacheProc();

    // Get GrFontScaler from cache
    GrFontScaler* fontScaler = GetGrFontScaler(cache);

    const char*        stop = text + byteLength;
    SkTextAlignProc    alignProc(skPaint.getTextAlign());
    SkTextMapStateProc tmsProc(viewMatrix, offset, scalarsPerPosition);

    if (cache->isSubpixel()) {
        // maybe we should skip the rounding if linearText is set
        SkAxisAlignment baseline = SkComputeAxisAlignmentForHText(viewMatrix);

        SkFixed fxMask = ~0;
        SkFixed fyMask = ~0;
        SkScalar halfSampleX = SkFixedToScalar(SkGlyph::kSubpixelRound);
        SkScalar halfSampleY = SkFixedToScalar(SkGlyph::kSubpixelRound);
        if (kX_SkAxisAlignment == baseline) {
            fyMask = 0;
            halfSampleY = SK_ScalarHalf;
        } else if (kY_SkAxisAlignment == baseline) {
            fxMask = 0;
            halfSampleX = SK_ScalarHalf;
        }

        if (SkPaint::kLeft_Align == skPaint.getTextAlign()) {
            while (text < stop) {
                SkPoint tmsLoc;
                tmsProc(pos, &tmsLoc);
                Sk48Dot16 fx = SkScalarTo48Dot16(tmsLoc.fX + halfSampleX);
                Sk48Dot16 fy = SkScalarTo48Dot16(tmsLoc.fY + halfSampleY);

                const SkGlyph& glyph = glyphCacheProc(cache, &text,
                                                      fx & fxMask, fy & fyMask);

                if (glyph.fWidth) {
                    this->appendGlyph(blob,
                                      runIndex,
                                      GrGlyph::Pack(glyph.getGlyphID(),
                                                    glyph.getSubXFixed(),
                                                    glyph.getSubYFixed(),
                                                    GrGlyph::kCoverage_MaskStyle),
                                      Sk48Dot16FloorToInt(fx),
                                      Sk48Dot16FloorToInt(fy),
                                      color,
                                      fontScaler,
                                      clipRect);
                }
                pos += scalarsPerPosition;
            }
        } else {
            while (text < stop) {
                const char* currentText = text;
                const SkGlyph& metricGlyph = glyphCacheProc(cache, &text, 0, 0);

                if (metricGlyph.fWidth) {
                    SkDEBUGCODE(SkFixed prevAdvX = metricGlyph.fAdvanceX;)
                    SkDEBUGCODE(SkFixed prevAdvY = metricGlyph.fAdvanceY;)
                    SkPoint tmsLoc;
                    tmsProc(pos, &tmsLoc);
                    SkPoint alignLoc;
                    alignProc(tmsLoc, metricGlyph, &alignLoc);

                    Sk48Dot16 fx = SkScalarTo48Dot16(alignLoc.fX + halfSampleX);
                    Sk48Dot16 fy = SkScalarTo48Dot16(alignLoc.fY + halfSampleY);

                    // have to call again, now that we've been "aligned"
                    const SkGlyph& glyph = glyphCacheProc(cache, &currentText,
                                                          fx & fxMask, fy & fyMask);
                    // the assumption is that the metrics haven't changed
                    SkASSERT(prevAdvX == glyph.fAdvanceX);
                    SkASSERT(prevAdvY == glyph.fAdvanceY);
                    SkASSERT(glyph.fWidth);

                    this->appendGlyph(blob,
                                      runIndex,
                                      GrGlyph::Pack(glyph.getGlyphID(),
                                                    glyph.getSubXFixed(),
                                                    glyph.getSubYFixed(),
                                                    GrGlyph::kCoverage_MaskStyle),
                                      Sk48Dot16FloorToInt(fx),
                                      Sk48Dot16FloorToInt(fy),
                                      color,
                                      fontScaler,
                                      clipRect);
                }
                pos += scalarsPerPosition;
            }
        }
    } else {    // not subpixel

        if (SkPaint::kLeft_Align == skPaint.getTextAlign()) {
            while (text < stop) {
                // the last 2 parameters are ignored
                const SkGlyph& glyph = glyphCacheProc(cache, &text, 0, 0);

                if (glyph.fWidth) {
                    SkPoint tmsLoc;
                    tmsProc(pos, &tmsLoc);

                    Sk48Dot16 fx = SkScalarTo48Dot16(tmsLoc.fX + SK_ScalarHalf); //halfSampleX;
                    Sk48Dot16 fy = SkScalarTo48Dot16(tmsLoc.fY + SK_ScalarHalf); //halfSampleY;
                    this->appendGlyph(blob,
                                      runIndex,
                                      GrGlyph::Pack(glyph.getGlyphID(),
                                                    glyph.getSubXFixed(),
                                                    glyph.getSubYFixed(),
                                                    GrGlyph::kCoverage_MaskStyle),
                                      Sk48Dot16FloorToInt(fx),
                                      Sk48Dot16FloorToInt(fy),
                                      color,
                                      fontScaler,
                                      clipRect);
                }
                pos += scalarsPerPosition;
            }
        } else {
            while (text < stop) {
                // the last 2 parameters are ignored
                const SkGlyph& glyph = glyphCacheProc(cache, &text, 0, 0);

                if (glyph.fWidth) {
                    SkPoint tmsLoc;
                    tmsProc(pos, &tmsLoc);

                    SkPoint alignLoc;
                    alignProc(tmsLoc, glyph, &alignLoc);

                    Sk48Dot16 fx = SkScalarTo48Dot16(alignLoc.fX + SK_ScalarHalf); //halfSampleX;
                    Sk48Dot16 fy = SkScalarTo48Dot16(alignLoc.fY + SK_ScalarHalf); //halfSampleY;
                    this->appendGlyph(blob,
                                      runIndex,
                                      GrGlyph::Pack(glyph.getGlyphID(),
                                                    glyph.getSubXFixed(),
                                                    glyph.getSubYFixed(),
                                                    GrGlyph::kCoverage_MaskStyle),
                                      Sk48Dot16FloorToInt(fx),
                                      Sk48Dot16FloorToInt(fy),
                                      color,
                                      fontScaler,
                                      clipRect);
                }
                pos += scalarsPerPosition;
            }
        }
    }
}

void GrAtlasTextContext::appendGlyph(BitmapTextBlob* blob, int runIndex, GrGlyph::PackedID packed,
                                     int vx, int vy, GrColor color, GrFontScaler* scaler,
                                     const SkIRect& clipRect) {
    if (NULL == fCurrStrike) {
        fCurrStrike = fContext->getBatchFontCache()->getStrike(scaler);
    }

    GrGlyph* glyph = fCurrStrike->getGlyph(packed, scaler);
    if (NULL == glyph || glyph->fBounds.isEmpty()) {
        return;
    }

    int x = vx + glyph->fBounds.fLeft;
    int y = vy + glyph->fBounds.fTop;

    // keep them as ints until we've done the clip-test
    int width = glyph->fBounds.width();
    int height = glyph->fBounds.height();

#if 0
    // Not checking the clip bounds might introduce a performance regression.  However, its not
    // clear if this is still true today with the larger tiles we use in Chrome.  For repositionable
    // blobs, we want to make sure we have all of the glyphs, so clipping them out is not ideal.
    // We could store the cliprect in the key, but then we'd lose the ability to do integer scrolls
    // TODO verify this
    // check if we clipped out
    if (clipRect.quickReject(x, y, x + width, y + height)) {
        return;
    }
#endif

    // If the glyph is too large we fall back to paths
    if (fCurrStrike->glyphTooLargeForAtlas(glyph)) {
        if (NULL == glyph->fPath) {
            SkPath* path = SkNEW(SkPath);
            if (!scaler->getGlyphPath(glyph->glyphID(), path)) {
                // flag the glyph as being dead?
                SkDELETE(path);
                return;
            }
            glyph->fPath = path;
        }
        SkASSERT(glyph->fPath);
        blob->fBigGlyphs.push_back(BitmapTextBlob::BigGlyph(*glyph->fPath, vx, vy));
        return;
    }

    Run& run = blob->fRuns[runIndex];

    GrMaskFormat format = glyph->fMaskFormat;

    PerSubRunInfo* subRun = &run.fSubRunInfo.back();
    if (run.fInitialized && subRun->fMaskFormat != format) {
        PerSubRunInfo* newSubRun = &run.fSubRunInfo.push_back();
        newSubRun->fGlyphStartIndex = subRun->fGlyphEndIndex;
        newSubRun->fGlyphEndIndex = subRun->fGlyphEndIndex;

        newSubRun->fVertexStartIndex = subRun->fVertexEndIndex;
        newSubRun->fVertexEndIndex = subRun->fVertexEndIndex;

        subRun = newSubRun;
    }

    run.fInitialized = true;
    subRun->fMaskFormat = format;
    blob->fGlyphIDs[subRun->fGlyphEndIndex] = packed;

    size_t vertexStride = get_vertex_stride(format);

    SkRect r;
    r.fLeft = SkIntToScalar(x);
    r.fTop = SkIntToScalar(y);
    r.fRight = r.fLeft + SkIntToScalar(width);
    r.fBottom = r.fTop + SkIntToScalar(height);

    run.fVertexBounds.joinNonEmptyArg(r);
    run.fColor = color;

    intptr_t vertex = reinterpret_cast<intptr_t>(blob->fVertices + subRun->fVertexEndIndex);

    // V0
    SkPoint* position = reinterpret_cast<SkPoint*>(vertex);
    position->set(r.fLeft, r.fTop);
    if (kA8_GrMaskFormat == format) {
        SkColor* colorPtr = reinterpret_cast<SkColor*>(vertex + sizeof(SkPoint));
        *colorPtr = color;
    }
    vertex += vertexStride;
    // V1
    position = reinterpret_cast<SkPoint*>(vertex);
    position->set(r.fLeft, r.fBottom);
    if (kA8_GrMaskFormat == format) {
        SkColor* colorPtr = reinterpret_cast<SkColor*>(vertex + sizeof(SkPoint));
        *colorPtr = color;
    }
    vertex += vertexStride;

    // V2
    position = reinterpret_cast<SkPoint*>(vertex);
    position->set(r.fRight, r.fBottom);
    if (kA8_GrMaskFormat == format) {
        SkColor* colorPtr = reinterpret_cast<SkColor*>(vertex + sizeof(SkPoint));
        *colorPtr = color;
    }
    vertex += vertexStride;

    // V3
    position = reinterpret_cast<SkPoint*>(vertex);
    position->set(r.fRight, r.fTop);
    if (kA8_GrMaskFormat == format) {
        SkColor* colorPtr = reinterpret_cast<SkColor*>(vertex + sizeof(SkPoint));
        *colorPtr = color;
    }

    subRun->fGlyphEndIndex++;
    subRun->fVertexEndIndex += vertexStride * kVerticesPerGlyph;
}

class BitmapTextBatch : public GrBatch {
public:
    typedef GrAtlasTextContext::BitmapTextBlob Blob;
    typedef Blob::Run Run;
    typedef Run::SubRunInfo TextInfo;
    struct Geometry {
        Geometry() {}
        Geometry(const Geometry& geometry)
            : fBlob(SkRef(geometry.fBlob.get()))
            , fRun(geometry.fRun)
            , fSubRun(geometry.fSubRun)
            , fColor(geometry.fColor)
            , fTransX(geometry.fTransX)
            , fTransY(geometry.fTransY) {}
        SkAutoTUnref<Blob> fBlob;
        int fRun;
        int fSubRun;
        GrColor fColor;
        SkScalar fTransX;
        SkScalar fTransY;
    };

    static GrBatch* Create(const Geometry& geometry, GrMaskFormat maskFormat,
                           int glyphCount, GrBatchFontCache* fontCache) {
        return SkNEW_ARGS(BitmapTextBatch, (geometry, maskFormat, glyphCount, fontCache));
    }

    const char* name() const override { return "BitmapTextBatch"; }

    void getInvariantOutputColor(GrInitInvariantOutput* out) const override {
        if (kARGB_GrMaskFormat == fMaskFormat) {
            out->setUnknownFourComponents();
        } else {
            out->setKnownFourComponents(fBatch.fColor);
        }
    }

    void getInvariantOutputCoverage(GrInitInvariantOutput* out) const override {
        if (kARGB_GrMaskFormat != fMaskFormat) {
            if (GrPixelConfigIsAlphaOnly(fPixelConfig)) {
                out->setUnknownSingleComponent();
            } else if (GrPixelConfigIsOpaque(fPixelConfig)) {
                out->setUnknownOpaqueFourComponents();
                out->setUsingLCDCoverage();
            } else {
                out->setUnknownFourComponents();
                out->setUsingLCDCoverage();
            }
        } else {
            out->setKnownSingleComponent(0xff);
        }
    }

    void initBatchTracker(const GrPipelineInfo& init) override {
        // Handle any color overrides
        if (init.fColorIgnored) {
            fBatch.fColor = GrColor_ILLEGAL;
        } else if (GrColor_ILLEGAL != init.fOverrideColor) {
            fBatch.fColor = init.fOverrideColor;
        }

        // setup batch properties
        fBatch.fColorIgnored = init.fColorIgnored;
        fBatch.fUsesLocalCoords = init.fUsesLocalCoords;
        fBatch.fCoverageIgnored = init.fCoverageIgnored;
    }

    void generateGeometry(GrBatchTarget* batchTarget, const GrPipeline* pipeline) override {
        // if we have RGB, then we won't have any SkShaders so no need to use a localmatrix.
        // TODO actually only invert if we don't have RGBA
        SkMatrix localMatrix;
        if (this->usesLocalCoords() && !this->viewMatrix().invert(&localMatrix)) {
            SkDebugf("Cannot invert viewmatrix\n");
            return;
        }

        GrTexture* texture = fFontCache->getTexture(fMaskFormat);
        if (!texture) {
            SkDebugf("Could not allocate backing texture for atlas\n");
            return;
        }

        GrTextureParams params(SkShader::kClamp_TileMode, GrTextureParams::kNone_FilterMode);

        // This will be ignored in the non A8 case
        bool opaqueVertexColors = GrColorIsOpaque(this->color());
        SkAutoTUnref<const GrGeometryProcessor> gp(
                GrBitmapTextGeoProc::Create(this->color(),
                                            texture,
                                            params,
                                            fMaskFormat,
                                            opaqueVertexColors,
                                            localMatrix));

        size_t vertexStride = gp->getVertexStride();
        SkASSERT(vertexStride == get_vertex_stride(fMaskFormat));

        this->initDraw(batchTarget, gp, pipeline);

        int glyphCount = this->numGlyphs();
        int instanceCount = fGeoData.count();
        const GrVertexBuffer* vertexBuffer;
        int firstVertex;

        void* vertices = batchTarget->vertexPool()->makeSpace(vertexStride,
                                                              glyphCount * kVerticesPerGlyph,
                                                              &vertexBuffer,
                                                              &firstVertex);
        if (!vertices) {
            SkDebugf("Could not allocate vertices\n");
            return;
        }

        unsigned char* currVertex = reinterpret_cast<unsigned char*>(vertices);

        // setup drawinfo
        const GrIndexBuffer* quadIndexBuffer = batchTarget->quadIndexBuffer();
        int maxInstancesPerDraw = quadIndexBuffer->maxQuads();

        GrDrawTarget::DrawInfo drawInfo;
        drawInfo.setPrimitiveType(kTriangles_GrPrimitiveType);
        drawInfo.setStartVertex(0);
        drawInfo.setStartIndex(0);
        drawInfo.setVerticesPerInstance(kVerticesPerGlyph);
        drawInfo.setIndicesPerInstance(kIndicesPerGlyph);
        drawInfo.adjustStartVertex(firstVertex);
        drawInfo.setVertexBuffer(vertexBuffer);
        drawInfo.setIndexBuffer(quadIndexBuffer);

        int instancesToFlush = 0;
        for (int i = 0; i < instanceCount; i++) {
            Geometry& args = fGeoData[i];
            Blob* blob = args.fBlob;
            Run& run = blob->fRuns[args.fRun];
            TextInfo& info = run.fSubRunInfo[args.fSubRun];

            uint64_t currentAtlasGen = fFontCache->atlasGeneration(fMaskFormat);
            bool regenerateTextureCoords = info.fAtlasGeneration != currentAtlasGen;
            bool regenerateColors = kA8_GrMaskFormat == fMaskFormat && run.fColor != args.fColor;
            bool regeneratePositions = args.fTransX != 0.f || args.fTransY != 0.f;
            int glyphCount = info.fGlyphEndIndex - info.fGlyphStartIndex;

            // We regenerate both texture coords and colors in the blob itself, and update the
            // atlas generation.  If we don't end up purging any unused plots, we can avoid
            // regenerating the coords.  We could take a finer grained approach to updating texture
            // coords but its not clear if the extra bookkeeping would offset any gains.
            // To avoid looping over the glyphs twice, we do one loop and conditionally update color
            // or coords as needed.  One final note, if we have to break a run for an atlas eviction
            // then we can't really trust the atlas has all of the correct data.  Atlas evictions
            // should be pretty rare, so we just always regenerate in those cases
            if (regenerateTextureCoords || regenerateColors || regeneratePositions) {
                // first regenerate texture coordinates / colors if need be
                const SkDescriptor* desc = NULL;
                SkGlyphCache* cache = NULL;
                GrFontScaler* scaler = NULL;
                GrBatchTextStrike* strike = NULL;
                bool brokenRun = false;
                if (regenerateTextureCoords) {
                    info.fBulkUseToken.reset();
                    desc = run.fDescriptor.getDesc();
                    cache = SkGlyphCache::DetachCache(run.fTypeface, desc);
                    scaler = GrTextContext::GetGrFontScaler(cache);
                    strike = fFontCache->getStrike(scaler);
                }
                for (int glyphIdx = 0; glyphIdx < glyphCount; glyphIdx++) {
                    GrGlyph::PackedID glyphID = blob->fGlyphIDs[glyphIdx + info.fGlyphStartIndex];

                    if (regenerateTextureCoords) {
                        // Upload the glyph only if needed
                        GrGlyph* glyph = strike->getGlyph(glyphID, scaler);
                        SkASSERT(glyph);

                        if (!fFontCache->hasGlyph(glyph) &&
                            !strike->addGlyphToAtlas(batchTarget, glyph, scaler)) {
                            this->flush(batchTarget, &drawInfo, instancesToFlush,
                                        maxInstancesPerDraw);
                            this->initDraw(batchTarget, gp, pipeline);
                            instancesToFlush = 0;
                            brokenRun = glyphIdx > 0;

                            SkDEBUGCODE(bool success =) strike->addGlyphToAtlas(batchTarget, glyph,
                                                                                scaler);
                            SkASSERT(success);
                        }
                        fFontCache->addGlyphToBulkAndSetUseToken(&info.fBulkUseToken, glyph,
                                                                 batchTarget->currentToken());

                        // Texture coords are the last vertex attribute so we get a pointer to the
                        // first one and then map with stride in regenerateTextureCoords
                        intptr_t vertex = reinterpret_cast<intptr_t>(blob->fVertices);
                        vertex += info.fVertexStartIndex;
                        vertex += vertexStride * glyphIdx * kVerticesPerGlyph;
                        vertex += vertexStride - sizeof(SkIPoint16);

                        this->regenerateTextureCoords(glyph, vertex, vertexStride);
                    }

                    if (regenerateColors) {
                        intptr_t vertex = reinterpret_cast<intptr_t>(blob->fVertices);
                        vertex += info.fVertexStartIndex;
                        vertex += vertexStride * glyphIdx * kVerticesPerGlyph + sizeof(SkPoint);
                        this->regenerateColors(vertex, vertexStride, args.fColor);
                    }

                    if (regeneratePositions) {
                        intptr_t vertex = reinterpret_cast<intptr_t>(blob->fVertices);
                        vertex += info.fVertexStartIndex;
                        vertex += vertexStride * glyphIdx * kVerticesPerGlyph;
                        SkScalar transX = args.fTransX;
                        SkScalar transY = args.fTransY;
                        this->regeneratePositions(vertex, vertexStride, transX, transY);
                    }
                    instancesToFlush++;
                }

                // We my have changed the color so update it here
                run.fColor = args.fColor;
                if (regenerateTextureCoords) {
                    SkGlyphCache::AttachCache(cache);
                    info.fAtlasGeneration = brokenRun ? GrBatchAtlas::kInvalidAtlasGeneration :
                                                        fFontCache->atlasGeneration(fMaskFormat);
                }
            } else {
                instancesToFlush += glyphCount;

                // set use tokens for all of the glyphs in our subrun.  This is only valid if we
                // have a valid atlas generation
                fFontCache->setUseTokenBulk(info.fBulkUseToken,
                                            batchTarget->currentToken(),
                                            fMaskFormat);
            }

            // now copy all vertices
            size_t byteCount = info.fVertexEndIndex - info.fVertexStartIndex;
            memcpy(currVertex, blob->fVertices + info.fVertexStartIndex, byteCount);

            currVertex += byteCount;
        }

        this->flush(batchTarget, &drawInfo, instancesToFlush, maxInstancesPerDraw);
    }

    SkSTArray<1, Geometry, true>* geoData() { return &fGeoData; }

private:
    BitmapTextBatch(const Geometry& geometry, GrMaskFormat maskFormat,
                    int glyphCount, GrBatchFontCache* fontCache)
            : fMaskFormat(maskFormat)
            , fPixelConfig(fontCache->getPixelConfig(maskFormat))
            , fFontCache(fontCache) {
        this->initClassID<BitmapTextBatch>();
        fGeoData.push_back(geometry);
        fBatch.fColor = geometry.fColor;
        fBatch.fViewMatrix = geometry.fBlob->fViewMatrix;
        fBatch.fNumGlyphs = glyphCount;
    }

    void regenerateTextureCoords(GrGlyph* glyph, intptr_t vertex, size_t vertexStride) {
        int width = glyph->fBounds.width();
        int height = glyph->fBounds.height();
        int u0 = glyph->fAtlasLocation.fX;
        int v0 = glyph->fAtlasLocation.fY;
        int u1 = u0 + width;
        int v1 = v0 + height;

        // we assume texture coords are the last vertex attribute, this is a bit fragile.
        // TODO pass in this offset or something
        SkIPoint16* textureCoords;
        // V0
        textureCoords = reinterpret_cast<SkIPoint16*>(vertex);
        textureCoords->set(u0, v0);
        vertex += vertexStride;

        // V1
        textureCoords = reinterpret_cast<SkIPoint16*>(vertex);
        textureCoords->set(u0, v1);
        vertex += vertexStride;

        // V2
        textureCoords = reinterpret_cast<SkIPoint16*>(vertex);
        textureCoords->set(u1, v1);
        vertex += vertexStride;

        // V3
        textureCoords = reinterpret_cast<SkIPoint16*>(vertex);
        textureCoords->set(u1, v0);
    }

    void regenerateColors(intptr_t vertex, size_t vertexStride, GrColor color) {
        for (int i = 0; i < kVerticesPerGlyph; i++) {
            SkColor* vcolor = reinterpret_cast<SkColor*>(vertex);
            *vcolor = color;
            vertex += vertexStride;
        }
    }

    void regeneratePositions(intptr_t vertex, size_t vertexStride, SkScalar transX,
                             SkScalar transY) {
        for (int i = 0; i < kVerticesPerGlyph; i++) {
            SkPoint* point = reinterpret_cast<SkPoint*>(vertex);
            point->fX += transX;
            point->fY += transY;
            vertex += vertexStride;
        }
    }

    void initDraw(GrBatchTarget* batchTarget,
                  const GrGeometryProcessor* gp,
                  const GrPipeline* pipeline) {
        batchTarget->initDraw(gp, pipeline);

        // TODO remove this when batch is everywhere
        GrPipelineInfo init;
        init.fColorIgnored = fBatch.fColorIgnored;
        init.fOverrideColor = GrColor_ILLEGAL;
        init.fCoverageIgnored = fBatch.fCoverageIgnored;
        init.fUsesLocalCoords = this->usesLocalCoords();
        gp->initBatchTracker(batchTarget->currentBatchTracker(), init);
    }

    void flush(GrBatchTarget* batchTarget,
               GrDrawTarget::DrawInfo* drawInfo,
               int instanceCount,
               int maxInstancesPerDraw) {
        while (instanceCount) {
            drawInfo->setInstanceCount(SkTMin(instanceCount, maxInstancesPerDraw));
            drawInfo->setVertexCount(drawInfo->instanceCount() * drawInfo->verticesPerInstance());
            drawInfo->setIndexCount(drawInfo->instanceCount() * drawInfo->indicesPerInstance());

            batchTarget->draw(*drawInfo);

            drawInfo->setStartVertex(drawInfo->startVertex() + drawInfo->vertexCount());
            instanceCount -= drawInfo->instanceCount();
       }
    }

    GrColor color() const { return fBatch.fColor; }
    const SkMatrix& viewMatrix() const { return fBatch.fViewMatrix; }
    bool usesLocalCoords() const { return fBatch.fUsesLocalCoords; }
    int numGlyphs() const { return fBatch.fNumGlyphs; }

    bool onCombineIfPossible(GrBatch* t) override {
        BitmapTextBatch* that = t->cast<BitmapTextBatch>();

        if (this->fMaskFormat != that->fMaskFormat) {
            return false;
        }

        if (this->fMaskFormat != kA8_GrMaskFormat && this->color() != that->color()) {
            return false;
        }

        if (this->usesLocalCoords() && !this->viewMatrix().cheapEqualTo(that->viewMatrix())) {
            return false;
        }

        fBatch.fNumGlyphs += that->numGlyphs();
        fGeoData.push_back_n(that->geoData()->count(), that->geoData()->begin());
        return true;
    }

    struct BatchTracker {
        GrColor fColor;
        SkMatrix fViewMatrix;
        bool fUsesLocalCoords;
        bool fColorIgnored;
        bool fCoverageIgnored;
        int fNumGlyphs;
    };

    BatchTracker fBatch;
    SkSTArray<1, Geometry, true> fGeoData;
    GrMaskFormat fMaskFormat;
    GrPixelConfig fPixelConfig;
    GrBatchFontCache* fFontCache;
};

void GrAtlasTextContext::flushRunAsPaths(const SkTextBlob::RunIterator& it, const SkPaint& skPaint,
                                         SkDrawFilter* drawFilter, const SkMatrix& viewMatrix,
                                         const SkIRect& clipBounds, SkScalar x, SkScalar y) {
    SkPaint runPaint = skPaint;

    size_t textLen = it.glyphCount() * sizeof(uint16_t);
    const SkPoint& offset = it.offset();

    it.applyFontToPaint(&runPaint);

    if (drawFilter && !drawFilter->filter(&runPaint, SkDrawFilter::kText_Type)) {
        return;
    }

    runPaint.setFlags(fGpuDevice->filterTextFlags(runPaint));

    switch (it.positioning()) {
        case SkTextBlob::kDefault_Positioning:
            this->drawTextAsPath(runPaint, viewMatrix, (const char *)it.glyphs(),
                                 textLen, x + offset.x(), y + offset.y(), clipBounds);
            break;
        case SkTextBlob::kHorizontal_Positioning:
            this->drawPosTextAsPath(runPaint, viewMatrix, (const char*)it.glyphs(),
                                    textLen, it.pos(), 1, SkPoint::Make(x, y + offset.y()),
                                    clipBounds);
            break;
        case SkTextBlob::kFull_Positioning:
            this->drawPosTextAsPath(runPaint, viewMatrix, (const char*)it.glyphs(),
                                    textLen, it.pos(), 2, SkPoint::Make(x, y), clipBounds);
            break;
    }
}

inline void GrAtlasTextContext::flushRun(GrDrawTarget* target, GrPipelineBuilder* pipelineBuilder,
                                         BitmapTextBlob* cacheBlob, int run, GrColor color,
                                         uint8_t paintAlpha, SkScalar transX, SkScalar transY) {
    for (int subRun = 0; subRun < cacheBlob->fRuns[run].fSubRunInfo.count(); subRun++) {
        const PerSubRunInfo& info = cacheBlob->fRuns[run].fSubRunInfo[subRun];
        int glyphCount = info.fGlyphEndIndex - info.fGlyphStartIndex;
        if (0 == glyphCount) {
            continue;
        }

        GrMaskFormat format = info.fMaskFormat;
        GrColor subRunColor = kARGB_GrMaskFormat == format ?
                              SkColorSetARGB(paintAlpha, paintAlpha, paintAlpha, paintAlpha) :
                              color;

        BitmapTextBatch::Geometry geometry;
        geometry.fBlob.reset(SkRef(cacheBlob));
        geometry.fRun = run;
        geometry.fSubRun = subRun;
        geometry.fColor = subRunColor;
        geometry.fTransX = transX;
        geometry.fTransY = transY;
        SkAutoTUnref<GrBatch> batch(BitmapTextBatch::Create(geometry, format, glyphCount,
                                                            fContext->getBatchFontCache()));

        target->drawBatch(pipelineBuilder, batch, &cacheBlob->fRuns[run].fVertexBounds);
    }
}

inline void GrAtlasTextContext::flushBigGlyphs(BitmapTextBlob* cacheBlob, GrRenderTarget* rt,
                                               const GrPaint& grPaint, const GrClip& clip,
                                               SkScalar transX, SkScalar transY) {
    for (int i = 0; i < cacheBlob->fBigGlyphs.count(); i++) {
        BitmapTextBlob::BigGlyph& bigGlyph = cacheBlob->fBigGlyphs[i];
        bigGlyph.fVx += SkScalarTruncToInt(transX);
        bigGlyph.fVy += SkScalarTruncToInt(transY);
        SkMatrix translate;
        translate.setTranslate(SkIntToScalar(bigGlyph.fVx),
                               SkIntToScalar(bigGlyph.fVy));
        SkPath tmpPath(bigGlyph.fPath);
        tmpPath.transform(translate);
        GrStrokeInfo strokeInfo(SkStrokeRec::kFill_InitStyle);
        fContext->drawPath(rt, clip, grPaint, SkMatrix::I(), tmpPath, strokeInfo);
    }
}

void GrAtlasTextContext::flush(GrDrawTarget* target,
                               const SkTextBlob* blob,
                               BitmapTextBlob* cacheBlob,
                               GrRenderTarget* rt,
                               const SkPaint& skPaint,
                               const GrPaint& grPaint,
                               SkDrawFilter* drawFilter,
                               const GrClip& clip,
                               const SkMatrix& viewMatrix,
                               const SkIRect& clipBounds,
                               SkScalar x, SkScalar y,
                               SkScalar transX, SkScalar transY) {
    // We loop through the runs of the blob, flushing each.  If any run is too large, then we flush
    // it as paths
    GrPipelineBuilder pipelineBuilder;
    pipelineBuilder.setFromPaint(grPaint, rt, clip);

    GrColor color = grPaint.getColor();
    uint8_t paintAlpha = skPaint.getAlpha();

    SkTextBlob::RunIterator it(blob);
    for (int run = 0; !it.done(); it.next(), run++) {
        if (cacheBlob->fRuns[run].fDrawAsPaths) {
            this->flushRunAsPaths(it, skPaint, drawFilter, viewMatrix, clipBounds, x, y);
            continue;
        }
        cacheBlob->fRuns[run].fVertexBounds.offset(transX, transY);
        this->flushRun(target, &pipelineBuilder, cacheBlob, run, color, paintAlpha, transX, transY);
    }

    // Now flush big glyphs
    this->flushBigGlyphs(cacheBlob, rt, grPaint, clip, transX, transY);
}

void GrAtlasTextContext::flush(GrDrawTarget* target,
                               BitmapTextBlob* cacheBlob,
                               GrRenderTarget* rt,
                               const SkPaint& skPaint,
                               const GrPaint& grPaint,
                               const GrClip& clip,
                               const SkMatrix& viewMatrix) {
    GrPipelineBuilder pipelineBuilder;
    pipelineBuilder.setFromPaint(grPaint, rt, clip);

    GrColor color = grPaint.getColor();
    uint8_t paintAlpha = skPaint.getAlpha();
    for (int run = 0; run < cacheBlob->fRunCount; run++) {
        this->flushRun(target, &pipelineBuilder, cacheBlob, run, color, paintAlpha, 0, 0);
    }

    // Now flush big glyphs
    this->flushBigGlyphs(cacheBlob, rt, grPaint, clip, 0, 0);
}
