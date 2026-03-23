#include "sierrachart.h"
#include <float.h>

SCDLLName("Large Volume Location Arrow")

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

SCSFExport scsf_LargeVolumeLocationArrow(SCStudyInterfaceRef sc)
{
    SCSubgraphRef Marker = sc.Subgraph[0];

    SCInputRef In_SideFilter       = sc.Input[0];
    SCInputRef In_VolumeThreshold  = sc.Input[1];
    SCInputRef In_TopPercent       = sc.Input[2];
    SCInputRef In_BottomPercent    = sc.Input[3];
    SCInputRef In_MarkerPosition   = sc.Input[4];
    SCInputRef In_ArrowDirection   = sc.Input[5];
    SCInputRef In_ArrowColor       = sc.Input[6];
    SCInputRef In_OffsetTicks      = sc.Input[7];
    SCInputRef In_ArrowSize        = sc.Input[8];
    SCInputRef In_UseEitherZone    = sc.Input[9];
    SCInputRef In_DebugLogging     = sc.Input[10];

    if (sc.SetDefaults)
    {
        sc.GraphName = "Large Volume Location Arrow";
        sc.StudyDescription =
            "Uses sc.p_VolumeLevelAtPriceForBars (same data as Large Volume Trade Indicator) "
            "to place an arrow when qualifying buy/sell volume occurs in the top/bottom "
            "percent of the bar range.";
        sc.AutoLoop = 0;
        sc.UpdateAlways = 1;
        sc.GraphRegion = 0;
        sc.ValueFormat = VALUEFORMAT_INHERITED;

        // Required for price-level volume data
        sc.MaintainVolumeAtPriceData = 1;

        Marker.Name = "Signal";
        Marker.DrawStyle = DRAWSTYLE_ARROW_UP;
        Marker.PrimaryColor = RGB(0, 255, 0);
        Marker.LineWidth = 3;
        Marker.DrawZeros = false;

        In_SideFilter.Name = "Large Volume Buy/Sell";
        In_SideFilter.SetCustomInputStrings("Either;Buy;Sell");
        In_SideFilter.SetCustomInputIndex(2); // default Sell

        In_VolumeThreshold.Name = "Volume Threshold";
        In_VolumeThreshold.SetInt(20);
        In_VolumeThreshold.SetIntLimits(1, INT_MAX);

        In_TopPercent.Name = "Must Occur In Top Range % (0=Off)";
        In_TopPercent.SetFloat(0.0f);
        In_TopPercent.SetFloatLimits(0.0f, 100.0f);

        In_BottomPercent.Name = "Must Occur In Bottom Range % (0=Off)";
        In_BottomPercent.SetFloat(25.0f);
        In_BottomPercent.SetFloatLimits(0.0f, 100.0f);

        In_MarkerPosition.Name = "Marker Position";
        In_MarkerPosition.SetCustomInputStrings("Above Bar;Below Bar");
        In_MarkerPosition.SetCustomInputIndex(1); // Below Bar

        In_ArrowDirection.Name = "Arrow Direction";
        In_ArrowDirection.SetCustomInputStrings("Auto;Down;Up");
        In_ArrowDirection.SetCustomInputIndex(0);

        In_ArrowColor.Name = "Arrow Color";
        In_ArrowColor.SetColor(RGB(0, 255, 0));

        In_OffsetTicks.Name = "Arrow Offset (Ticks)";
        In_OffsetTicks.SetInt(20);
        In_OffsetTicks.SetIntLimits(0, INT_MAX);

        In_ArrowSize.Name = "Arrow Size";
        In_ArrowSize.SetInt(3);
        In_ArrowSize.SetIntLimits(1, 10);

        In_UseEitherZone.Name = "If Top and Bottom Both Set Use Either";
        In_UseEitherZone.SetYesNo(1);

        In_DebugLogging.Name = "Debug Logging";
        In_DebugLogging.SetYesNo(0);

        return;
    }

    const int arraySize = sc.ArraySize;
    if (arraySize <= 0)
        return;

    Marker.PrimaryColor = In_ArrowColor.GetColor();
    Marker.LineWidth = In_ArrowSize.GetInt();

    const int markerPos = In_MarkerPosition.GetIndex(); // 0 above, 1 below
    const int arrowDir  = In_ArrowDirection.GetIndex(); // 0 auto, 1 down, 2 up

    if (arrowDir == 0)
        Marker.DrawStyle = (markerPos == 0) ? DRAWSTYLE_ARROW_DOWN : DRAWSTYLE_ARROW_UP;
    else if (arrowDir == 1)
        Marker.DrawStyle = DRAWSTYLE_ARROW_DOWN;
    else
        Marker.DrawStyle = DRAWSTYLE_ARROW_UP;

    const int sideFilter = In_SideFilter.GetIndex(); // 0 either, 1 buy, 2 sell
    const unsigned int volThreshold = (unsigned int)In_VolumeThreshold.GetInt();
    const float topPct = In_TopPercent.GetFloat();
    const float botPct = In_BottomPercent.GetFloat();
    const bool useEither = In_UseEitherZone.GetYesNo() != 0;
    const bool debug = In_DebugLogging.GetYesNo() != 0;
    const float offset = (float)In_OffsetTicks.GetInt() * sc.TickSize;

    for (int bar = 0; bar < arraySize; ++bar)
    {
        Marker[bar] = 0.0f;

        if (sc.p_VolumeLevelAtPriceForBars == nullptr)
            continue;

        const int sizeAtBar = sc.p_VolumeLevelAtPriceForBars->GetSizeAtBarIndex(bar);
        if (sizeAtBar <= 0)
            continue;

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

            if (debug)
            {
                SCString msg;
                msg.Format("Bar=%d Price=%.8f MaxVol=%u BidVol=%u AskVol=%u",
                    bar, (double)price, maxVol, bidVol, askVol);
                sc.AddMessageToLog(msg, 0);
            }
        }

        if (!foundQualifying)
            continue;

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
            finalPass = useEither ? (topPass || botPass) : (topPass && botPass);
        else
            finalPass = (topPass || botPass);

        if (!finalPass)
            continue;

        if (markerPos == 0)
            Marker[bar] = high + offset;
        else
            Marker[bar] = low - offset;

        if (debug)
        {
            SCString msg;
            msg.Format("Marker set on bar=%d high=%.8f low=%.8f hiQual=%.8f loQual=%.8f topPass=%d botPass=%d",
                bar, (double)high, (double)low,
                (double)highestQualifyingPrice, (double)lowestQualifyingPrice,
                (int)topPass, (int)botPass);
            sc.AddMessageToLog(msg, 0);
        }
    }
}