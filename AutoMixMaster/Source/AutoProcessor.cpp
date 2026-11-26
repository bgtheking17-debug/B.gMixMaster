#include "AutoProcessor.h"

using namespace juce;

namespace
{
    // skewCentre = 0 -> linear; otherwise the knob's middle position maps to this value
    std::unique_ptr<AudioParameterFloat> makeFloat (const String& id, const String& name, float lo, float hi,
                                                    float step, float def, const String& unit, float skewCentre = 0.0f)
    {
        NormalisableRange<float> range (lo, hi, step);
        if (skewCentre > lo && skewCentre < hi)
            range.setSkewForCentre (skewCentre);
        return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, range, def,
                                                      AudioParameterFloatAttributes().withLabel (unit));
    }

    float get (const AudioProcessorValueTreeState& s, const char* id)
    {
        return s.getRawParameterValue (id)->load();
    }
}

//==============================================================================
AudioProcessorValueTreeState::ParameterLayout AutoProcessor::createLayout()
{
    AudioProcessorValueTreeState::ParameterLayout l;

    // ---- main
    StringArray genreNames;
    for (int i = 0; i < bg::kNumGenres; ++i) genreNames.add (bg::genres[i].name);
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "genre", 1 }, "Genre", genreNames, 0));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode", StringArray { "Auto", "Manual" }, 0));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "learn", 1 }, "Learn (collect)", false));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "useLearned", 1 }, "Use Learned EQ", false));
    l.add (makeFloat ("amount", "Amount", 0.f, 100.f, 0.1f, 70.f, "%"));
    l.add (makeFloat ("loudOffset", "Loudness Offset", -6.f, 6.f, 0.1f, 0.f, "dB"));
    l.add (makeFloat ("ceiling", "Ceiling", -6.f, 0.f, 0.1f, -1.f, "dB"));
    l.add (makeFloat ("limRelease", "Limiter Release", 5.f, 1000.f, 1.f, 100.f, "ms", 100.f));
    l.add (makeFloat ("width", "Stereo Width", 0.f, 200.f, 1.f, 100.f, "%"));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "monoBass", 1 }, "Mono Bass", false));
    l.add (makeFloat ("monoFreq", "Mono Bass Freq", 40.f, 300.f, 1.f, 120.f, "Hz", 120.f));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "dither", 1 }, "Dither", StringArray { "Off", "16 bit", "24 bit" }, 0));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "bypassMatch", 1 }, "Loudness-matched Bypass", true));

    // ---- manual
    l.add (makeFloat ("lowcut", "Manual Low Cut", 20.f, 200.f, 1.f, 25.f, "Hz", 50.f));
    l.add (makeFloat ("eqQ", "Manual EQ Q", 0.2f, 5.f, 0.01f, 1.f, ""));
    static const char* bandNames[9] = { "62 Hz", "125 Hz", "250 Hz", "500 Hz", "1 kHz", "2 kHz", "4 kHz", "8 kHz", "16 kHz" };
    for (int b = 0; b < 9; ++b)
        l.add (makeFloat ("eq" + String (b), "Manual EQ " + String (bandNames[b]), -12.f, 12.f, 0.1f, 0.f, "dB"));
    l.add (makeFloat ("xover1", "Crossover Low/Mid", 50.f, 400.f, 1.f, 150.f, "Hz", 150.f));
    l.add (makeFloat ("xover2", "Crossover Mid/High", 1000.f, 8000.f, 10.f, 3000.f, "Hz", 3000.f));
    static const char* bn[3] = { "Low", "Mid", "High" };
    static const float defAtk[3] = { 30.f, 20.f, 10.f }, defRel[3] = { 200.f, 150.f, 100.f };
    for (int b = 0; b < 3; ++b)
    {
        const String n (bn[b]);
        l.add (makeFloat ("thr" + String (b),  "Manual " + n + " Threshold", -60.f, 0.f, 0.1f, -18.f, "dB"));
        l.add (makeFloat ("ratio" + String (b), "Manual " + n + " Ratio", 1.f, 20.f, 0.1f, 2.f, ":1", 4.f));
        l.add (makeFloat ("atk" + String (b),  "Manual " + n + " Attack", 0.1f, 200.f, 0.1f, defAtk[b], "ms", 20.f));
        l.add (makeFloat ("rel" + String (b),  "Manual " + n + " Release", 10.f, 1000.f, 1.f, defRel[b], "ms", 150.f));
        l.add (makeFloat ("make" + String (b), "Manual " + n + " Makeup", -12.f, 12.f, 0.1f, 0.f, "dB"));
        l.add (makeFloat ("wid" + String (b),  "Manual " + n + " Width", 0.f, 200.f, 1.f, 100.f, "%"));
    }
    l.add (makeFloat ("sat", "Manual Saturation", 0.f, 100.f, 0.1f, 0.f, "%"));
    l.add (makeFloat ("outGain", "Manual Output Gain", -12.f, 12.f, 0.1f, 0.f, "dB"));
    return l;
}

AutoProcessor::AutoProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", AudioChannelSet::stereo(), true)
                          .withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
}

bool AutoProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainInputChannelSet() == AudioChannelSet::stereo()
        && l.getMainOutputChannelSet() == AudioChannelSet::stereo();
}

void AutoProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    engine.prepare (sampleRate);
    setLatencySamples (engine.latency());
}

bg::EngineParams AutoProcessor::readParams() const
{
    bg::EngineParams p;
    p.genre         = (int) std::lround (get (apvts, "genre"));
    p.manual        = get (apvts, "mode") > 0.5f;
    p.learn         = get (apvts, "learn") > 0.5f;
    p.analysisFrozen = get (apvts, "useLearned") > 0.5f;
    p.amount        = get (apvts, "amount") / 100.0;
    p.loudOffsetDb  = get (apvts, "loudOffset");
    p.ceilingDb     = get (apvts, "ceiling");
    p.limReleaseMs  = get (apvts, "limRelease");
    p.widthScale    = get (apvts, "width") / 100.0;
    p.monoBass      = get (apvts, "monoBass") > 0.5f;
    p.monoFreq      = get (apvts, "monoFreq");
    p.dither        = (int) std::lround (get (apvts, "dither"));
    p.bypass        = get (apvts, "bypass") > 0.5f;
    p.bypassMatch   = get (apvts, "bypassMatch") > 0.5f;

    p.lowcut = get (apvts, "lowcut");
    p.eqQ    = get (apvts, "eqQ");
    for (int b = 0; b < 9; ++b)
        p.eqGain[b] = get (apvts, ("eq" + String (b)).toRawUTF8());
    p.xover1 = get (apvts, "xover1");
    p.xover2 = get (apvts, "xover2");
    for (int b = 0; b < 3; ++b)
    {
        const String s (b);
        p.bThr[b]   = get (apvts, ("thr" + s).toRawUTF8());
        p.bRatio[b] = get (apvts, ("ratio" + s).toRawUTF8());
        p.bAtk[b]   = get (apvts, ("atk" + s).toRawUTF8());
        p.bRel[b]   = get (apvts, ("rel" + s).toRawUTF8());
        p.bMake[b]  = get (apvts, ("make" + s).toRawUTF8());
        p.bWidth[b] = get (apvts, ("wid" + s).toRawUTF8());
    }
    p.sat     = get (apvts, "sat") / 100.0;
    p.outGainDb = get (apvts, "outGain");
    return p;
}

void AutoProcessor::processBlock (AudioBuffer<float>& buf, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = buf.getNumSamples();
    if (n <= 0 || buf.getNumChannels() < 2) return;

    const auto params = readParams();
    engine.process (buf.getWritePointer (0), buf.getWritePointer (1), n, params);
}

void AutoProcessor::setParam (const String& id, float realValue)
{
    if (auto* p = apvts.getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (realValue));
}

void AutoProcessor::copyAutoToManual()
{
    const auto a = engine.autoValues();
    for (int b = 0; b < 9; ++b) setParam ("eq" + String (b), (float) a.eq[b]);
    for (int b = 0; b < 3; ++b)
    {
        const String s (b);
        setParam ("thr" + s,   (float) a.thr[b]);
        setParam ("ratio" + s, (float) a.ratio[b]);
        setParam ("atk" + s,   (float) a.atk[b]);
        setParam ("rel" + s,   (float) a.rel[b]);
        setParam ("wid" + s,   (float) a.width[b]);
    }
    setParam ("sat", (float) (a.sat * 100.0));
    setParam ("outGain", (float) a.gainDb);
}

//==============================================================================
AudioProcessorEditor* AutoProcessor::createEditor()
{
    return new GenericAudioProcessorEditor (*this);
}

void AutoProcessor::getStateInformation (MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, dest);
}

void AutoProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (ValueTree::fromXml (*xml));
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AutoProcessor(); }
