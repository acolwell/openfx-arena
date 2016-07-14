/*
 * This file is part of openfx-arena <https://github.com/olear/openfx-arena>,
 * Copyright (C) 2015, 2016 FxArena DA
 * Copyright (C) 2016 INRIA
 *
 * openfx-arena is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * openfx-arena is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with openfx-arena.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
*/

#include "ofxsMacros.h"
#include "ofxsImageEffect.h"
#include "openCLUtilities.hpp"
#include <cmath>

#define kPluginName "SwirlCL"
#define kPluginGrouping "Extra/Distort"
#define kPluginIdentifier "fr.inria.openfx.SwirlCL"
#define kPluginDescription "OpenCL Swirl Filter"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

#define kSupportsTiles 0
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kRenderThreadSafety eRenderFullySafe
#define kHostFrameThreading false
#define kHostMasking true
#define kHostMixing true

#define kParamAmount "amount"
#define kParamAmountLabel "Amount"
#define kParamAmountHint "Adjust swirl amount"
#define kParamAmountDefault 45

#define kParamRadius "radius"
#define kParamRadiusLabel "Radius"
#define kParamRadiusHint "Adjust swirl radius"
#define kParamRadiusDefault 100

#define kParamKernel

using namespace OFX;

class SwirlCLPlugin : public ImageEffect
{
public:
    SwirlCLPlugin(OfxImageEffectHandle handle);
    virtual ~SwirlCLPlugin();
    virtual void render(const RenderArguments &args) OVERRIDE FINAL;
    virtual bool getRegionOfDefinition(const RegionOfDefinitionArguments &args, OfxRectD &rod) OVERRIDE FINAL;
private:
    Clip *dstClip_;
    Clip *srcClip_;
    cl::Context context_;
    cl::Program program_;
    DoubleParam *amount_;
    DoubleParam *radius_;
};

SwirlCLPlugin::SwirlCLPlugin(OfxImageEffectHandle handle)
: ImageEffect(handle)
, dstClip_(0)
, srcClip_(0)
, amount_(0)
, radius_(0)
{
    dstClip_ = fetchClip(kOfxImageEffectOutputClipName);
    assert(dstClip_ && dstClip_->getPixelComponents() == ePixelComponentRGBA);
    srcClip_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
    assert(srcClip_ && srcClip_->getPixelComponents() == ePixelComponentRGBA);

    amount_ = fetchDoubleParam(kParamAmount);
    radius_ = fetchDoubleParam(kParamRadius);
    assert(amount_ && radius_);

    // setup opencl context and build kernel
    context_ = createCLContext();
    program_ = buildProgramFromSource(context_, "test.cl"); // still testing kernel
}

SwirlCLPlugin::~SwirlCLPlugin()
{
}

void SwirlCLPlugin::render(const RenderArguments &args)
{
    // render scale
    if (!kSupportsRenderScale && (args.renderScale.x != 1. || args.renderScale.y != 1.)) {
        throwSuiteStatusException(kOfxStatFailed);
        return;
    }

    // get src clip
    if (!srcClip_) {
        throwSuiteStatusException(kOfxStatFailed);
        return;
    }
    assert(srcClip_);
    std::auto_ptr<const Image> srcImg(srcClip_->fetchImage(args.time));
    OfxRectI srcRod,srcBounds;
    if (srcImg.get()) {
        srcRod = srcImg->getRegionOfDefinition();
        srcBounds = srcImg->getBounds();
        if (srcImg->getRenderScale().x != args.renderScale.x ||
            srcImg->getRenderScale().y != args.renderScale.y ||
            srcImg->getField() != args.fieldToRender) {
            setPersistentMessage(Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
            throwSuiteStatusException(kOfxStatFailed);
            return;
        }
    } else {
        throwSuiteStatusException(kOfxStatFailed);
        return;
    }

    // get dest clip
    if (!dstClip_) {
        throwSuiteStatusException(kOfxStatFailed);
        return;
    }
    assert(dstClip_);
    std::auto_ptr<Image> dstImg(dstClip_->fetchImage(args.time));
    if (!dstImg.get()) {
        throwSuiteStatusException(kOfxStatFailed);
        return;
    }
    if (dstImg->getRenderScale().x != args.renderScale.x ||
        dstImg->getRenderScale().y != args.renderScale.y ||
        dstImg->getField() != args.fieldToRender) {
        setPersistentMessage(Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
        throwSuiteStatusException(kOfxStatFailed);
        return;
    }

    // get bit depth
    BitDepthEnum dstBitDepth = dstImg->getPixelDepth();
    if (dstBitDepth != eBitDepthFloat || (srcImg.get() && (dstBitDepth != srcImg->getPixelDepth()))) {
        throwSuiteStatusException(kOfxStatErrFormat);
        return;
    }

    // get pixel component
    PixelComponentEnum dstComponents  = dstImg->getPixelComponents();
    if (dstComponents != ePixelComponentRGBA|| (srcImg.get() && (dstComponents != srcImg->getPixelComponents()))) {
        throwSuiteStatusException(kOfxStatErrFormat);
        return;
    }

    // are we in the image bounds?
    OfxRectI dstBounds = dstImg->getBounds();
    if(args.renderWindow.x1 < dstBounds.x1 || args.renderWindow.x1 >= dstBounds.x2 || args.renderWindow.y1 < dstBounds.y1 || args.renderWindow.y1 >= dstBounds.y2 ||
       args.renderWindow.x2 <= dstBounds.x1 || args.renderWindow.x2 > dstBounds.x2 || args.renderWindow.y2 <= dstBounds.y1 || args.renderWindow.y2 > dstBounds.y2) {
        throwSuiteStatusException(kOfxStatErrValue);
        return;
    }

    // get params
    double amount, radius;
    amount_->getValueAtTime(args.time, amount);
    radius_->getValueAtTime(args.time, radius);

    // setup kernel
    cl::Kernel kernel(program_, "swirl");
    // kernel arg 0 & 1 is reserved for input & output
    kernel.setArg(2, std::floor(amount * args.renderScale.x + 0.5));
    kernel.setArg(3, std::floor(radius * args.renderScale.x + 0.5));

    // get available devices
    VECTOR_CLASS<cl::Device> devices = context_.getInfo<CL_CONTEXT_DEVICES>();

    // render
    renderCL((float*)dstImg->getPixelData(), (float*)srcImg->getPixelData(), args.renderWindow.x2 - args.renderWindow.x1, args.renderWindow.y2 - args.renderWindow.y1, context_, devices[0], kernel);
}

bool SwirlCLPlugin::getRegionOfDefinition(const RegionOfDefinitionArguments &args, OfxRectD &rod)
{
    if (!kSupportsRenderScale && (args.renderScale.x != 1. || args.renderScale.y != 1.)) {
        throwSuiteStatusException(kOfxStatFailed);
        return false;
    }
    if (srcClip_ && srcClip_->isConnected()) {
        rod = srcClip_->getRegionOfDefinition(args.time);
    } else {
        rod.x1 = rod.y1 = kOfxFlagInfiniteMin;
        rod.x2 = rod.y2 = kOfxFlagInfiniteMax;
    }
    return true;
}

mDeclarePluginFactory(SwirlCLPluginFactory, {}, {});

/** @brief The basic describe function, passed a plugin descriptor */
void SwirlCLPluginFactory::describe(ImageEffectDescriptor &desc)
{
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    // add the supported contexts
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedContext(eContextFilter);

    // add supported pixel depths
    desc.addSupportedBitDepth(eBitDepthFloat);

    // add other
    desc.setSupportsTiles(kSupportsTiles);
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    desc.setRenderThreadSafety(kRenderThreadSafety);
    desc.setHostFrameThreading(kHostFrameThreading);
    desc.setHostMaskingEnabled(kHostMasking);
    desc.setHostMixingEnabled(kHostMixing);
}

/** @brief The describe in context function, passed a plugin descriptor and a context */
void SwirlCLPluginFactory::describeInContext(ImageEffectDescriptor &desc, ContextEnum /*context*/)
{
    // create the mandated source clip
    ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    // create the mandated output clip
    ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->setSupportsTiles(kSupportsTiles);

    // create param(s)
    PageParamDescriptor *page = desc.definePageParam(kPluginName);
    {
        DoubleParamDescriptor *param = desc.defineDoubleParam(kParamAmount);
        param->setLabel(kParamAmountLabel);
        param->setHint(kParamAmountHint);
        param->setRange(-360, 360);
        param->setDisplayRange(-10, 10);
        param->setDefault(kParamAmountDefault);
        page->addChild(*param);
    }
    {
        DoubleParamDescriptor *param = desc.defineDoubleParam(kParamRadius);
        param->setLabel(kParamRadiusLabel);
        param->setHint(kParamRadiusHint);
        param->setRange(0, 10000);
        param->setDisplayRange(0, 1000);
        param->setDefault(kParamRadiusDefault);
        page->addChild(*param);
    }
}

/** @brief The create instance function, the plugin must return an object derived from the \ref ImageEffect class */
ImageEffect* SwirlCLPluginFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    return new SwirlCLPlugin(handle);
}

static SwirlCLPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)
