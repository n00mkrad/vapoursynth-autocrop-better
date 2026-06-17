#include "VapourSynth.h"
#include "VSHelper.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#define OUT
#define IN

enum SideIndex {
    sideLeft = 0,
    sideRight,
    sideTop,
    sideBottom,
    sideCount
};

typedef struct {
    VSNodeRef *node;
    const VSVideoInfo *vi_finish;
    const VSVideoInfo *vi;
    int max_crop[sideCount];
    uint32_t color_min[3];
    uint32_t color_max[3];
    int pad[sideCount];
    int mod[sideCount];
    bool roundup;
} AutoCropData;

struct CropPlaneValues {
    int topArray[3];
    int bottomArray[3];
    int leftArray[3];
    int rightArray[3];
};

struct CropFrameValues {
    int top;
    int bottom;
    int left;
    int right;
    int width;
    int height;
};

int checkSubSampling(int cropValueArray[3], int subSampling, int numPlanes){
    int cropValue = cropValueArray[0];
    for (int plane = 1; plane < numPlanes; plane++) {
        cropValue = std::min(cropValue, cropValueArray[plane] << subSampling);
    }
    cropValue = cropValue - (cropValue % (1 << subSampling));

    return cropValue;
}

uint32_t scale8BitValue(uint32_t value, const VSFormat *format) {
    return value << (format->bitsPerSample - 8);
}

void setFilterError(VSMap *out, const VSAPI *vsapi, const char *filterName, const char *message) {
    std::string error = std::string(filterName) + message;
    vsapi->setError(out, error.c_str());
}

bool parseColorBounds(const VSMap *in, VSMap *out, AutoCropData *d, const VSAPI *vsapi, const char *filterName) {
    const VSFormat *format = d->vi->format;
    int numPlanes = format->numPlanes;
    uint32_t maxSample = (1u << format->bitsPerSample) - 1;
    uint32_t refColor[3] = {0, 127, 127};
    double maxColorDeviation[3] = {0.0, 0.0, 0.0};
    int nRefColors = vsapi->propNumElements(in, "ref_color");
    int nDeviations = vsapi->propNumElements(in, "max_color_deviation");

    if (nRefColors > 0) {
        if (nRefColors != 1 && nRefColors != numPlanes) {
            setFilterError(out, vsapi, filterName, ": ref_color must contain one value or one value per clip plane");
            return false;
        }
        for (int i = 0; i < nRefColors; i++) {
            int err;
            int refValue = int64ToIntS(vsapi->propGetInt(in, "ref_color", i, &err));
            if (err || refValue < 0 || refValue > 255) {
                setFilterError(out, vsapi, filterName, ": ref_color values must be in the 0...255 range");
                return false;
            }
            refColor[i] = static_cast<uint32_t>(refValue);
        }
    }

    if (nDeviations > 0) {
        if (nDeviations != 1 && nDeviations != numPlanes) {
            setFilterError(out, vsapi, filterName, ": max_color_deviation must contain one value or one value per clip plane");
            return false;
        }
        for (int i = 0; i < nDeviations; i++) {
            int err;
            double deviation = vsapi->propGetFloat(in, "max_color_deviation", i, &err);
            if (err || deviation < 0.0 || deviation > 1.0) {
                setFilterError(out, vsapi, filterName, ": max_color_deviation values must be in the 0.0...1.0 range");
                return false;
            }
            maxColorDeviation[i] = deviation;
        }
        if (nDeviations == 1) {
            for (int plane = 1; plane < numPlanes; plane++) {
                maxColorDeviation[plane] = maxColorDeviation[0];
            }
        }
    }

    for (int plane = 0; plane < numPlanes; plane++) {
        // Convert the 8-bit-style reference color and normalized tolerance to native sample bounds.
        uint32_t scaledRefColor = scale8BitValue(refColor[plane], format);
        uint32_t colorDeviation = static_cast<uint32_t>(std::round(maxColorDeviation[plane] * maxSample));
        d->color_min[plane] = colorDeviation > scaledRefColor ? 0 : scaledRefColor - colorDeviation;
        d->color_max[plane] = std::min(maxSample, scaledRefColor + colorDeviation);
    }

    return true;
}

bool parseSideArray(const VSMap *in, VSMap *out, int values[sideCount], const VSAPI *vsapi, const char *filterName, const char *key, int defaultValue, int minValue) {
    for (int i = 0; i < sideCount; i++) {
        values[i] = defaultValue;
    }

    int numValues = vsapi->propNumElements(in, key);
    if (numValues < 0) {
        return true;
    }
    if (numValues != 1 && numValues != sideCount) {
        std::string message = std::string(": ") + key + " must contain one value or four values in left, right, top, bottom order";
        setFilterError(out, vsapi, filterName, message.c_str());
        return false;
    }

    for (int i = 0; i < numValues; i++) {
        int err;
        int value = int64ToIntS(vsapi->propGetInt(in, key, i, &err));
        if (err || value < minValue) {
            std::string message = std::string(": ") + key + " values must be at least " + std::to_string(minValue);
            setFilterError(out, vsapi, filterName, message.c_str());
            return false;
        }
        values[i] = value;
    }
    if (numValues == 1) {
        for (int i = 1; i < sideCount; i++) {
            values[i] = values[0];
        }
    }

    return true;
}

bool parseCropAdjustments(const VSMap *in, VSMap *out, AutoCropData *d, const VSAPI *vsapi, const char *filterName) {
    const VSFormat *format = d->vi->format;
    if (!parseSideArray(in, out, d->pad, vsapi, filterName, "pad", 0, 0) || !parseSideArray(in, out, d->mod, vsapi, filterName, "mod", 2, 1)) {
        return false;
    }

    int err;
    d->roundup = vsapi->propGetInt(in, "roundup", 0, &err) != 0;
    if (err) {
        d->roundup = false;
    }

    int horizontalFactor = 1 << format->subSamplingW;
    int verticalFactor = 1 << format->subSamplingH;
    if (d->mod[sideLeft] % horizontalFactor != 0 || d->mod[sideRight] % horizontalFactor != 0) {
        setFilterError(out, vsapi, filterName, ": left and right mod values must be divisible by the horizontal subsampling factor");
        return false;
    }
    if (d->mod[sideTop] % verticalFactor != 0 || d->mod[sideBottom] % verticalFactor != 0) {
        setFilterError(out, vsapi, filterName, ": top and bottom mod values must be divisible by the vertical subsampling factor");
        return false;
    }

    return true;
}

bool validateSubsamplingSideArray(VSMap *out, int values[sideCount], const VSFormat *format, const VSAPI *vsapi, const char *filterName, const char *key) {
    int horizontalFactor = 1 << format->subSamplingW;
    int verticalFactor = 1 << format->subSamplingH;
    if (values[sideLeft] % horizontalFactor != 0 || values[sideRight] % horizontalFactor != 0) {
        std::string message = std::string(": left and right ") + key + " values must be divisible by the horizontal subsampling factor";
        setFilterError(out, vsapi, filterName, message.c_str());
        return false;
    }
    if (values[sideTop] % verticalFactor != 0 || values[sideBottom] % verticalFactor != 0) {
        std::string message = std::string(": top and bottom ") + key + " values must be divisible by the vertical subsampling factor";
        setFilterError(out, vsapi, filterName, message.c_str());
        return false;
    }

    return true;
}

bool parseMaxCrop(const VSMap *in, VSMap *out, AutoCropData *d, const VSAPI *vsapi, const char *filterName) {
    if (!parseSideArray(in, out, d->max_crop, vsapi, filterName, "max_crop", 16, 0)) {
        return false;
    }

    return validateSubsamplingSideArray(out, d->max_crop, d->vi->format, vsapi, filterName, "max_crop");
}

int64_t roundCropValue(int64_t value, int mod, bool roundup) {
    int64_t remainder = value % mod;
    if (remainder == 0) {
        return value;
    }

    return roundup ? value + mod - remainder : value - remainder;
}

int clampCropValue(int64_t value, int maxValue, int mod) {
    if (maxValue < 0) {
        return 0;
    }
    if (value <= maxValue) {
        return static_cast<int>(value);
    }

    return maxValue - (maxValue % mod);
}

void applyCropAdjustments(AutoCropData *data, CropFrameValues *cFrame) {
    const VSFormat *format = data->vi->format;
    int minWidth = 1 << format->subSamplingW;
    int minHeight = 1 << format->subSamplingH;

    int64_t left = roundCropValue(static_cast<int64_t>(cFrame->left) + data->pad[sideLeft], data->mod[sideLeft], data->roundup);
    int64_t right = roundCropValue(static_cast<int64_t>(cFrame->right) + data->pad[sideRight], data->mod[sideRight], data->roundup);
    int64_t top = roundCropValue(static_cast<int64_t>(cFrame->top) + data->pad[sideTop], data->mod[sideTop], data->roundup);
    int64_t bottom = roundCropValue(static_cast<int64_t>(cFrame->bottom) + data->pad[sideBottom], data->mod[sideBottom], data->roundup);

    cFrame->left = clampCropValue(left, data->vi->width - minWidth, data->mod[sideLeft]);
    cFrame->right = clampCropValue(right, data->vi->width - minWidth - cFrame->left, data->mod[sideRight]);
    cFrame->top = clampCropValue(top, data->vi->height - minHeight, data->mod[sideTop]);
    cFrame->bottom = clampCropValue(bottom, data->vi->height - minHeight - cFrame->top, data->mod[sideBottom]);
}

template <typename Bit>
void getCropValues(CropPlaneValues *c, const Bit *srcp, int src_stride, int w, int h, uint32_t color, uint32_t color2,
                   int topRange, int bottomRange, int leftRange, int rightRange, int plane) {
    int topValue = 0;
    int bottomValue = bottomRange;
    int leftValue = leftRange;
    int rightValue = rightRange;
    for (int y = 0; y < h; y++) {
        //top
        if (y < topRange) {
            for (int x = 0; x < w; x += 10) {
                if (!(color <= srcp[x] && srcp[x] <= color2)) {
                    topRange = 0;
                    break;
                }
            }
            if (topRange) {
                topValue++;
            }
        }
        //left
        for (int x = 0; x < leftRange; x++) {
            if (!(color <= srcp[x] && srcp[x] <= color2)) {
                if (leftValue >= x) {
                    leftValue = x;
                }
            }
        }
        //right
        for (int x = w - rightRange; x < w; x++) {
            if (!(color <= srcp[x] && srcp[x] <= color2)) {
                if (rightValue >= w - x) {
                    rightValue = w - x - 1;
                }
            }
        }
        //bottom
        if (y >= h - bottomRange) {
            for (int x = 0; x < w; x += 10) {
                if (!(color <= srcp[x] && srcp[x] <= color2)) {
                    bottomValue = h - (y + 1);
                    break;
                }
            }
        }
        srcp += src_stride;
    }

    c->topArray[plane] = topValue;
    c->bottomArray[plane] = bottomValue;
    c->leftArray[plane] = leftValue;
    c->rightArray[plane] = rightValue;
}

template <typename Bit>
void getFramePlane(const VSFrameRef *src, AutoCropData *data, const VSAPI *vsapi, CropFrameValues *cFrame) {
    CropPlaneValues cPlane{};
    const VSFormat *fi = data->vi->format;

    for (int plane = 0; plane < fi->numPlanes; plane++) {
        const auto *srcp = (const Bit *) vsapi->getReadPtr(src, plane);
        int src_stride = vsapi->getStride(src, plane)  / sizeof(Bit);
        int h = vsapi->getFrameHeight(src, plane);
        int w = vsapi->getFrameWidth(src, plane);
        int verticalShift = plane ? fi->subSamplingH : 0;
        int horizontalShift = plane ? fi->subSamplingW : 0;
        int topRange = std::min(h, data->max_crop[sideTop] >> verticalShift);
        int bottomRange = std::min(h, data->max_crop[sideBottom] >> verticalShift);
        int leftRange = std::min(w, data->max_crop[sideLeft] >> horizontalShift);
        int rightRange = std::min(w, data->max_crop[sideRight] >> horizontalShift);

        getCropValues<Bit>(&cPlane, srcp, src_stride, w, h, data->color_min[plane], data->color_max[plane], topRange, bottomRange, leftRange, rightRange, plane);
    }

    cFrame->top    = checkSubSampling(cPlane.topArray, data->vi->format->subSamplingH, fi->numPlanes);
    cFrame->bottom = checkSubSampling(cPlane.bottomArray, data->vi->format->subSamplingH, fi->numPlanes);
    cFrame->left   = checkSubSampling(cPlane.leftArray, data->vi->format->subSamplingW, fi->numPlanes);
    cFrame->right  = checkSubSampling(cPlane.rightArray, data->vi->format->subSamplingW, fi->numPlanes);
    applyCropAdjustments(data, cFrame);
    cFrame->height   = data->vi->height - cFrame->top - cFrame->bottom;
    cFrame->width    = data->vi->width - cFrame->left - cFrame->right;
}

/////////////////
// CropValues

static void VS_CC cropValuesInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    auto *data = (AutoCropData *) * instanceData;
    VSVideoInfo vi_finish = *data->vi_finish;
    vsapi->setVideoInfo(&vi_finish, 1, node);
}

static const VSFrameRef *VS_CC cropValuesGetFrame(int n, int activationReason, void **instanceData, void **frameData,
                                                  VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    auto *data = (AutoCropData *) * instanceData;
    const char *top = "CropTopValue";
    const char *bottom = "CropBottomValue";
    const char *left = "CropLeftValue";
    const char *right = "CropRightValue";

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, data->node, frameCtx);
    }
    else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src = vsapi->getFrameFilter(n, data->node, frameCtx);
        const VSFormat *fi = data->vi->format;
        CropFrameValues cFrame{};

        if (data->vi->format->sampleType == stInteger && data->vi->format->bitsPerSample == 8) {
            getFramePlane<uint8_t>(src, data, vsapi, &cFrame);
        } else if (data->vi->format->sampleType == stInteger && data->vi->format->bitsPerSample <= 16){
            getFramePlane<uint16_t>(src, data, vsapi, &cFrame);
        }

        VSFrameRef *dst = vsapi->copyFrame(src, core);
        for (int plane = 0; plane < fi->numPlanes; plane++) {
            vsapi->getWritePtr(dst, plane);
        }

        VSMap *dstProps = vsapi->getFramePropsRW(dst);

        if (fi->sampleType == stInteger) {
            vsapi->propSetInt(dstProps, top, cFrame.top, paAppend);
            vsapi->propSetInt(dstProps, bottom, cFrame.bottom, paAppend);
            vsapi->propSetInt(dstProps, left, cFrame.left, paAppend);
            vsapi->propSetInt(dstProps, right, cFrame.right, paAppend);
        }
        vsapi->freeFrame(src);

        return dst;
    }

    return 0;
}

static void VS_CC cropValuesFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    auto *data = (AutoCropData *)instanceData;
    vsapi->freeNode(data->node);
    free(data);
}

static void VS_CC cropValuesCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    AutoCropData d;
    AutoCropData *data;

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi_finish = vsapi->getVideoInfo(d.node);
    d.vi = vsapi->getVideoInfo(d.node);

    if (!isConstantFormat(d.vi) || d.vi->format->sampleType != stInteger || d.vi->format->bitsPerSample < 8 || d.vi->format->bitsPerSample > 16) {
        vsapi->setError(out, "CropValues: only constant format 8...16Bit integer input supported");
        vsapi->freeNode(d.node);
        return;
    }

    if (!(d.vi->format->colorFamily == cmGray || d.vi->format->colorFamily == cmYCoCg || d.vi->format->colorFamily == cmYUV)){
        vsapi->setError(out, "CropValues: only GRAY, YUV or YCoCg input supported");
        vsapi->freeNode(d.node);
        return;
    }

    if (!parseMaxCrop(in, out, &d, vsapi, "CropValues")) {
        vsapi->freeNode(d.node);
        return;
    }
    if (!parseColorBounds(in, out, &d, vsapi, "CropValues")) {
        vsapi->freeNode(d.node);
        return;
    }
    if (!parseCropAdjustments(in, out, &d, vsapi, "CropValues")) {
        vsapi->freeNode(d.node);
        return;
    }

    data = static_cast<AutoCropData*>(malloc(sizeof(d)));
    *data = d;

    vsapi->createFilter(in, out, "CropValues", cropValuesInit, cropValuesGetFrame, cropValuesFree, fmParallel, 0, data, core);
}

/////////////////
// AutoCrop

static void VS_CC autocropInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    auto *data = (AutoCropData *) * instanceData;
    VSVideoInfo vi_finish = *data->vi_finish;
    vi_finish.height = 0;
    vi_finish.width = 0;
    vsapi->setVideoInfo(&vi_finish, 1, node);
}

static const VSFrameRef *VS_CC autocropGetFrame(int n, int activationReason, void **instanceData, void **frameData,
                                                VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    auto *data = (AutoCropData *) * instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, data->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src = vsapi->getFrameFilter(n, data->node, frameCtx);
        const VSFormat *fi = data->vi->format;
        CropFrameValues cFrame{};

        if (data->vi->format->sampleType == stInteger && data->vi->format->bitsPerSample == 8) {
            getFramePlane<uint8_t>(src, data, vsapi, &cFrame);
        } else if (data->vi->format->sampleType == stInteger && data->vi->format->bitsPerSample <= 16){
            getFramePlane<uint16_t>(src, data, vsapi, &cFrame);
        }

        //from https://github.com/vapoursynth/vapoursynth/blob/738f2be63b8d2b19c73b5e20116058f12f9b278d/src/core/simplefilters.c#L132
        VSFrameRef *dst_finish = vsapi->newVideoFrame(fi, cFrame.width, cFrame.height, src, core);
        for (int plane_new = 0; plane_new < fi->numPlanes; plane_new++) {
            int srcstride = vsapi->getStride(src, plane_new);
            int dststride = vsapi->getStride(dst_finish, plane_new);
            const uint8_t *srcdata = vsapi->getReadPtr(src, plane_new);
            uint8_t *dstdata = vsapi->getWritePtr(dst_finish, plane_new);
            srcdata += srcstride * (cFrame.top >> (plane_new ? fi->subSamplingH : 0));
            srcdata += (cFrame.left >> (plane_new ? fi->subSamplingW : 0)) * fi->bytesPerSample;
            vs_bitblt(dstdata, dststride, srcdata, srcstride,
                      (cFrame.width >> (plane_new ? fi->subSamplingW : 0)) * fi->bytesPerSample,
                      vsapi->getFrameHeight(dst_finish, plane_new));
        }
        vsapi->freeFrame(src);

        return dst_finish;
        }

    return 0;
}

static void VS_CC autocropFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    auto *data = (AutoCropData *)instanceData;
    vsapi->freeNode(data->node);
    free(data);
}

static void VS_CC autocropCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    AutoCropData d;
    AutoCropData *data;

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi_finish = vsapi->getVideoInfo(d.node);
    d.vi = vsapi->getVideoInfo(d.node);

    if (!isConstantFormat(d.vi) || d.vi->format->sampleType != stInteger || d.vi->format->bitsPerSample < 8 || d.vi->format->bitsPerSample > 16) {
        vsapi->setError(out, "AutoCrop: only constant format 8...16Bit integer input supported");
        vsapi->freeNode(d.node);
        return;
    }

    if (!(d.vi->format->colorFamily == cmGray || d.vi->format->colorFamily == cmYCoCg || d.vi->format->colorFamily == cmYUV)){
        vsapi->setError(out, "AutoCrop: only GRAY, YUV or YCoCg input supported");
        vsapi->freeNode(d.node);
        return;
    }

    if (!parseMaxCrop(in, out, &d, vsapi, "AutoCrop")) {
        vsapi->freeNode(d.node);
        return;
    }
    if (!parseColorBounds(in, out, &d, vsapi, "AutoCrop")) {
        vsapi->freeNode(d.node);
        return;
    }
    if (!parseCropAdjustments(in, out, &d, vsapi, "AutoCrop")) {
        vsapi->freeNode(d.node);
        return;
    }
    data = static_cast<AutoCropData*>(malloc(sizeof(d)));
    *data = d;

    vsapi->createFilter(in, out, "AutoCrop", autocropInit, autocropGetFrame, autocropFree, fmParallel, 0, data, core);
}

//////////////////////////////////////////
// Init

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("moe.infi.autocrop", "acrop", "VapourSynth AutoCrop", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("AutoCrop", "clip:clip;max_crop:int[]:opt;ref_color:int[]:opt;max_color_deviation:float[]:opt;pad:int[]:opt;mod:int[]:opt;roundup:int:opt", autocropCreate, 0, plugin);
    registerFunc("CropValues", "clip:clip;max_crop:int[]:opt;ref_color:int[]:opt;max_color_deviation:float[]:opt;pad:int[]:opt;mod:int[]:opt;roundup:int:opt", cropValuesCreate, 0, plugin);
}
