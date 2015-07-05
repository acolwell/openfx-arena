/*

openfx-arena - https://github.com/olear/openfx-arena

Copyright (c) 2015, Ole-André Rodlie <olear@fxarena.net>
Copyright (c) 2015, FxArena DA <mail@fxarena.net>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Neither the name of FxArena DA nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "Texture.h"
#include "ofxsMacros.h"
#include "ofxNatron.h"
#include <Magick++.h>

#define kPluginName "Texture"
#define kPluginGrouping "Draw"

#define kPluginIdentifier "net.fxarena.openfx.Texture"
#define kPluginVersionMajor 3
#define kPluginVersionMinor 3

#define kSupportsTiles 0
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 0
#define kRenderThreadSafety eRenderFullySafe
#define kHostFrameThreading false

#define kParamEffect "background"
#define kParamEffectLabel "Background"
#define kParamEffectHint "Background type"
#define kParamEffectDefault 6

#define kParamSeed "seed"
#define kParamSeedLabel "Seed"
#define kParamSeedHint "Seed the random generator (-1 is random)"
#define kParamSeedDefault 4321

#define kParamWidth "width"
#define kParamWidthLabel "Width"
#define kParamWidthHint "Set canvas width, default (0) is project format"
#define kParamWidthDefault 0

#define kParamHeight "height"
#define kParamHeightLabel "Height"
#define kParamHeightHint "Set canvas height, default (0) is project format"
#define kParamHeightDefault 0

#define kParamFromColor "fromColor"
#define kParamFromColorLabel "Color from"
#define kParamFromColorHint "Set start color, you must set a end color for this to work. Valid values are: none (transparent), color name (red, blue etc) or hex colors"

#define kParamToColor "toColor"
#define kParamToColorLabel "Color to"
#define kParamToColorHint "Set end color, you must set a start color for this to work. Valid values are : none (transparent), color name (red, blue etc) or hex colors"

using namespace OFX;
static bool gHostIsNatron = false;

class TexturePlugin : public OFX::ImageEffect
{
public:
    TexturePlugin(OfxImageEffectHandle handle);
    virtual ~TexturePlugin();
    virtual void render(const OFX::RenderArguments &args) OVERRIDE FINAL;
    virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod) OVERRIDE FINAL;
private:
    OFX::Clip *dstClip_;
    OFX::ChoiceParam *effect_;
    OFX::IntParam *seed_;
    OFX::IntParam *width_;
    OFX::IntParam *height_;
    OFX::StringParam *fromColor_;
    OFX::StringParam *toColor_;
};

TexturePlugin::TexturePlugin(OfxImageEffectHandle handle)
: OFX::ImageEffect(handle)
, dstClip_(0)
, width_(0)
, height_(0)
{
    Magick::InitializeMagick(NULL);

    dstClip_ = fetchClip(kOfxImageEffectOutputClipName);
    assert(dstClip_ && (dstClip_->getPixelComponents() == OFX::ePixelComponentRGBA || dstClip_->getPixelComponents() == OFX::ePixelComponentRGB));

    effect_ = fetchChoiceParam(kParamEffect);
    seed_ = fetchIntParam(kParamSeed);
    width_ = fetchIntParam(kParamWidth);
    height_ = fetchIntParam(kParamHeight);
    fromColor_ = fetchStringParam(kParamFromColor);
    toColor_ = fetchStringParam(kParamToColor);

    assert(effect_ && seed_ && width_ && height_ && fromColor_ && toColor_);
}

TexturePlugin::~TexturePlugin()
{
}

/* Override the render */
void TexturePlugin::render(const OFX::RenderArguments &args)
{
    if (!kSupportsRenderScale && (args.renderScale.x != 1. || args.renderScale.y != 1.)) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
        return;
    }

    if (!dstClip_) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
        return;
    }
    assert(dstClip_);

    std::auto_ptr<OFX::Image> dstImg(dstClip_->fetchImage(args.time));
    if (!dstImg.get()) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
        return;
    }

    if (dstImg->getRenderScale().x != args.renderScale.x ||
        dstImg->getRenderScale().y != args.renderScale.y ||
        dstImg->getField() != args.fieldToRender) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
        OFX::throwSuiteStatusException(kOfxStatFailed);
        return;
    }

    OFX::BitDepthEnum dstBitDepth = dstImg->getPixelDepth();
    if (dstBitDepth != OFX::eBitDepthFloat && dstBitDepth != OFX::eBitDepthUShort && dstBitDepth != OFX::eBitDepthUByte) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
        return;
    }

    OFX::PixelComponentEnum dstComponents  = dstImg->getPixelComponents();
    if ((dstComponents != OFX::ePixelComponentRGBA && dstComponents != OFX::ePixelComponentRGB && dstComponents != OFX::ePixelComponentAlpha)) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
        return;
    }

    // are we in the image bounds
    OfxRectI dstBounds = dstImg->getBounds();
    OfxRectI dstRod = dstImg->getRegionOfDefinition();
    if(args.renderWindow.x1 < dstBounds.x1 || args.renderWindow.x1 >= dstBounds.x2 || args.renderWindow.y1 < dstBounds.y1 || args.renderWindow.y1 >= dstBounds.y2 ||
       args.renderWindow.x2 <= dstBounds.x1 || args.renderWindow.x2 > dstBounds.x2 || args.renderWindow.y2 <= dstBounds.y1 || args.renderWindow.y2 > dstBounds.y2) {
        OFX::throwSuiteStatusException(kOfxStatErrValue);
        return;
    }

    // Get params
    int effect,seed;
    std::string fromColor, toColor;
    effect_->getValueAtTime(args.time, effect);
    seed_->getValueAtTime(args.time, seed);
    fromColor_->getValueAtTime(args.time, fromColor);
    toColor_->getValueAtTime(args.time, toColor);

    // Generate empty image
    int width = dstRod.x2-dstRod.x1;
    int height = dstRod.y2-dstRod.y1;
    Magick::Image image(Magick::Geometry(width,height),Magick::Color("rgba(0,0,0,0)"));

    // Set seed
    Magick::SetRandomSeed(seed);

    // generate background
    switch (effect) {
    case 0: // Plasma
        if (fromColor.empty() && toColor.empty())
            image.read("plasma:");
        else
            image.read("plasma:"+fromColor+"-"+toColor);
        break;
    case 1: // Plasma Fractal
        image.read("plasma:fractal");
        break;
    case 2: // GaussianNoise
        image.addNoise(Magick::GaussianNoise);
        break;
    case 3: // ImpulseNoise
        image.addNoise(Magick::ImpulseNoise);
        break;
    case 4: // LaplacianNoise
        image.addNoise(Magick::LaplacianNoise);
        break;
    case 5: // checkerboard
        image.read("pattern:checkerboard");
        break;
    case 6: // stripes
        image.extent(Magick::Geometry(width,1));
        image.addNoise(Magick::GaussianNoise);
        image.channel(Magick::GreenChannel);
        image.negate();
        image.scale(Magick::Geometry(width,height));
        break;
    case 7: // gradient
        if (fromColor.empty() && toColor.empty())
            image.read("gradient:");
        else
            image.read("gradient:"+fromColor+"-"+toColor);
        break;
    case 8: // radial-gradient
        if (fromColor.empty() && toColor.empty())
            image.read("radial-gradient:");
        else
            image.read("radial-gradient:"+fromColor+"-"+toColor);
        break;
    }

    // return image
    if (dstClip_ && dstClip_->isConnected())
        image.write(0,0,width,height,"RGBA",Magick::FloatPixel,(float*)dstImg->getPixelData());
}

bool TexturePlugin::getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod)
{
    if (!kSupportsRenderScale && (args.renderScale.x != 1. || args.renderScale.y != 1.)) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
        return false;
    }

    int width,height;
    width_->getValue(width);
    height_->getValue(height);

    if (width>0 && height>0) {
        rod.x1 = rod.y1 = 0;
        rod.x2 = width;
        rod.y2 = height;
    }
    else {
        rod.x1 = rod.y1 = kOfxFlagInfiniteMin;
        rod.x2 = rod.y2 = kOfxFlagInfiniteMax;
    }

    return true;
}

mDeclarePluginFactory(TexturePluginFactory, {}, {});

/** @brief The basic describe function, passed a plugin descriptor */
void TexturePluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    std::string magickV = MagickCore::GetMagickVersion(NULL);
    desc.setPluginDescription("Texture/Background generator for Natron.\n\nWritten by Ole-André Rodlie <olear@fxarena.net>\n\nPowered by "+magickV);

    // add the supported contexts
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedContext(eContextGenerator);

    // add supported pixel depths
    desc.addSupportedBitDepth(eBitDepthFloat);

    desc.setSupportsTiles(kSupportsTiles);
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    desc.setRenderThreadSafety(kRenderThreadSafety);
    desc.setHostFrameThreading(kHostFrameThreading);
}

/** @brief The describe in context function, passed a plugin descriptor and a context */
void TexturePluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, ContextEnum /*context*/)
{   
    gHostIsNatron = (OFX::getImageEffectHostDescription()->hostName == kNatronOfxHostName);

    // there has to be an input clip, even for generators
    ClipDescriptor* srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setOptional(true);

    // create the mandated output clip
    ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->setSupportsTiles(kSupportsTiles);

    // make some pages
    PageParamDescriptor *page = desc.definePageParam(kPluginName);
    {
        ChoiceParamDescriptor *param = desc.defineChoiceParam(kParamEffect);
        param->setLabel(kParamEffectLabel);
        param->setHint(kParamEffectHint);
        if (gHostIsNatron) {
            param->setCascading(OFX::getImageEffectHostDescription()->supportsCascadingChoices);
            param->appendOption("Plasma/Regular");
            param->appendOption("Plasma/Fractal");
            param->appendOption("Noise/Gaussian");
            param->appendOption("Noise/Impulse");
            param->appendOption("Noise/Laplacian");
            param->appendOption("Misc/Checkerboard");
            param->appendOption("Misc/Stripes");
            param->appendOption("Gradient/Regular");
            param->appendOption("Gradient/Radial");
        }
        else {
            param->appendOption("Plasma");
            param->appendOption("Plasma Fractal");
            param->appendOption("GaussianNoise");
            param->appendOption("ImpulseNoise");
            param->appendOption("LaplacianNoise");
            param->appendOption("Checkerboard");
            param->appendOption("Stripes");
            param->appendOption("Gradient");
            param->appendOption("Gradient Radial");
        }
        param->setDefault(kParamEffectDefault);
        param->setAnimates(true);
        page->addChild(*param);
    }
    {
        IntParamDescriptor *param = desc.defineIntParam(kParamSeed);
        param->setLabel(kParamSeedLabel);
        param->setHint(kParamSeedHint);
        param->setRange(-1, 10000);
        param->setDisplayRange(-1, 5000);
        param->setDefault(kParamSeedDefault);
        page->addChild(*param);
    }
    {
        IntParamDescriptor* param = desc.defineIntParam(kParamWidth);
        param->setLabel(kParamWidthLabel);
        param->setHint(kParamWidthHint);
        param->setRange(0, 10000);
        param->setDisplayRange(0, 4000);
        param->setDefault(kParamWidthDefault);
        page->addChild(*param);
    }
    {
        IntParamDescriptor* param = desc.defineIntParam(kParamHeight);
        param->setLabel(kParamHeightLabel);
        param->setHint(kParamHeightHint);
        param->setRange(0, 10000);
        param->setDisplayRange(0, 4000);
        param->setDefault(kParamHeightDefault);
        page->addChild(*param);
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kParamFromColor);
        param->setLabel(kParamFromColorLabel);
        param->setHint(kParamFromColorHint);
        param->setStringType(eStringTypeSingleLine);
        param->setAnimates(true);
        page->addChild(*param);
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kParamToColor);
        param->setLabel(kParamToColorLabel);
        param->setHint(kParamToColorHint);
        param->setStringType(eStringTypeSingleLine);
        param->setAnimates(true);
        page->addChild(*param);
    }
}

/** @brief The create instance function, the plugin must return an object derived from the \ref OFX::ImageEffect class */
ImageEffect* TexturePluginFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    return new TexturePlugin(handle);
}

void getTexturePluginID(OFX::PluginFactoryArray &ids)
{
    static TexturePluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
    ids.push_back(&p);
}
