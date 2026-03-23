#include "sierrachart.h"
#include <float.h>

SCDLLName("TwoBarPattern_Arrows_Versatile_1to5Bars")

// Direction mode: 0 = Either, 1 = Bullish, 2 = Bearish
static bool PassDirection(int mode, float o, float c)
{
    if (mode == 0) return true;
    if (mode == 1) return c > o;
    if (mode == 2) return c < o;
    return true;
}

static float AbsNoCmath(float x) { return (x >= 0.0f) ? x : -x; }
static float Min2(float a, float b) { return (a < b) ? a : b; }
static float Max2(float a, float b) { return (a > b) ? a : b; }

struct CandlePercents
{
    float BodyPct;
    float UpperWickPct;
    float LowerWickPct;
};

static bool ComputePercents(float o, float h, float l, float c, CandlePercents& out)
{
    const float range = h - l;
    if (range <= 0.0f)
        return false;

    const float bodyAbs = AbsNoCmath(c - o);
    const float upper   = h - ((c > o) ? c : o);
    const float lower   = ((c < o) ? c : o) - l;

    out.BodyPct      = (bodyAbs / range) * 100.0f;
    out.UpperWickPct = (upper   / range) * 100.0f;
    out.LowerWickPct = (lower   / range) * 100.0f;
    return true;
}

static bool PassMinMax(float value, float minV, float maxV, int useMax)
{
    if (value < minV) return false;
    if (useMax && value > maxV) return false;
    return true;
}

// current bar range as % of prior bar range
static bool PassRelativeCandleSize(
    int enabled,
    float hPrior, float lPrior,
    float hCurrent, float lCurrent,
    float minPct,
    float maxPct)
{
    if (!enabled) return true;

    const float priorRange = hPrior - lPrior;
    if (priorRange <= 0.0f)
        return false;

    const float currentRange = hCurrent - lCurrent;
    const float relativePct = (currentRange / priorRange) * 100.0f;

    if (relativePct < minPct) return false;
    if (relativePct > maxPct) return false;

    return true;
}

// 0=None, 1=Inside, 2=Outside, 3=UpperBreakOnly, 4=LowerBreakOnly
static bool PassRangeRelationship(
    int mode,
    float hParent, float lParent,
    float hChild,  float lChild,
    float tickSize)
{
    if (mode == 0) return true;

    const float eps = tickSize * 0.25f;

    const bool upperBroken = hChild > (hParent + eps);
    const bool lowerBroken = lChild < (lParent - eps);

    const bool upperInside = hChild <= (hParent + eps);
    const bool lowerInside = lChild >= (lParent - eps);

    if (mode == 1) return upperInside && lowerInside; // Inside
    if (mode == 2) return upperBroken && lowerBroken; // Outside
    if (mode == 3) return upperBroken && lowerInside; // UpperBreakOnly
    if (mode == 4) return lowerBroken && upperInside; // LowerBreakOnly

    return true;
}

// 0=None, 1=CannotBreakLower, 2=CannotBreakUpper
static bool PassNoBreakConstraint(
    int mode,
    float hParent, float lParent,
    float hChild,  float lChild,
    float tickSize)
{
    if (mode == 0) return true;

    const float eps = tickSize * 0.25f;

    if (mode == 1)
        return lChild >= (lParent - eps);

    if (mode == 2)
        return hChild <= (hParent + eps);

    return true;
}

// If enabled, require (High-Low) >= (ATR * multiple)
// If ATR not ready yet, allow.
static bool PassRangeToATR(
    int enabled,
    float barHigh,
    float barLow,
    float atrValue,
    float multiple)
{
    if (!enabled) return true;
    if (atrValue <= 0.0f) return true;

    const float barRange = barHigh - barLow;
    return barRange >= (multiple * atrValue);
}

// If parent bullish: retrace zone is parentClose -> parentLow, measure childLow.
// If parent bearish: retrace zone is parentClose -> parentHigh, measure childHigh.
static bool PassRetraceGate(
    int enabled,
    float oParent, float hParent, float lParent, float cParent,
    float hChild,  float lChild,
    float minPct,
    float maxPct,
    float tickSize)
{
    if (!enabled) return true;

    if (cParent == oParent) return true; // doji parent: fail-open

    const float epsPx = tickSize * 0.25f;

    if (cParent > oParent)
    {
        const float denom = cParent - lParent;
        if (denom <= epsPx) return true;

        const float retracePct = ((cParent - lChild) / denom) * 100.0f;

        if (retracePct + 1e-6f < minPct) return false;
        if (retracePct - 1e-6f > maxPct) return false;
        return true;
    }

    const float denom = hParent - cParent;
    if (denom <= epsPx) return true;

    const float retracePct = ((hChild - cParent) / denom) * 100.0f;

    if (retracePct + 1e-6f < minPct) return false;
    if (retracePct - 1e-6f > maxPct) return false;
    return true;
}

// mode: 0=None, 1=HighestHigh, 2=LowestLow
// whichBarForExtreme: 0=Any bar in pattern, 1=A,2=B,3=C,4=D,5=E
static bool PassRelativeExtreme(
    SCStudyInterfaceRef sc,
    int idxSignal,
    int mode,
    int windowMinutes,
    int useBarsLookback,
    int windowBars,
    int patternBars,
    int whichBarForExtreme)
{
    if (mode == 0) return true;
    if (patternBars < 1 || patternBars > 5) return true;
    if (idxSignal < (patternBars - 1)) return true;

    int barsBack = 0;

    if (useBarsLookback != 0)
    {
        if (windowBars <= 0) return true;
        barsBack = windowBars;
    }
    else
    {
        if (windowMinutes <= 0) return true;

        barsBack = windowMinutes;

        const int secondsPerBar = (int)sc.SecondsPerBar;
        if (secondsPerBar > 0)
        {
            const int windowSeconds = windowMinutes * 60;
            barsBack = windowSeconds / secondsPerBar;
            if (barsBack < 1) barsBack = 1;
        }
    }

    if (barsBack < 1)
        barsBack = 1;

    int start = idxSignal - barsBack + 1;
    if (start < 0) start = 0;

    float hh = sc.High[start];
    float ll = sc.Low[start];

    for (int j = start + 1; j <= idxSignal; ++j)
    {
        if (sc.High[j] > hh) hh = sc.High[j];
        if (sc.Low[j]  < ll) ll = sc.Low[j];
    }

    const float eps = sc.TickSize * 0.25f;

    const int idxA = idxSignal - (patternBars - 1);
    int idx[5] = { idxA, idxA + 1, idxA + 2, idxA + 3, idxA + 4 };

    float candidateHigh = sc.High[idxSignal];
    float candidateLow  = sc.Low[idxSignal];

    if (whichBarForExtreme == 0)
    {
        candidateHigh = sc.High[idx[0]];
        candidateLow  = sc.Low[idx[0]];
        for (int k = 1; k < patternBars; ++k)
        {
            if (sc.High[idx[k]] > candidateHigh) candidateHigh = sc.High[idx[k]];
            if (sc.Low[idx[k]]  < candidateLow)  candidateLow  = sc.Low[idx[k]];
        }
    }
    else
    {
        const int sel = whichBarForExtreme - 1;
        if (sel >= 0 && sel < patternBars)
        {
            candidateHigh = sc.High[idx[sel]];
            candidateLow  = sc.Low[idx[sel]];
        }
    }

    if (mode == 1) return candidateHigh >= (hh - eps);
    if (mode == 2) return candidateLow  <= (ll + eps);
    return true;
}

// Relative volume gate on signal bar
static bool PassRelativeVolume(
    SCStudyInterfaceRef sc,
    int enabled,
    int idxSignal,
    int lookbackBars,
    float multiple)
{
    if (!enabled) return true;
    if (lookbackBars <= 0) return true;
    if (multiple <= 0.0f) return true;

    const int start = idxSignal - lookbackBars;
    if (start < 0) return true; // fail-open when insufficient history

    double sum = 0.0;
    for (int j = start; j <= idxSignal - 1; ++j)
        sum += (double)sc.Volume[j];

    const double avg = sum / (double)lookbackBars;
    if (avg <= 0.0) return true;

    const double vSig = (double)sc.Volume[idxSignal];
    return vSig >= (avg * (double)multiple);
}

// 0=OFF, 1=YES must interact, 2=NO must NOT interact
static bool PassLevelInteraction(
    int mode,
    float barHigh,
    float barLow,
    float level,
    float tickSize)
{
    if (mode == 0)
        return true;

    const float eps = tickSize * 0.5f;
    const bool touched = (barLow <= (level + eps) && barHigh >= (level - eps));

    if (mode == 1) return touched;
    if (mode == 2) return !touched;

    return true;
}

// Find a study level at or very near the bar.
// For TPO POC use a short maxLookbackBars (like 1 or 2).
// For PSC use a large maxLookbackBars since it is session-constant.
static bool GetStudyLevelNearBar(
    const SCFloatArrayRef levelArray,
    int idxBar,
    int maxLookbackBars,
    float& outLevel)
{
    if (idxBar < 0)
        return false;

    const int maxIdx = levelArray.GetArraySize() - 1;
    if (maxIdx < 0)
        return false;

    if (idxBar > maxIdx)
        idxBar = maxIdx;

    if (maxLookbackBars < 0)
        maxLookbackBars = 0;

    const int start = idxBar - maxLookbackBars;
    const int first = (start < 0) ? 0 : start;

    for (int j = idxBar; j >= first; --j)
    {
        const float v = levelArray[j];
        if (v != 0.0f)
        {
            outLevel = v;
            return true;
        }
    }

    return false;
}

static bool PassStudyLevelGate(
    SCStudyInterfaceRef sc,
    int mode,
    int idxBar,
    const SCFloatArrayRef levelArray,
    int maxLookbackBars)
{
    if (mode == 0)
        return true;

    float level = 0.0f;
    if (!GetStudyLevelNearBar(levelArray, idxBar, maxLookbackBars, level))
        return false;

    return PassLevelInteraction(mode, sc.High[idxBar], sc.Low[idxBar], level, sc.TickSize);
}

// ---------------- Large Volume Location helpers ----------------
// These are copied directly from the working standalone study logic.

static bool PriceInTopPercent(double price, float high, float low, float pct)
{
    if (pct <= 0.0f)
        return false;

    const double range = (double)high - (double)low;
    if (range <= 0.0)
        return false;

    const double cutoff = (double)high - range * ((double)pct / 100.0);
    return price >= cutoff;
}

static bool PriceInBottomPercent(double price, float high, float low, float pct)
{
    if (pct <= 0.0f)
        return false;

    const double range = (double)high - (double)low;
    if (range <= 0.0)
        return false;

    const double cutoff = (double)low + range * ((double)pct / 100.0);
    return price <= cutoff;
}

// sideFilter: 0 either, 1 buy, 2 sell
static bool PassLargeVolumeLocation(
    SCStudyInterfaceRef sc,
    int bar,
    int sideFilter,
    unsigned int volThreshold,
    float topPct,
    float botPct,
    int useEitherZone)
{
    if (bar < 0 || bar >= sc.ArraySize)
        return false;

    if (sc.p_VolumeLevelAtPriceForBars == nullptr)
        return false;

    const int sizeAtBar = sc.p_VolumeLevelAtPriceForBars->GetSizeAtBarIndex(bar);
    if (sizeAtBar <= 0)
        return false;

    float highestQualifyingPrice = -FLT_MAX;
    float lowestQualifyingPrice  = FLT_MAX;
    bool foundQualifying = false;

    for (int vapIndex = 0; vapIndex < sizeAtBar; ++vapIndex)
    {
        const s_VolumeLevelAtPrice* pLevel = nullptr;
        if (!sc.p_VolumeLevelAtPriceForBars->GetVAPElementAtIndex(bar, vapIndex, &pLevel) || pLevel == nullptr)
            break;

        // Same price-level data used by Large Volume Trade Indicator
        const unsigned int maxVol  = pLevel->MaxVolume;
        const unsigned int bidVol  = pLevel->BidTradeVolume;
        const unsigned int askVol  = pLevel->AskTradeVolume;
        const float price = (float)sc.TicksToPriceValue(pLevel->PriceInTicks);

        // Require threshold at this price level
        if (maxVol < volThreshold)
            continue;

        bool sidePass = false;

        // For "Buy" and "Sell", use dominant side at that price level.
        // This is the closest practical match to the built-in red/green behavior.
        if (sideFilter == 0)
        {
            sidePass = true;
        }
        else if (sideFilter == 1) // Buy
        {
            sidePass = (askVol > bidVol && askVol > 0);
        }
        else if (sideFilter == 2) // Sell
        {
            sidePass = (bidVol > askVol && bidVol > 0);
        }

        if (!sidePass)
            continue;

        foundQualifying = true;

        if (price > highestQualifyingPrice)
            highestQualifyingPrice = price;

        if (price < lowestQualifyingPrice)
            lowestQualifyingPrice = price;
    }

    if (!foundQualifying)
        return false;

    const float high = sc.High[bar];
    const float low  = sc.Low[bar];

    bool topPass = false;
    bool botPass = false;

    if (topPct > 0.0f)
        topPass = PriceInTopPercent((double)highestQualifyingPrice, high, low, topPct);

    if (botPct > 0.0f)
        botPass = PriceInBottomPercent((double)lowestQualifyingPrice, high, low, botPct);

    bool finalPass = false;

    if (topPct > 0.0f && botPct > 0.0f)
        finalPass = (useEitherZone != 0) ? (topPass || botPass) : (topPass && botPass);
    else
        finalPass = (topPass || botPass);

    return finalPass;
}

SCSFExport scsf_TwoBarPattern_Arrows_Versatile_1to5Bars(SCStudyInterfaceRef sc)
{
    SCSubgraphRef Signal   = sc.Subgraph[0];
    SCSubgraphRef ATR      = sc.Subgraph[1];
    SCSubgraphRef DotAbove = sc.Subgraph[2];
    SCSubgraphRef DotBelow = sc.Subgraph[3];

    // Inputs (ordered for settings layout)

    SCInputRef In_PatternBars = sc.Input[0];
    SCInputRef In_ATRLength   = sc.Input[1];

    // Shared source studies
    SCInputRef In_TPOPOCSource = sc.Input[2];
    SCInputRef In_PSCSource    = sc.Input[3];

    // ---------------- Bar A ----------------
    SCInputRef In_A_Direction   = sc.Input[4];
    SCInputRef In_A_BodyMinPct  = sc.Input[5];
    SCInputRef In_A_BodyUseMax  = sc.Input[6];
    SCInputRef In_A_BodyMaxPct  = sc.Input[7];

    SCInputRef In_A_UpperMinPct = sc.Input[8];
    SCInputRef In_A_UpperUseMax = sc.Input[9];
    SCInputRef In_A_UpperMaxPct = sc.Input[10];

    SCInputRef In_A_LowerMinPct = sc.Input[11];
    SCInputRef In_A_LowerUseMax = sc.Input[12];
    SCInputRef In_A_LowerMaxPct = sc.Input[13];

    SCInputRef In_A_TuneToATR   = sc.Input[14];
    SCInputRef In_A_ATRMultiple = sc.Input[15];

    SCInputRef In_A_TPOPOCGate  = sc.Input[16];
    SCInputRef In_A_PSCGate     = sc.Input[17];

    // ---------------- Bar B (vs A) ----------------
    SCInputRef In_B_Direction   = sc.Input[18];
    SCInputRef In_B_BodyMinPct  = sc.Input[19];
    SCInputRef In_B_BodyUseMax  = sc.Input[20];
    SCInputRef In_B_BodyMaxPct  = sc.Input[21];

    SCInputRef In_B_UpperMinPct = sc.Input[22];
    SCInputRef In_B_UpperUseMax = sc.Input[23];
    SCInputRef In_B_UpperMaxPct = sc.Input[24];

    SCInputRef In_B_LowerMinPct = sc.Input[25];
    SCInputRef In_B_LowerUseMax = sc.Input[26];
    SCInputRef In_B_LowerMaxPct = sc.Input[27];

    SCInputRef In_B_vs_A_RangeRelationMode = sc.Input[28];
    SCInputRef In_B_vs_A_NoBreakMode       = sc.Input[29];

    SCInputRef In_B_RetraceEnable = sc.Input[30];
    SCInputRef In_B_RetraceMinPct = sc.Input[31];
    SCInputRef In_B_RetraceMaxPct = sc.Input[32];

    SCInputRef In_B_TuneToATR     = sc.Input[33];
    SCInputRef In_B_ATRMultiple   = sc.Input[34];

    SCInputRef In_B_RelSizeEnable = sc.Input[35];
    SCInputRef In_B_RelSizeMinPct = sc.Input[36];
    SCInputRef In_B_RelSizeMaxPct = sc.Input[37];

    SCInputRef In_B_TPOPOCGate    = sc.Input[38];
    SCInputRef In_B_PSCGate       = sc.Input[39];

    // ---------------- Bar C (vs B) ----------------
    SCInputRef In_C_Direction   = sc.Input[40];
    SCInputRef In_C_BodyMinPct  = sc.Input[41];
    SCInputRef In_C_BodyUseMax  = sc.Input[42];
    SCInputRef In_C_BodyMaxPct  = sc.Input[43];

    SCInputRef In_C_UpperMinPct = sc.Input[44];
    SCInputRef In_C_UpperUseMax = sc.Input[45];
    SCInputRef In_C_UpperMaxPct = sc.Input[46];

    SCInputRef In_C_LowerMinPct = sc.Input[47];
    SCInputRef In_C_LowerUseMax = sc.Input[48];
    SCInputRef In_C_LowerMaxPct = sc.Input[49];

    SCInputRef In_C_vs_B_RangeRelationMode = sc.Input[50];
    SCInputRef In_C_vs_B_NoBreakMode       = sc.Input[51];

    SCInputRef In_C_RetraceEnable = sc.Input[52];
    SCInputRef In_C_RetraceMinPct = sc.Input[53];
    SCInputRef In_C_RetraceMaxPct = sc.Input[54];

    SCInputRef In_C_TuneToATR     = sc.Input[55];
    SCInputRef In_C_ATRMultiple   = sc.Input[56];

    SCInputRef In_C_RelSizeEnable = sc.Input[57];
    SCInputRef In_C_RelSizeMinPct = sc.Input[58];
    SCInputRef In_C_RelSizeMaxPct = sc.Input[59];

    SCInputRef In_C_TPOPOCGate    = sc.Input[60];
    SCInputRef In_C_PSCGate       = sc.Input[61];

    // ---------------- Bar D (vs C) ----------------
    SCInputRef In_D_Direction   = sc.Input[62];
    SCInputRef In_D_BodyMinPct  = sc.Input[63];
    SCInputRef In_D_BodyUseMax  = sc.Input[64];
    SCInputRef In_D_BodyMaxPct  = sc.Input[65];

    SCInputRef In_D_UpperMinPct = sc.Input[66];
    SCInputRef In_D_UpperUseMax = sc.Input[67];
    SCInputRef In_D_UpperMaxPct = sc.Input[68];

    SCInputRef In_D_LowerMinPct = sc.Input[69];
    SCInputRef In_D_LowerUseMax = sc.Input[70];
    SCInputRef In_D_LowerMaxPct = sc.Input[71];

    SCInputRef In_D_vs_C_RangeRelationMode = sc.Input[72];
    SCInputRef In_D_vs_C_NoBreakMode       = sc.Input[73];

    SCInputRef In_D_RetraceEnable = sc.Input[74];
    SCInputRef In_D_RetraceMinPct = sc.Input[75];
    SCInputRef In_D_RetraceMaxPct = sc.Input[76];

    SCInputRef In_D_TuneToATR     = sc.Input[77];
    SCInputRef In_D_ATRMultiple   = sc.Input[78];

    SCInputRef In_D_RelSizeEnable = sc.Input[79];
    SCInputRef In_D_RelSizeMinPct = sc.Input[80];
    SCInputRef In_D_RelSizeMaxPct = sc.Input[81];

    SCInputRef In_D_TPOPOCGate    = sc.Input[82];
    SCInputRef In_D_PSCGate       = sc.Input[83];

    // ---------------- Bar E (vs D) ----------------
    SCInputRef In_E_Direction   = sc.Input[84];
    SCInputRef In_E_BodyMinPct  = sc.Input[85];
    SCInputRef In_E_BodyUseMax  = sc.Input[86];
    SCInputRef In_E_BodyMaxPct  = sc.Input[87];

    SCInputRef In_E_UpperMinPct = sc.Input[88];
    SCInputRef In_E_UpperUseMax = sc.Input[89];
    SCInputRef In_E_UpperMaxPct = sc.Input[90];

    SCInputRef In_E_LowerMinPct = sc.Input[91];
    SCInputRef In_E_LowerUseMax = sc.Input[92];
    SCInputRef In_E_LowerMaxPct = sc.Input[93];

    SCInputRef In_E_vs_D_RangeRelationMode = sc.Input[94];
    SCInputRef In_E_vs_D_NoBreakMode       = sc.Input[95];

    SCInputRef In_E_RetraceEnable = sc.Input[96];
    SCInputRef In_E_RetraceMinPct = sc.Input[97];
    SCInputRef In_E_RetraceMaxPct = sc.Input[98];

    SCInputRef In_E_TuneToATR     = sc.Input[99];
    SCInputRef In_E_ATRMultiple   = sc.Input[100];

    SCInputRef In_E_RelSizeEnable = sc.Input[101];
    SCInputRef In_E_RelSizeMinPct = sc.Input[102];
    SCInputRef In_E_RelSizeMaxPct = sc.Input[103];

    SCInputRef In_E_TPOPOCGate    = sc.Input[104];
    SCInputRef In_E_PSCGate       = sc.Input[105];

    // Marker settings
    SCInputRef In_DrawMode         = sc.Input[106];
    SCInputRef In_ArrowOffsetTicks = sc.Input[107];

    // Relative volume
    SCInputRef In_UseRelVolume     = sc.Input[108];
    SCInputRef In_VolLookbackBars  = sc.Input[109];
    SCInputRef In_VolMultiple      = sc.Input[110];

    // Relative extreme + No Relative Extreme (shared window/basis)
    SCInputRef In_ExtremeMode          = sc.Input[111];
    SCInputRef In_NoExtremeMode        = sc.Input[112];
    SCInputRef In_ExtremeWindowMin     = sc.Input[113];
    SCInputRef In_ExtremeUseBars       = sc.Input[114];
    SCInputRef In_ExtremeWindowBars    = sc.Input[115];
    SCInputRef In_ExtremeWhichBar      = sc.Input[116];

    // Large Volume Location gate
    SCInputRef In_UseLargeVolumeLocation = sc.Input[117];
    SCInputRef In_LV_WhichCandle         = sc.Input[118];
    SCInputRef In_LV_SideFilter          = sc.Input[119];
    SCInputRef In_LV_VolumeThreshold     = sc.Input[120];
    SCInputRef In_LV_TopPercent          = sc.Input[121];
    SCInputRef In_LV_BottomPercent       = sc.Input[122];
    SCInputRef In_LV_UseEitherZone       = sc.Input[123];

    if (sc.SetDefaults)
    {
        sc.GraphName   = "Bar Pattern Arrows (Versatile)";
        sc.AutoLoop    = 1;
        sc.UpdateAlways = 1;
        sc.GraphRegion = 0;
        sc.ValueFormat = VALUEFORMAT_INHERITED;

        // Required for price-level volume data used by the embedded Large Volume logic
        sc.MaintainVolumeAtPriceData = 1;

        Signal.Name         = "Pattern Marker (Arrow)";
        Signal.DrawStyle    = DRAWSTYLE_ARROW_UP;
        Signal.PrimaryColor = RGB(0, 255, 0);
        Signal.LineWidth    = 3;
        Signal.DrawZeros    = 0;

        ATR.Name      = "ATR (hidden)";
        ATR.DrawStyle = DRAWSTYLE_IGNORE;
        ATR.DrawZeros = 0;

        DotAbove.Name         = "Pattern Marker (Dot Above)";
        DotAbove.DrawStyle    = DRAWSTYLE_POINT;
        DotAbove.PrimaryColor = RGB(160, 160, 160);
        DotAbove.LineWidth    = 6;
        DotAbove.DrawZeros    = 0;

        DotBelow.Name         = "Pattern Marker (Dot Below)";
        DotBelow.DrawStyle    = DRAWSTYLE_POINT;
        DotBelow.PrimaryColor = RGB(160, 160, 160);
        DotBelow.LineWidth    = 6;
        DotBelow.DrawZeros    = 0;

        In_PatternBars.Name = "Pattern Bars (1 to 5)";
        In_PatternBars.SetInt(2);

        In_ATRLength.Name = "ATR Length (for Range-to-ATR gates)";
        In_ATRLength.SetInt(14);

        In_TPOPOCSource.Name = "TPO POC Source Study/Subgraph";
        In_TPOPOCSource.SetStudySubgraphValues(1, 0);

        In_PSCSource.Name = "Prior Session Close Source Study/Subgraph";
        In_PSCSource.SetStudySubgraphValues(1, 0);

        // ---------------- Bar A ----------------
        In_A_Direction.Name  = "Bar A Direction (0=Either,1=Bull,2=Bear)";
        In_A_Direction.SetInt(0);

        In_A_BodyMinPct.Name = "Bar A Body% Min";
        In_A_BodyMinPct.SetFloat(0.0f);
        In_A_BodyUseMax.Name = "Bar A Body% Use Max? (0/1)";
        In_A_BodyUseMax.SetInt(0);
        In_A_BodyMaxPct.Name = "Bar A Body% Max";
        In_A_BodyMaxPct.SetFloat(0.0f);

        In_A_UpperMinPct.Name = "Bar A UpperWick% Min";
        In_A_UpperMinPct.SetFloat(0.0f);
        In_A_UpperUseMax.Name = "Bar A UpperWick% Use Max? (0/1)";
        In_A_UpperUseMax.SetInt(0);
        In_A_UpperMaxPct.Name = "Bar A UpperWick% Max";
        In_A_UpperMaxPct.SetFloat(0.0f);

        In_A_LowerMinPct.Name = "Bar A LowerWick% Min";
        In_A_LowerMinPct.SetFloat(0.0f);
        In_A_LowerUseMax.Name = "Bar A LowerWick% Use Max? (0/1)";
        In_A_LowerUseMax.SetInt(0);
        In_A_LowerMaxPct.Name = "Bar A LowerWick% Max";
        In_A_LowerMaxPct.SetFloat(0.0f);

        In_A_TuneToATR.Name   = "Bar A Tune to ATR? (0/1)";
        In_A_TuneToATR.SetInt(0);
        In_A_ATRMultiple.Name = "Bar A ATR Multiple (Range >= ATR * X)";
        In_A_ATRMultiple.SetFloat(0.0f);

        In_A_TPOPOCGate.Name = "Bar A Crosses TPO POC? (0=OFF,1=YES,2=NO)";
        In_A_TPOPOCGate.SetInt(0);
        In_A_PSCGate.Name = "Bar A Crosses Prior Session Close? (0=OFF,1=YES,2=NO)";
        In_A_PSCGate.SetInt(0);

        // ---------------- Bar B ----------------
        In_B_Direction.Name = "Bar B Direction (0=Either,1=Bull,2=Bear) [PatternBars>=2]";
        In_B_Direction.SetInt(0);

        In_B_BodyMinPct.Name = "Bar B Body% Min [PatternBars>=2]";
        In_B_BodyMinPct.SetFloat(0.0f);
        In_B_BodyUseMax.Name = "Bar B Body% Use Max? (0/1) [PatternBars>=2]";
        In_B_BodyUseMax.SetInt(0);
        In_B_BodyMaxPct.Name = "Bar B Body% Max [PatternBars>=2]";
        In_B_BodyMaxPct.SetFloat(0.0f);

        In_B_UpperMinPct.Name = "Bar B UpperWick% Min [PatternBars>=2]";
        In_B_UpperMinPct.SetFloat(0.0f);
        In_B_UpperUseMax.Name = "Bar B UpperWick% Use Max? (0/1) [PatternBars>=2]";
        In_B_UpperUseMax.SetInt(0);
        In_B_UpperMaxPct.Name = "Bar B UpperWick% Max [PatternBars>=2]";
        In_B_UpperMaxPct.SetFloat(0.0f);

        In_B_LowerMinPct.Name = "Bar B LowerWick% Min [PatternBars>=2]";
        In_B_LowerMinPct.SetFloat(0.0f);
        In_B_LowerUseMax.Name = "Bar B LowerWick% Use Max? (0/1) [PatternBars>=2]";
        In_B_LowerUseMax.SetInt(0);
        In_B_LowerMaxPct.Name = "Bar B LowerWick% Max [PatternBars>=2]";
        In_B_LowerMaxPct.SetFloat(0.0f);

        In_B_vs_A_RangeRelationMode.Name =
            "Bar B vs Bar A Range (0=None,1=Inside,2=Outside,3=UpperBreakOnly,4=LowerBreakOnly) [PatternBars>=2]";
        In_B_vs_A_RangeRelationMode.SetInt(0);

        In_B_vs_A_NoBreakMode.Name =
            "Bar B vs Bar A No-Break Constraint (0=None,1=CannotBreakLower,2=CannotBreakUpper) [PatternBars>=2]";
        In_B_vs_A_NoBreakMode.SetInt(0);

        In_B_RetraceEnable.Name = "Bar B Retrace Gate Enable? (0/1) [vs Bar A]";
        In_B_RetraceEnable.SetInt(0);
        In_B_RetraceMinPct.Name = "Bar B Min Retrace % [vs Bar A]";
        In_B_RetraceMinPct.SetFloat(0.0f);
        In_B_RetraceMaxPct.Name = "Bar B Max Retrace % [vs Bar A]";
        In_B_RetraceMaxPct.SetFloat(0.0f);

        In_B_TuneToATR.Name   = "Bar B Tune to ATR? (0/1) [PatternBars>=2]";
        In_B_TuneToATR.SetInt(0);
        In_B_ATRMultiple.Name = "Bar B ATR Multiple (Range >= ATR * X) [PatternBars>=2]";
        In_B_ATRMultiple.SetFloat(0.0f);

        In_B_RelSizeEnable.Name = "Bar B Relative Candle Size Enable? (0/1) [vs Bar A]";
        In_B_RelSizeEnable.SetInt(0);
        In_B_RelSizeMinPct.Name = "Bar B Relative Candle Size Min % [vs Bar A]";
        In_B_RelSizeMinPct.SetFloat(0.0f);
        In_B_RelSizeMaxPct.Name = "Bar B Relative Candle Size Max % [vs Bar A]";
        In_B_RelSizeMaxPct.SetFloat(300.0f);

        In_B_TPOPOCGate.Name = "Bar B Crosses TPO POC? (0=OFF,1=YES,2=NO) [PatternBars>=2]";
        In_B_TPOPOCGate.SetInt(0);
        In_B_PSCGate.Name = "Bar B Crosses Prior Session Close? (0=OFF,1=YES,2=NO) [PatternBars>=2]";
        In_B_PSCGate.SetInt(0);

        // ---------------- Bar C ----------------
        In_C_Direction.Name = "Bar C Direction (0=Either,1=Bull,2=Bear) [PatternBars>=3]";
        In_C_Direction.SetInt(0);

        In_C_BodyMinPct.Name = "Bar C Body% Min [PatternBars>=3]";
        In_C_BodyMinPct.SetFloat(0.0f);
        In_C_BodyUseMax.Name = "Bar C Body% Use Max? (0/1) [PatternBars>=3]";
        In_C_BodyUseMax.SetInt(0);
        In_C_BodyMaxPct.Name = "Bar C Body% Max [PatternBars>=3]";
        In_C_BodyMaxPct.SetFloat(0.0f);

        In_C_UpperMinPct.Name = "Bar C UpperWick% Min [PatternBars>=3]";
        In_C_UpperMinPct.SetFloat(0.0f);
        In_C_UpperUseMax.Name = "Bar C UpperWick% Use Max? (0/1) [PatternBars>=3]";
        In_C_UpperUseMax.SetInt(0);
        In_C_UpperMaxPct.Name = "Bar C UpperWick% Max [PatternBars>=3]";
        In_C_UpperMaxPct.SetFloat(0.0f);

        In_C_LowerMinPct.Name = "Bar C LowerWick% Min [PatternBars>=3]";
        In_C_LowerMinPct.SetFloat(0.0f);
        In_C_LowerUseMax.Name = "Bar C LowerWick% Use Max? (0/1) [PatternBars>=3]";
        In_C_LowerUseMax.SetInt(0);
        In_C_LowerMaxPct.Name = "Bar C LowerWick% Max [PatternBars>=3]";
        In_C_LowerMaxPct.SetFloat(0.0f);

        In_C_vs_B_RangeRelationMode.Name =
            "Bar C vs Bar B Range (0=None,1=Inside,2=Outside,3=UpperBreakOnly,4=LowerBreakOnly) [PatternBars>=3]";
        In_C_vs_B_RangeRelationMode.SetInt(0);

        In_C_vs_B_NoBreakMode.Name =
            "Bar C vs Bar B No-Break Constraint (0=None,1=CannotBreakLower,2=CannotBreakUpper) [PatternBars>=3]";
        In_C_vs_B_NoBreakMode.SetInt(0);

        In_C_RetraceEnable.Name = "Bar C Retrace Gate Enable? (0/1) [vs Bar B]";
        In_C_RetraceEnable.SetInt(0);
        In_C_RetraceMinPct.Name = "Bar C Min Retrace % [vs Bar B]";
        In_C_RetraceMinPct.SetFloat(0.0f);
        In_C_RetraceMaxPct.Name = "Bar C Max Retrace % [vs Bar B]";
        In_C_RetraceMaxPct.SetFloat(0.0f);

        In_C_TuneToATR.Name   = "Bar C Tune to ATR? (0/1) [PatternBars>=3]";
        In_C_TuneToATR.SetInt(0);
        In_C_ATRMultiple.Name = "Bar C ATR Multiple (Range >= ATR * X) [PatternBars>=3]";
        In_C_ATRMultiple.SetFloat(0.0f);

        In_C_RelSizeEnable.Name = "Bar C Relative Candle Size Enable? (0/1) [vs Bar B]";
        In_C_RelSizeEnable.SetInt(0);
        In_C_RelSizeMinPct.Name = "Bar C Relative Candle Size Min % [vs Bar B]";
        In_C_RelSizeMinPct.SetFloat(0.0f);
        In_C_RelSizeMaxPct.Name = "Bar C Relative Candle Size Max % [vs Bar B]";
        In_C_RelSizeMaxPct.SetFloat(300.0f);

        In_C_TPOPOCGate.Name = "Bar C Crosses TPO POC? (0=OFF,1=YES,2=NO) [PatternBars>=3]";
        In_C_TPOPOCGate.SetInt(0);
        In_C_PSCGate.Name = "Bar C Crosses Prior Session Close? (0=OFF,1=YES,2=NO) [PatternBars>=3]";
        In_C_PSCGate.SetInt(0);

        // ---------------- Bar D ----------------
        In_D_Direction.Name = "Bar D Direction (0=Either,1=Bull,2=Bear) [PatternBars>=4]";
        In_D_Direction.SetInt(0);

        In_D_BodyMinPct.Name = "Bar D Body% Min [PatternBars>=4]";
        In_D_BodyMinPct.SetFloat(0.0f);
        In_D_BodyUseMax.Name = "Bar D Body% Use Max? (0/1) [PatternBars>=4]";
        In_D_BodyUseMax.SetInt(0);
        In_D_BodyMaxPct.Name = "Bar D Body% Max [PatternBars>=4]";
        In_D_BodyMaxPct.SetFloat(0.0f);

        In_D_UpperMinPct.Name = "Bar D UpperWick% Min [PatternBars>=4]";
        In_D_UpperMinPct.SetFloat(0.0f);
        In_D_UpperUseMax.Name = "Bar D UpperWick% Use Max? (0/1) [PatternBars>=4]";
        In_D_UpperUseMax.SetInt(0);
        In_D_UpperMaxPct.Name = "Bar D UpperWick% Max [PatternBars>=4]";
        In_D_UpperMaxPct.SetFloat(0.0f);

        In_D_LowerMinPct.Name = "Bar D LowerWick% Min [PatternBars>=4]";
        In_D_LowerMinPct.SetFloat(0.0f);
        In_D_LowerUseMax.Name = "Bar D LowerWick% Use Max? (0/1) [PatternBars>=4]";
        In_D_LowerUseMax.SetInt(0);
        In_D_LowerMaxPct.Name = "Bar D LowerWick% Max [PatternBars>=4]";
        In_D_LowerMaxPct.SetFloat(0.0f);

        In_D_vs_C_RangeRelationMode.Name =
            "Bar D vs Bar C Range (0=None,1=Inside,2=Outside,3=UpperBreakOnly,4=LowerBreakOnly) [PatternBars>=4]";
        In_D_vs_C_RangeRelationMode.SetInt(0);

        In_D_vs_C_NoBreakMode.Name =
            "Bar D vs Bar C No-Break Constraint (0=None,1=CannotBreakLower,2=CannotBreakUpper) [PatternBars>=4]";
        In_D_vs_C_NoBreakMode.SetInt(0);

        In_D_RetraceEnable.Name = "Bar D Retrace Gate Enable? (0/1) [vs Bar C]";
        In_D_RetraceEnable.SetInt(0);
        In_D_RetraceMinPct.Name = "Bar D Min Retrace % [vs Bar C]";
        In_D_RetraceMinPct.SetFloat(0.0f);
        In_D_RetraceMaxPct.Name = "Bar D Max Retrace % [vs Bar C]";
        In_D_RetraceMaxPct.SetFloat(0.0f);

        In_D_TuneToATR.Name   = "Bar D Tune to ATR? (0/1) [PatternBars>=4]";
        In_D_TuneToATR.SetInt(0);
        In_D_ATRMultiple.Name = "Bar D ATR Multiple (Range >= ATR * X) [PatternBars>=4]";
        In_D_ATRMultiple.SetFloat(0.0f);

        In_D_RelSizeEnable.Name = "Bar D Relative Candle Size Enable? (0/1) [vs Bar C]";
        In_D_RelSizeEnable.SetInt(0);
        In_D_RelSizeMinPct.Name = "Bar D Relative Candle Size Min % [vs Bar C]";
        In_D_RelSizeMinPct.SetFloat(0.0f);
        In_D_RelSizeMaxPct.Name = "Bar D Relative Candle Size Max % [vs Bar C]";
        In_D_RelSizeMaxPct.SetFloat(300.0f);

        In_D_TPOPOCGate.Name = "Bar D Crosses TPO POC? (0=OFF,1=YES,2=NO) [PatternBars>=4]";
        In_D_TPOPOCGate.SetInt(0);
        In_D_PSCGate.Name = "Bar D Crosses Prior Session Close? (0=OFF,1=YES,2=NO) [PatternBars>=4]";
        In_D_PSCGate.SetInt(0);

        // ---------------- Bar E ----------------
        In_E_Direction.Name = "Bar E Direction (0=Either,1=Bull,2=Bear) [PatternBars=5]";
        In_E_Direction.SetInt(0);

        In_E_BodyMinPct.Name = "Bar E Body% Min [PatternBars=5]";
        In_E_BodyMinPct.SetFloat(0.0f);
        In_E_BodyUseMax.Name = "Bar E Body% Use Max? (0/1) [PatternBars=5]";
        In_E_BodyUseMax.SetInt(0);
        In_E_BodyMaxPct.Name = "Bar E Body% Max [PatternBars=5]";
        In_E_BodyMaxPct.SetFloat(0.0f);

        In_E_UpperMinPct.Name = "Bar E UpperWick% Min [PatternBars=5]";
        In_E_UpperMinPct.SetFloat(0.0f);
        In_E_UpperUseMax.Name = "Bar E UpperWick% Use Max? (0/1) [PatternBars=5]";
        In_E_UpperUseMax.SetInt(0);
        In_E_UpperMaxPct.Name = "Bar E UpperWick% Max [PatternBars=5]";
        In_E_UpperMaxPct.SetFloat(0.0f);

        In_E_LowerMinPct.Name = "Bar E LowerWick% Min [PatternBars=5]";
        In_E_LowerMinPct.SetFloat(0.0f);
        In_E_LowerUseMax.Name = "Bar E LowerWick% Use Max? (0/1) [PatternBars=5]";
        In_E_LowerUseMax.SetInt(0);
        In_E_LowerMaxPct.Name = "Bar E LowerWick% Max [PatternBars=5]";
        In_E_LowerMaxPct.SetFloat(0.0f);

        In_E_vs_D_RangeRelationMode.Name =
            "Bar E vs Bar D Range (0=None,1=Inside,2=Outside,3=UpperBreakOnly,4=LowerBreakOnly) [PatternBars=5]";
        In_E_vs_D_RangeRelationMode.SetInt(0);

        In_E_vs_D_NoBreakMode.Name =
            "Bar E vs Bar D No-Break Constraint (0=None,1=CannotBreakLower,2=CannotBreakUpper) [PatternBars=5]";
        In_E_vs_D_NoBreakMode.SetInt(0);

        In_E_RetraceEnable.Name = "Bar E Retrace Gate Enable? (0/1) [vs Bar D]";
        In_E_RetraceEnable.SetInt(0);
        In_E_RetraceMinPct.Name = "Bar E Min Retrace % [vs Bar D]";
        In_E_RetraceMinPct.SetFloat(0.0f);
        In_E_RetraceMaxPct.Name = "Bar E Max Retrace % [vs Bar D]";
        In_E_RetraceMaxPct.SetFloat(0.0f);

        In_E_TuneToATR.Name   = "Bar E Tune to ATR? (0/1) [PatternBars=5]";
        In_E_TuneToATR.SetInt(0);
        In_E_ATRMultiple.Name = "Bar E ATR Multiple (Range >= ATR * X) [PatternBars=5]";
        In_E_ATRMultiple.SetFloat(0.0f);

        In_E_RelSizeEnable.Name = "Bar E Relative Candle Size Enable? (0/1) [vs Bar D]";
        In_E_RelSizeEnable.SetInt(0);
        In_E_RelSizeMinPct.Name = "Bar E Relative Candle Size Min % [vs Bar D]";
        In_E_RelSizeMinPct.SetFloat(0.0f);
        In_E_RelSizeMaxPct.Name = "Bar E Relative Candle Size Max % [vs Bar D]";
        In_E_RelSizeMaxPct.SetFloat(300.0f);

        In_E_TPOPOCGate.Name = "Bar E Crosses TPO POC? (0=OFF,1=YES,2=NO) [PatternBars=5]";
        In_E_TPOPOCGate.SetInt(0);
        In_E_PSCGate.Name = "Bar E Crosses Prior Session Close? (0=OFF,1=YES,2=NO) [PatternBars=5]";
        In_E_PSCGate.SetInt(0);

        // Marker defaults
        In_DrawMode.Name = "Marker Mode (0=UpArrowBelow, 1=DownArrowAbove, 2=TwoGrayDots)";
        In_DrawMode.SetInt(0);

        In_ArrowOffsetTicks.Name = "Marker Offset (Ticks)";
        In_ArrowOffsetTicks.SetInt(2);

        // Relative volume defaults
        In_UseRelVolume.Name = "Use Relative Volume Filter? (0/1) [Signal Bar]";
        In_UseRelVolume.SetInt(0);

        In_VolLookbackBars.Name = "Relative Volume Lookback (Bars) [Signal Bar]";
        In_VolLookbackBars.SetInt(14);

        In_VolMultiple.Name = "Relative Volume Multiple Cutoff (SignalVol >= Avg * X)";
        In_VolMultiple.SetFloat(2.0f);

        // Relative extreme defaults
        In_ExtremeMode.Name = "Relative Extreme (0=None, 1=HighestHigh, 2=LowestLow)";
        In_ExtremeMode.SetInt(0);

        In_NoExtremeMode.Name = "No Relative Extreme (0=None, 1=HighestHigh, 2=LowestLow)";
        In_NoExtremeMode.SetInt(0);

        In_ExtremeWindowMin.Name = "Relative Extreme Window (Minutes)";
        In_ExtremeWindowMin.SetInt(240);

        In_ExtremeUseBars.Name = "Relative Extreme (in bars): 0=Disabled, 1=Enabled";
        In_ExtremeUseBars.SetInt(0);

        In_ExtremeWindowBars.Name = "Relative Extreme Lookback Period (in bars)";
        In_ExtremeWindowBars.SetInt(20);

        In_ExtremeWhichBar.Name = "Extreme Basis: 0=Any in Pattern, 1=A,2=B,3=C,4=D,5=E";
        In_ExtremeWhichBar.SetInt(0);

        // Large Volume Location defaults
        In_UseLargeVolumeLocation.Name = "Use Large Volume Location Tool? (0=No, 1=Yes)";
        In_UseLargeVolumeLocation.SetInt(0);

        In_LV_WhichCandle.Name = "Apply Large Volume Location To Which Candle (0=None,1=A,2=B,3=C,4=D,5=E)";
        In_LV_WhichCandle.SetInt(0);

        In_LV_SideFilter.Name = "Large Volume Buy/Sell";
        In_LV_SideFilter.SetCustomInputStrings("Either;Buy;Sell");
        In_LV_SideFilter.SetCustomInputIndex(2); // default Sell

        In_LV_VolumeThreshold.Name = "Large Volume Threshold";
        In_LV_VolumeThreshold.SetInt(20);
        In_LV_VolumeThreshold.SetIntLimits(1, INT_MAX);

        In_LV_TopPercent.Name = "Large Volume Must Occur In Top Range % (0=Off)";
        In_LV_TopPercent.SetFloat(0.0f);
        In_LV_TopPercent.SetFloatLimits(0.0f, 100.0f);

        In_LV_BottomPercent.Name = "Large Volume Must Occur In Bottom Range % (0=Off)";
        In_LV_BottomPercent.SetFloat(25.0f);
        In_LV_BottomPercent.SetFloatLimits(0.0f, 100.0f);

        In_LV_UseEitherZone.Name = "If Large Volume Top and Bottom Both Set Use Either";
        In_LV_UseEitherZone.SetYesNo(1);

        return;
    }

    const int i = sc.Index;

    const int drawMode = In_DrawMode.GetInt();

    // DrawStyle is subgraph-wide, not per-bar.
    if (drawMode == 2)
    {
        Signal.DrawStyle    = DRAWSTYLE_IGNORE;
        DotAbove.DrawStyle  = DRAWSTYLE_POINT;
        DotBelow.DrawStyle  = DRAWSTYLE_POINT;

        DotAbove.PrimaryColor = RGB(160, 160, 160);
        DotBelow.PrimaryColor = RGB(160, 160, 160);
    }
    else if (drawMode == 1)
    {
        Signal.DrawStyle    = DRAWSTYLE_ARROW_DOWN;
        Signal.PrimaryColor = RGB(255, 0, 0);

        DotAbove.DrawStyle  = DRAWSTYLE_IGNORE;
        DotBelow.DrawStyle  = DRAWSTYLE_IGNORE;
    }
    else
    {
        Signal.DrawStyle    = DRAWSTYLE_ARROW_UP;
        Signal.PrimaryColor = RGB(0, 255, 0);

        DotAbove.DrawStyle  = DRAWSTYLE_IGNORE;
        DotBelow.DrawStyle  = DRAWSTYLE_IGNORE;
    }

    // IMPORTANT:
    // On a full recalculation, clear ALL existing markers first,
    // otherwise stale arrows/dots from prior settings can remain.
    if (sc.IsFullRecalculation && i == 0)
    {
        for (int j = 0; j < sc.ArraySize; ++j)
        {
            Signal[j]   = 0.0f;
            DotAbove[j] = 0.0f;
            DotBelow[j] = 0.0f;
        }
    }

    // Always clear the current bar before evaluating it.
    Signal[i]   = 0.0f;
    DotAbove[i] = 0.0f;
    DotBelow[i] = 0.0f;
    const int patternBars = In_PatternBars.GetInt();
    if (patternBars < 1 || patternBars > 5)
        return;

    // historical-only behavior
    if (i == sc.ArraySize - 1)
        return;

    if (i < (patternBars - 1))
        return;

    const int atrLen = In_ATRLength.GetInt();
    if (atrLen >= 1)
        sc.ATR(sc.BaseDataIn, ATR, atrLen, MOVAVGTYPE_SIMPLE);

    SCFloatArray TPOPOCArray;
    sc.GetStudyArrayUsingID(
        In_TPOPOCSource.GetStudyID(),
        In_TPOPOCSource.GetSubgraphIndex(),
        TPOPOCArray);

    SCFloatArray PSCArray;
    sc.GetStudyArrayUsingID(
        In_PSCSource.GetStudyID(),
        In_PSCSource.GetSubgraphIndex(),
        PSCArray);

    const int idxSignal = i;
    const int idxA = idxSignal - (patternBars - 1);
    const int idxB = idxA + 1;
    const int idxC = idxA + 2;
    const int idxD = idxA + 3;
    const int idxE = idxA + 4;

    float oA = sc.Open[idxA], hA = sc.High[idxA], lA = sc.Low[idxA], cA = sc.Close[idxA];
    float oB=0,hB=0,lB=0,cB=0;
    float oC=0,hC=0,lC=0,cC=0;
    float oD=0,hD=0,lD=0,cD=0;
    float oE=0,hE=0,lE=0,cE=0;

    if (patternBars >= 2) { oB=sc.Open[idxB]; hB=sc.High[idxB]; lB=sc.Low[idxB]; cB=sc.Close[idxB]; }
    if (patternBars >= 3) { oC=sc.Open[idxC]; hC=sc.High[idxC]; lC=sc.Low[idxC]; cC=sc.Close[idxC]; }
    if (patternBars >= 4) { oD=sc.Open[idxD]; hD=sc.High[idxD]; lD=sc.Low[idxD]; cD=sc.Close[idxD]; }
    if (patternBars == 5) { oE=sc.Open[idxE]; hE=sc.High[idxE]; lE=sc.Low[idxE]; cE=sc.Close[idxE]; }

    // Direction gates
    if (!PassDirection(In_A_Direction.GetInt(), oA, cA)) return;
    if (patternBars >= 2) if (!PassDirection(In_B_Direction.GetInt(), oB, cB)) return;
    if (patternBars >= 3) if (!PassDirection(In_C_Direction.GetInt(), oC, cC)) return;
    if (patternBars >= 4) if (!PassDirection(In_D_Direction.GetInt(), oD, cD)) return;
    if (patternBars == 5) if (!PassDirection(In_E_Direction.GetInt(), oE, cE)) return;

    // Range-to-ATR gates
    if (!PassRangeToATR(In_A_TuneToATR.GetInt(), hA, lA, ATR[idxA], In_A_ATRMultiple.GetFloat())) return;
    if (patternBars >= 2) if (!PassRangeToATR(In_B_TuneToATR.GetInt(), hB, lB, ATR[idxB], In_B_ATRMultiple.GetFloat())) return;
    if (patternBars >= 3) if (!PassRangeToATR(In_C_TuneToATR.GetInt(), hC, lC, ATR[idxC], In_C_ATRMultiple.GetFloat())) return;
    if (patternBars >= 4) if (!PassRangeToATR(In_D_TuneToATR.GetInt(), hD, lD, ATR[idxD], In_D_ATRMultiple.GetFloat())) return;
    if (patternBars == 5) if (!PassRangeToATR(In_E_TuneToATR.GetInt(), hE, lE, ATR[idxE], In_E_ATRMultiple.GetFloat())) return;

    // Relative candle size gates
    if (patternBars >= 2)
        if (!PassRelativeCandleSize(In_B_RelSizeEnable.GetInt(), hA, lA, hB, lB,
                                    In_B_RelSizeMinPct.GetFloat(), In_B_RelSizeMaxPct.GetFloat())) return;

    if (patternBars >= 3)
        if (!PassRelativeCandleSize(In_C_RelSizeEnable.GetInt(), hB, lB, hC, lC,
                                    In_C_RelSizeMinPct.GetFloat(), In_C_RelSizeMaxPct.GetFloat())) return;

    if (patternBars >= 4)
        if (!PassRelativeCandleSize(In_D_RelSizeEnable.GetInt(), hC, lC, hD, lD,
                                    In_D_RelSizeMinPct.GetFloat(), In_D_RelSizeMaxPct.GetFloat())) return;

    if (patternBars == 5)
        if (!PassRelativeCandleSize(In_E_RelSizeEnable.GetInt(), hD, lD, hE, lE,
                                    In_E_RelSizeMinPct.GetFloat(), In_E_RelSizeMaxPct.GetFloat())) return;

    // Range relationship + no-break constraint
    if (patternBars >= 2)
    {
        if (!PassRangeRelationship(In_B_vs_A_RangeRelationMode.GetInt(), hA, lA, hB, lB, sc.TickSize)) return;
        if (!PassNoBreakConstraint(In_B_vs_A_NoBreakMode.GetInt(),       hA, lA, hB, lB, sc.TickSize)) return;
    }
    if (patternBars >= 3)
    {
        if (!PassRangeRelationship(In_C_vs_B_RangeRelationMode.GetInt(), hB, lB, hC, lC, sc.TickSize)) return;
        if (!PassNoBreakConstraint(In_C_vs_B_NoBreakMode.GetInt(),       hB, lB, hC, lC, sc.TickSize)) return;
    }
    if (patternBars >= 4)
    {
        if (!PassRangeRelationship(In_D_vs_C_RangeRelationMode.GetInt(), hC, lC, hD, lD, sc.TickSize)) return;
        if (!PassNoBreakConstraint(In_D_vs_C_NoBreakMode.GetInt(),       hC, lC, hD, lD, sc.TickSize)) return;
    }
    if (patternBars == 5)
    {
        if (!PassRangeRelationship(In_E_vs_D_RangeRelationMode.GetInt(), hD, lD, hE, lE, sc.TickSize)) return;
        if (!PassNoBreakConstraint(In_E_vs_D_NoBreakMode.GetInt(),       hD, lD, hE, lE, sc.TickSize)) return;
    }

    // Retrace gates
    if (patternBars >= 2)
        if (!PassRetraceGate(In_B_RetraceEnable.GetInt(), oA,hA,lA,cA, hB,lB,
                             In_B_RetraceMinPct.GetFloat(), In_B_RetraceMaxPct.GetFloat(), sc.TickSize)) return;
    if (patternBars >= 3)
        if (!PassRetraceGate(In_C_RetraceEnable.GetInt(), oB,hB,lB,cB, hC,lC,
                             In_C_RetraceMinPct.GetFloat(), In_C_RetraceMaxPct.GetFloat(), sc.TickSize)) return;
    if (patternBars >= 4)
        if (!PassRetraceGate(In_D_RetraceEnable.GetInt(), oC,hC,lC,cC, hD,lD,
                             In_D_RetraceMinPct.GetFloat(), In_D_RetraceMaxPct.GetFloat(), sc.TickSize)) return;
    if (patternBars == 5)
        if (!PassRetraceGate(In_E_RetraceEnable.GetInt(), oD,hD,lD,cD, hE,lE,
                             In_E_RetraceMinPct.GetFloat(), In_E_RetraceMaxPct.GetFloat(), sc.TickSize)) return;

    // Relative volume filter
    if (!PassRelativeVolume(sc,
                            In_UseRelVolume.GetInt(),
                            idxSignal,
                            In_VolLookbackBars.GetInt(),
                            In_VolMultiple.GetFloat()))
        return;

    // Relative extreme filter
    if (In_ExtremeMode.GetInt() != 0)
    {
        if (!PassRelativeExtreme(sc,
                                 idxSignal,
                                 In_ExtremeMode.GetInt(),
                                 In_ExtremeWindowMin.GetInt(),
                                 In_ExtremeUseBars.GetInt(),
                                 In_ExtremeWindowBars.GetInt(),
                                 patternBars,
                                 In_ExtremeWhichBar.GetInt()))
            return;
    }

    // No Relative Extreme filter
    if (In_NoExtremeMode.GetInt() != 0)
    {
        if (PassRelativeExtreme(sc,
                                idxSignal,
                                In_NoExtremeMode.GetInt(),
                                In_ExtremeWindowMin.GetInt(),
                                In_ExtremeUseBars.GetInt(),
                                In_ExtremeWindowBars.GetInt(),
                                patternBars,
                                In_ExtremeWhichBar.GetInt()))
            return;
    }

    // Large Volume Location gate
if (In_UseLargeVolumeLocation.GetInt() != 0)
{
    const int whichLV = In_LV_WhichCandle.GetInt();

    int idxLV = -1;
    if      (whichLV == 1) idxLV = idxA;
    else if (whichLV == 2 && patternBars >= 2) idxLV = idxB;
    else if (whichLV == 3 && patternBars >= 3) idxLV = idxC;
    else if (whichLV == 4 && patternBars >= 4) idxLV = idxD;
    else if (whichLV == 5 && patternBars >= 5) idxLV = idxE;

    // IMPORTANT:
    // If the user enabled the LV filter but did not choose a valid candle,
    // do NOT fail-open. Fail-closed so the filter is truly enforced.
    if (idxLV < 0)
        return;

    if (!PassLargeVolumeLocation(
            sc,
            idxLV,
            In_LV_SideFilter.GetIndex(),
            (unsigned int)In_LV_VolumeThreshold.GetInt(),
            In_LV_TopPercent.GetFloat(),
            In_LV_BottomPercent.GetFloat(),
            In_LV_UseEitherZone.GetYesNo()))
        return;
}
    

    // TPO POC gates: short carry-forward only
    if (!PassStudyLevelGate(sc, In_A_TPOPOCGate.GetInt(), idxA, TPOPOCArray, 1)) return;
    if (patternBars >= 2) if (!PassStudyLevelGate(sc, In_B_TPOPOCGate.GetInt(), idxB, TPOPOCArray, 1)) return;
    if (patternBars >= 3) if (!PassStudyLevelGate(sc, In_C_TPOPOCGate.GetInt(), idxC, TPOPOCArray, 1)) return;
    if (patternBars >= 4) if (!PassStudyLevelGate(sc, In_D_TPOPOCGate.GetInt(), idxD, TPOPOCArray, 1)) return;
    if (patternBars == 5) if (!PassStudyLevelGate(sc, In_E_TPOPOCGate.GetInt(), idxE, TPOPOCArray, 1)) return;

    // PSC gates: long carry-forward okay
    if (!PassStudyLevelGate(sc, In_A_PSCGate.GetInt(), idxA, PSCArray, 5000)) return;
    if (patternBars >= 2) if (!PassStudyLevelGate(sc, In_B_PSCGate.GetInt(), idxB, PSCArray, 5000)) return;
    if (patternBars >= 3) if (!PassStudyLevelGate(sc, In_C_PSCGate.GetInt(), idxC, PSCArray, 5000)) return;
    if (patternBars >= 4) if (!PassStudyLevelGate(sc, In_D_PSCGate.GetInt(), idxD, PSCArray, 5000)) return;
    if (patternBars == 5) if (!PassStudyLevelGate(sc, In_E_PSCGate.GetInt(), idxE, PSCArray, 5000)) return;

    // Percent gates
    CandlePercents pA, pB, pC, pD, pE;
    if (!ComputePercents(oA, hA, lA, cA, pA)) return;

    if (!PassMinMax(pA.BodyPct,      In_A_BodyMinPct.GetFloat(),  In_A_BodyMaxPct.GetFloat(),  In_A_BodyUseMax.GetInt()))  return;
    if (!PassMinMax(pA.UpperWickPct, In_A_UpperMinPct.GetFloat(), In_A_UpperMaxPct.GetFloat(), In_A_UpperUseMax.GetInt())) return;
    if (!PassMinMax(pA.LowerWickPct, In_A_LowerMinPct.GetFloat(), In_A_LowerMaxPct.GetFloat(), In_A_LowerUseMax.GetInt())) return;

    if (patternBars >= 2)
    {
        if (!ComputePercents(oB, hB, lB, cB, pB)) return;
        if (!PassMinMax(pB.BodyPct,      In_B_BodyMinPct.GetFloat(),  In_B_BodyMaxPct.GetFloat(),  In_B_BodyUseMax.GetInt()))  return;
        if (!PassMinMax(pB.UpperWickPct, In_B_UpperMinPct.GetFloat(), In_B_UpperMaxPct.GetFloat(), In_B_UpperUseMax.GetInt())) return;
        if (!PassMinMax(pB.LowerWickPct, In_B_LowerMinPct.GetFloat(), In_B_LowerMaxPct.GetFloat(), In_B_LowerUseMax.GetInt())) return;
    }
    if (patternBars >= 3)
    {
        if (!ComputePercents(oC, hC, lC, cC, pC)) return;
        if (!PassMinMax(pC.BodyPct,      In_C_BodyMinPct.GetFloat(),  In_C_BodyMaxPct.GetFloat(),  In_C_BodyUseMax.GetInt()))  return;
        if (!PassMinMax(pC.UpperWickPct, In_C_UpperMinPct.GetFloat(), In_C_UpperMaxPct.GetFloat(), In_C_UpperUseMax.GetInt())) return;
        if (!PassMinMax(pC.LowerWickPct, In_C_LowerMinPct.GetFloat(), In_C_LowerMaxPct.GetFloat(), In_C_LowerUseMax.GetInt())) return;
    }
    if (patternBars >= 4)
    {
        if (!ComputePercents(oD, hD, lD, cD, pD)) return;
        if (!PassMinMax(pD.BodyPct,      In_D_BodyMinPct.GetFloat(),  In_D_BodyMaxPct.GetFloat(),  In_D_BodyUseMax.GetInt()))  return;
        if (!PassMinMax(pD.UpperWickPct, In_D_UpperMinPct.GetFloat(), In_D_UpperMaxPct.GetFloat(), In_D_UpperUseMax.GetInt())) return;
        if (!PassMinMax(pD.LowerWickPct, In_D_LowerMinPct.GetFloat(), In_D_LowerMaxPct.GetFloat(), In_D_LowerUseMax.GetInt())) return;
    }
    if (patternBars == 5)
    {
        if (!ComputePercents(oE, hE, lE, cE, pE)) return;
        if (!PassMinMax(pE.BodyPct,      In_E_BodyMinPct.GetFloat(),  In_E_BodyMaxPct.GetFloat(),  In_E_BodyUseMax.GetInt()))  return;
        if (!PassMinMax(pE.UpperWickPct, In_E_UpperMinPct.GetFloat(), In_E_UpperMaxPct.GetFloat(), In_E_UpperUseMax.GetInt())) return;
        if (!PassMinMax(pE.LowerWickPct, In_E_LowerMinPct.GetFloat(), In_E_LowerMaxPct.GetFloat(), In_E_LowerUseMax.GetInt())) return;
    }

    // Draw marker at signal bar
    const float sigHigh = sc.High[idxSignal];
    const float sigLow  = sc.Low[idxSignal];

    const int offsetTicks = In_ArrowOffsetTicks.GetInt();

    if (drawMode == 2)
    {
        Signal[idxSignal]   = 0.0f;
        DotAbove[idxSignal] = sigHigh + (float)offsetTicks * sc.TickSize;
        DotBelow[idxSignal] = sigLow  - (float)offsetTicks * sc.TickSize;
    }
    else if (drawMode == 1)
    {
        DotAbove[idxSignal] = 0.0f;
        DotBelow[idxSignal] = 0.0f;
        Signal[idxSignal]   = sigHigh + (float)offsetTicks * sc.TickSize;
    }
    else
    {
        DotAbove[idxSignal] = 0.0f;
        DotBelow[idxSignal] = 0.0f;
        Signal[idxSignal]   = sigLow - (float)offsetTicks * sc.TickSize;
    }
}