#include "sierrachart.h"

SCDLLName("Range ATR Full Body Fill")

SCSFExport scsf_RangeATRFullBodyFill(SCStudyInterfaceRef sc)
{
    SCSubgraphRef BodyOpen  = sc.Subgraph[0];
    SCSubgraphRef BodyClose = sc.Subgraph[1];
    SCSubgraphRef ATRSG     = sc.Subgraph[2];
    SCSubgraphRef RatioSG   = sc.Subgraph[3];

    SCInputRef In_ATRLength   = sc.Input[0];
    SCInputRef In_Threshold   = sc.Input[1];
    SCInputRef In_UseEqual    = sc.Input[2];

    if (sc.SetDefaults)
    {
        sc.GraphName = "Range ATR Full Body Fill";
        sc.StudyDescription = "Fills qualifying hollow candle bodies when Range/ATR exceeds a threshold.";
        sc.AutoLoop = 1;
        sc.GraphRegion = 0;
        sc.ValueFormat = VALUEFORMAT_INHERITED;

        // ---- Visible candle-body overlay subgraphs ----
        BodyOpen.Name = "Body Open";
        BodyOpen.DrawStyle = DRAWSTYLE_CANDLESTICK_BODY_OPEN;
        BodyOpen.LineWidth = 1;
        BodyOpen.PrimaryColor = RGB(0, 255, 0);      // Up candle fill
        BodyOpen.SecondaryColor = RGB(255, 0, 0);    // Down candle fill
        BodyOpen.SecondaryColorUsed = 1;
        BodyOpen.DrawZeros = false;

        BodyClose.Name = "Body Close";
        BodyClose.DrawStyle = DRAWSTYLE_CANDLESTICK_BODY_CLOSE;
        BodyClose.LineWidth = 1;
        BodyClose.PrimaryColor = RGB(0, 255, 0);     // Up candle outline
        BodyClose.SecondaryColor = RGB(255, 0, 0);   // Down candle outline
        BodyClose.SecondaryColorUsed = 1;
        BodyClose.DrawZeros = false;

        // ---- Hidden working subgraphs ----
        ATRSG.Name = "ATR";
        ATRSG.DrawStyle = DRAWSTYLE_IGNORE;
        ATRSG.DrawZeros = false;

        RatioSG.Name = "Range/ATR";
        RatioSG.DrawStyle = DRAWSTYLE_IGNORE;
        RatioSG.DrawZeros = false;

        // ---- Inputs ----
        In_ATRLength.Name = "ATR Lookback Period";
        In_ATRLength.SetInt(14);
        In_ATRLength.SetIntLimits(1, 10000);

        In_Threshold.Name = "Range/ATR Threshold (0 = Disabled)";
        In_Threshold.SetFloat(0.0f);

        In_UseEqual.Name = "Use Greater Than Or Equal To Threshold";
        In_UseEqual.SetYesNo(1);

        return;
    }

    const int atrLength = In_ATRLength.GetInt();
    const float threshold = In_Threshold.GetFloat();
    const bool useEqual = In_UseEqual.GetYesNo() != 0;

    // Clear current bar by default so only qualifying candles draw
    BodyOpen[sc.Index] = 0.0f;
    BodyClose[sc.Index] = 0.0f;

    // Compute ATR using Sierra Chart's ATR function
    sc.ATR(sc.BaseDataIn, ATRSG, atrLength, MOVAVGTYPE_SIMPLE);

    const float atr = ATRSG[sc.Index];
    const float range = sc.High[sc.Index] - sc.Low[sc.Index];

    if (atr <= 0.0f)
    {
        RatioSG[sc.Index] = 0.0f;
        return;
    }

    const float ratio = range / atr;
    RatioSG[sc.Index] = ratio;

    // Threshold of 0 means inactive
    if (threshold <= 0.0f)
        return;

    const bool qualifies = useEqual ? (ratio >= threshold) : (ratio > threshold);
    if (!qualifies)
        return;

    // Draw a filled candle body from Open to Close
    BodyOpen[sc.Index] = sc.Open[sc.Index];
    BodyClose[sc.Index] = sc.Close[sc.Index];

    // Select up/down colors based on candle direction
    if (sc.Close[sc.Index] >= sc.Open[sc.Index])
    {
        BodyOpen.DataColor[sc.Index] = BodyOpen.PrimaryColor;
        BodyClose.DataColor[sc.Index] = BodyClose.PrimaryColor;
    }
    else
    {
        BodyOpen.DataColor[sc.Index] = BodyOpen.SecondaryColor;
        BodyClose.DataColor[sc.Index] = BodyClose.SecondaryColor;
    }
}