#include "sierrachart.h"

SCDLLName("Range / ATR (Numbers Row)")

SCSFExport scsf_RangeDivATR_NumbersRow(SCStudyInterfaceRef sc)
{
    // Working subgraph for ATR (hidden)
    SCSubgraphRef SG_ATR = sc.Subgraph[0];

    // Inputs
    SCInputRef In_ATRLength    = sc.Input[0];
    SCInputRef In_UseTrueRange = sc.Input[1];
    SCInputRef In_Decimals     = sc.Input[2];
    SCInputRef In_FontSize     = sc.Input[3];
    SCInputRef In_RowYValue    = sc.Input[4];
    SCInputRef In_FixedScale   = sc.Input[5];
    SCInputRef In_ScaleTop     = sc.Input[6];
    SCInputRef In_ScaleBottom  = sc.Input[7];
    SCInputRef In_DrawLastNBars= sc.Input[8];

    if (sc.SetDefaults)
    {
        sc.GraphName        = "Range / ATR (Numbers Row)";
        sc.StudyDescription = "Displays (bar range or true range) / ATR as a horizontal row of per-bar numbers (Numbers Bars style).";
        sc.AutoLoop         = 1;

        // Put in its own region (change in Study Settings if desired)
        sc.GraphRegion = 1;

        // Hidden ATR subgraph
        SG_ATR.Name      = "ATR";
        SG_ATR.DrawStyle = DRAWSTYLE_HIDDEN;

        In_ATRLength.Name = "ATR Length";
        In_ATRLength.SetInt(14);
        In_ATRLength.SetIntLimits(1, 500);

        In_UseTrueRange.Name = "Use True Range (vs High-Low)";
        In_UseTrueRange.SetYesNo(1);

        In_Decimals.Name = "Displayed Decimals";
        In_Decimals.SetInt(2);
        In_Decimals.SetIntLimits(0, 6);

        In_FontSize.Name = "Font Size";
        In_FontSize.SetInt(8);
        In_FontSize.SetIntLimits(6, 24);

        // This is the Y value (in the study region’s value scale) where the row sits
        In_RowYValue.Name = "Row Y Value (constant line)";
        In_RowYValue.SetFloat(1.0f);

        In_FixedScale.Name = "Use Fixed Scale (recommended)";
        In_FixedScale.SetYesNo(1);

        In_ScaleTop.Name = "Fixed Scale Top";
        In_ScaleTop.SetFloat(2.0f);

        In_ScaleBottom.Name = "Fixed Scale Bottom";
        In_ScaleBottom.SetFloat(0.0f);

        In_DrawLastNBars.Name = "Draw Only Last N Bars (0 = all)";
        In_DrawLastNBars.SetInt(3000);
        In_DrawLastNBars.SetIntLimits(0, 200000);

        return;
    }

    const int atrLen = In_ATRLength.GetInt();
    sc.DataStartIndex = atrLen;

    // Stable panel scale helps keep the row visually consistent
    if (In_FixedScale.GetYesNo())
    {
        sc.ScaleRangeType   = SCALE_USERDEFINED;
        sc.ScaleRangeTop    = In_ScaleTop.GetFloat();
        sc.ScaleRangeBottom = In_ScaleBottom.GetFloat();
    }
    else
    {
        sc.ScaleRangeType = SCALE_AUTO;
    }

    // Compute ATR into hidden subgraph
    sc.ATR(sc.BaseDataIn, SG_ATR, atrLen, MOVAVGTYPE_SIMPLE);

    if (sc.Index < sc.DataStartIndex)
        return;

    // Optionally limit drawings to last N bars (performance)
    const int drawLastN = In_DrawLastNBars.GetInt();
    if (drawLastN > 0)
    {
        const int firstIndexToDraw = max(0, sc.ArraySize - drawLastN);
        if (sc.Index < firstIndexToDraw)
            return;
    }

    // Compute range (HL or True Range)
    float range = sc.High[sc.Index] - sc.Low[sc.Index];

    if (In_UseTrueRange.GetYesNo() && sc.Index > 0)
    {
        const float prevClose = sc.Close[sc.Index - 1];
        const float tr1 = sc.High[sc.Index] - sc.Low[sc.Index];
        const float tr2 = fabsf(sc.High[sc.Index] - prevClose);
        const float tr3 = fabsf(sc.Low[sc.Index] - prevClose);
        range = max(tr1, max(tr2, tr3));
    }

    const float atr = SG_ATR[sc.Index];
    if (atr <= 0.0f)
        return;

    const float ratio = range / atr;

    // Format text
    SCString txt;
    txt.Format("%.*f", In_Decimals.GetInt(), ratio);

    // Draw the text centered on this bar at a constant Y value (row)
    s_UseTool Tool;
    Tool.Clear();
    Tool.ChartNumber = sc.ChartNumber;
    Tool.DrawingType = DRAWING_TEXT;
    Tool.Region      = sc.GraphRegion;
    Tool.AddMethod   = UTAM_ADD_OR_ADJUST;

    // Unique line number per bar so each bar has its own “cell”
    // (Use a big offset to avoid collisions with other studies)
    Tool.LineNumber  = 1000000 + sc.StudyGraphInstanceID * 200000 + sc.Index;

    Tool.BeginIndex  = sc.Index;
    Tool.BeginValue  = In_RowYValue.GetFloat();

    Tool.Color       = RGB(255, 255, 255);
    Tool.FontSize    = In_FontSize.GetInt();
    Tool.FontBold    = 0;

    // Center text on the bar
    Tool.TextAlignment = DT_CENTER | DT_VCENTER;

    Tool.Text = txt;

    sc.UseTool(Tool);
}
